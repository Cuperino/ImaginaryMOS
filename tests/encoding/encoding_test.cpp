// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Encoding engine (plan §3.4): UTF-8 ⇄ UCS-2 BE/LE, BOM handling, the
// out-of-BMP policy (R-4), and misconfiguration diagnostics.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <imos/encoding/encoding.h>

using namespace imos::encoding;

namespace {

std::vector<std::uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<std::uint8_t> out;
    for (const int v : values) {
        out.push_back(static_cast<std::uint8_t>(v));
    }
    return out;
}

}  // namespace

TEST(Encoding, AsciiRoundTripUcs2Be) {
    const std::string text = "<mos><keepAlive/></mos>";
    auto encoded = encodeFromUtf8(text, {});
    ASSERT_TRUE(encoded.ok());
    EXPECT_EQ(encoded.value().size(), text.size() * 2);
    EXPECT_EQ(encoded.value()[0], 0x00);
    EXPECT_EQ(encoded.value()[1], '<');

    auto decoded = decodeToUtf8(encoded.value(), {});
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value(), text);
}

TEST(Encoding, NonLatinBmpRoundTripBothOrders) {
    const std::string text = "Ñandú — 五時のニュース";  // BMP only
    for (const WireEncoding wire : {WireEncoding::Ucs2Be, WireEncoding::Ucs2Le}) {
        EncodeOptions encodeOptions;
        encodeOptions.encoding = wire;
        auto encoded = encodeFromUtf8(text, encodeOptions);
        ASSERT_TRUE(encoded.ok());
        DecodeOptions decodeOptions;
        decodeOptions.encoding = wire;
        auto decoded = decodeToUtf8(encoded.value(), decodeOptions);
        ASSERT_TRUE(decoded.ok());
        EXPECT_EQ(decoded.value(), text);
    }
}

TEST(Encoding, MatchingBomIsStrippedAndReported) {
    DecodeStats stats;
    auto decoded = decodeToUtf8(bytes({0xFE, 0xFF, 0x00, 'A'}), {}, &stats);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value(), "A");
    EXPECT_TRUE(stats.sawBom);
}

TEST(Encoding, OppositeBomNamesTheMisconfiguration) {
    auto decoded = decodeToUtf8(bytes({0xFF, 0xFE, 'A', 0x00}), {});
    ASSERT_FALSE(decoded.ok());
    EXPECT_EQ(decoded.error().code, imos::ErrorCode::EncodingError);
    EXPECT_NE(decoded.error().message.find("opposite endianness"), std::string::npos);
}

TEST(Encoding, OddByteCountIsAMisconfigurationDiagnostic) {
    auto decoded = decodeToUtf8(bytes({0x00, 'A', 0x00}), {});
    ASSERT_FALSE(decoded.ok());
    EXPECT_NE(decoded.error().message.find("misconfigured"), std::string::npos);
}

TEST(Encoding, SurrogatePairLeniencyOnReceive) {
    // U+1F600 as UTF-16 BE: D83D DE00 — strict UCS-2 forbids it, real peers
    // send it; decode accepts and counts it.
    DecodeStats stats;
    auto decoded = decodeToUtf8(bytes({0xD8, 0x3D, 0xDE, 0x00}), {}, &stats);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value(), "\xF0\x9F\x98\x80");
    EXPECT_EQ(stats.surrogatePairs, 1U);

    DecodeOptions strict;
    strict.acceptSurrogatePairs = false;
    EXPECT_FALSE(decodeToUtf8(bytes({0xD8, 0x3D, 0xDE, 0x00}), strict).ok());
    EXPECT_FALSE(decodeToUtf8(bytes({0xD8, 0x3D, 0x00, 'A'}), {}).ok());  // lone high
    EXPECT_FALSE(decodeToUtf8(bytes({0xDE, 0x00}), {}).ok());             // lone low
}

TEST(Encoding, NonBmpPolicyReplaceIsDefaultAndCounts) {
    EncodeStats stats;
    auto encoded = encodeFromUtf8("A\xF0\x9F\x98\x80Z", {}, &stats);  // A 😀 Z
    ASSERT_TRUE(encoded.ok());
    EXPECT_EQ(encoded.value(), bytes({0x00, 'A', 0xFF, 0xFD, 0x00, 'Z'}));
    EXPECT_EQ(stats.replacedNonBmp, 1U);
}

TEST(Encoding, NonBmpPolicyRejectAndSurrogatePair) {
    EncodeOptions reject;
    reject.nonBmp = NonBmpPolicy::Reject;
    EXPECT_FALSE(encodeFromUtf8("\xF0\x9F\x98\x80", reject).ok());

    EncodeOptions pairs;
    pairs.nonBmp = NonBmpPolicy::SurrogatePair;
    auto encoded = encodeFromUtf8("\xF0\x9F\x98\x80", pairs);
    ASSERT_TRUE(encoded.ok());
    EXPECT_EQ(encoded.value(), bytes({0xD8, 0x3D, 0xDE, 0x00}));
}

TEST(Encoding, Utf8PassthroughValidatesAndHandlesBom) {
    DecodeOptions utf8In;
    utf8In.encoding = WireEncoding::Utf8;
    DecodeStats stats;
    auto decoded = decodeToUtf8(bytes({0xEF, 0xBB, 0xBF, 'h', 'i'}), utf8In, &stats);
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value(), "hi");
    EXPECT_TRUE(stats.sawBom);

    auto bad = decodeToUtf8(bytes({'a', 0xC0, 0xAF}), utf8In);  // overlong '/'
    ASSERT_FALSE(bad.ok());
    EXPECT_NE(bad.error().message.find("UCS-2"), std::string::npos);
}

TEST(Encoding, InvalidUtf8InputRejectedOnEncode) {
    EXPECT_FALSE(encodeFromUtf8("\xED\xA0\x80", {}).ok());      // encoded surrogate
    EXPECT_FALSE(encodeFromUtf8("\xF4\x90\x80\x80", {}).ok());  // > U+10FFFF
    EXPECT_FALSE(encodeFromUtf8("\x80", {}).ok());
    EXPECT_EQ(findInvalidUtf8("ok \xC3\xB1"), std::string_view::npos);
    EXPECT_EQ(findInvalidUtf8("ab\xFF"), 2U);
}

TEST(Encoding, BomCanBeWrittenWhenDialectWantsIt) {
    EncodeOptions withBom;
    withBom.writeBom = true;
    auto encoded = encodeFromUtf8("A", withBom);
    ASSERT_TRUE(encoded.ok());
    EXPECT_EQ(encoded.value(), bytes({0xFE, 0xFF, 0x00, 'A'}));
}
