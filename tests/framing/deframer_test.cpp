// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// 2.8 stream deframer spike: tokenizing <mos> documents out of a raw byte
// stream with resynchronization and the hard size cap (plan §5.2/§12).

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <imos/encoding/encoding.h>
#include <imos/framing/deframer.h>

using imos::encoding::EncodeOptions;
using imos::encoding::WireEncoding;
using imos::framing::DeframerOptions;
using imos::framing::Frame;
using imos::framing::StreamDeframer;

namespace {

std::vector<std::uint8_t> encode(const std::string& text, WireEncoding wire) {
    EncodeOptions options;
    options.encoding = wire;
    auto encoded = imos::encoding::encodeFromUtf8(text, options);
    EXPECT_TRUE(encoded.ok());
    return std::move(encoded).value();
}

std::string decode(const std::vector<std::uint8_t>& wireBytes, WireEncoding wire) {
    imos::encoding::DecodeOptions options;
    options.encoding = wire;
    auto decoded = imos::encoding::decodeToUtf8(wireBytes, options);
    EXPECT_TRUE(decoded.ok());
    return std::move(decoded).value();
}

constexpr const char* kMsgA = "<mos><mosID>a</mosID><ncsID>b</ncsID><keepAlive/></mos>";
// Contains "</mosAck>" — a "</mos" prefix that must NOT terminate the frame.
constexpr const char* kMsgB =
    "<mos><mosID>a</mosID><ncsID>b</ncsID><messageID>1</messageID>"
    "<mosAck><objID>M1</objID><objRev>1</objRev><status>ACK</status>"
    "<statusDescription/></mosAck></mos>";

}  // namespace

TEST(Deframer, SplitsBackToBackMessagesAcrossArbitraryChunks) {
    for (const WireEncoding wire :
         {WireEncoding::Ucs2Be, WireEncoding::Ucs2Le, WireEncoding::Utf8}) {
        DeframerOptions options;
        options.encoding = wire;
        StreamDeframer deframer{options};

        std::vector<std::uint8_t> stream = encode(std::string{kMsgA} + "\r\n" + kMsgB, wire);
        // Feed in awkward 3-byte chunks (odd size wrecks any naive
        // unit-aligned scanner).
        std::vector<std::string> documents;
        for (std::size_t i = 0; i < stream.size(); i += 3) {
            const std::size_t len = std::min<std::size_t>(3, stream.size() - i);
            deframer.feed(std::span<const std::uint8_t>{stream.data() + i, len});
            while (auto frame = deframer.next()) {
                documents.push_back(decode(frame->document, wire));
                EXPECT_EQ(frame->discardedBefore, 0U);
            }
        }
        ASSERT_EQ(documents.size(), 2U) << wireEncodingName(wire);
        EXPECT_EQ(documents[0], kMsgA);
        EXPECT_EQ(documents[1], kMsgB);
        EXPECT_EQ(deframer.totalDiscarded(), 0U);
    }
}

TEST(Deframer, ResynchronizesPastGarbage) {
    StreamDeframer deframer;
    std::vector<std::uint8_t> garbage = {0xDE, 0xAD, 0xBE, 0xEF, 0x00};  // odd length too
    deframer.feed(garbage);
    deframer.feed(encode(kMsgA, WireEncoding::Ucs2Be));

    auto frame = deframer.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(decode(frame->document, WireEncoding::Ucs2Be), kMsgA);
    EXPECT_EQ(frame->discardedBefore, garbage.size());
    EXPECT_EQ(deframer.totalDiscarded(), garbage.size());
}

TEST(Deframer, FalseCloseTagPrefixDoesNotEndTheFrame) {
    // "<mosID>" after "<mos" and "</mosAck>" after "</mos" both require the
    // name-boundary check to be ignored.
    StreamDeframer deframer;
    deframer.feed(encode(kMsgB, WireEncoding::Ucs2Be));
    auto frame = deframer.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(decode(frame->document, WireEncoding::Ucs2Be), kMsgB);
}

TEST(Deframer, WhitespaceInsideCloseTagTolerated) {
    StreamDeframer deframer;
    deframer.feed(
        encode("<mos><mosID>a</mosID><ncsID>b</ncsID><keepAlive/></mos >", WireEncoding::Ucs2Be));
    auto frame = deframer.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_TRUE(decode(frame->document, WireEncoding::Ucs2Be).ends_with("</mos >"));
}

TEST(Deframer, OverCapCandidateIsDiscardedAndStreamRecovers) {
    DeframerOptions options;
    options.maxMessageBytes = 256;
    StreamDeframer deframer{options};

    // An unterminated <mos that balloons past the cap...
    std::string runaway = "<mos><mosID>evil</mosID>";
    runaway.append(600, 'x');
    deframer.feed(encode(runaway, WireEncoding::Ucs2Be));
    EXPECT_FALSE(deframer.next().has_value());
    EXPECT_GE(deframer.overflowCount(), 1U);

    // ...must not stop the next legitimate message from parsing out.
    deframer.feed(encode(kMsgA, WireEncoding::Ucs2Be));
    auto frame = deframer.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(decode(frame->document, WireEncoding::Ucs2Be), kMsgA);
}

TEST(Deframer, InterMessageWhitespaceIsNotCountedAsGarbage) {
    StreamDeframer deframer;
    deframer.feed(encode("\r\n  " + std::string{kMsgA}, WireEncoding::Ucs2Be));
    auto frame = deframer.next();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->discardedBefore, 0U);
    EXPECT_EQ(deframer.totalDiscarded(), 0U);
}
