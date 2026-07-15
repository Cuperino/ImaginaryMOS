// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// The M1 exit gate (plan §13): every spec example round-trips byte-cleanly
// through the codec, modulo the declared whitespace policy. Policy, exactly:
// whitespace INSIDE start/end tags (between attributes, around '=', before
// '>' or '/>') is normalized to the canonical single-space form. Nothing
// else may change — element content, attribute values, comments, inter-tag
// whitespace, and the empty-element form (<tag/> vs <tag></tag>) must all
// re-emit byte-identically. Sources that are already canonical inside their
// tags must round-trip exactly.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/message.h>
#include <imos/codec/xml.h>

namespace fs = std::filesystem;
using imos::codec::Message;
using imos::codec::parseMessage;
using imos::codec::serializeMessage;

namespace {

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream out;
    out << in.rdbuf();
    return std::move(out).str();
}

bool isXmlWs(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// Normalize whitespace inside element start/end tags only (the declared
/// round-trip whitespace policy). Comments, PIs, CDATA, and character data
/// pass through verbatim.
std::string normalizeTagWhitespace(const std::string& xml) {
    std::string out;
    out.reserve(xml.size());
    std::size_t i = 0;
    const std::size_t n = xml.size();
    while (i < n) {
        // Pass constructs whose interior is not a tag.
        if (xml.compare(i, 4, "<!--") == 0) {
            const std::size_t end = xml.find("-->", i + 4);
            const std::size_t stop = end == std::string::npos ? n : end + 3;
            out.append(xml, i, stop - i);
            i = stop;
            continue;
        }
        if (xml.compare(i, 9, "<![CDATA[") == 0) {
            const std::size_t end = xml.find("]]>", i + 9);
            const std::size_t stop = end == std::string::npos ? n : end + 3;
            out.append(xml, i, stop - i);
            i = stop;
            continue;
        }
        if (xml.compare(i, 2, "<?") == 0) {
            const std::size_t end = xml.find("?>", i + 2);
            const std::size_t stop = end == std::string::npos ? n : end + 2;
            out.append(xml, i, stop - i);
            i = stop;
            continue;
        }
        if (xml[i] != '<') {
            out += xml[i++];
            continue;
        }
        // Element tag: normalize interior whitespace outside quotes.
        std::string tag;
        bool pendingSpace = false;
        char quote = 0;
        tag += xml[i++];
        while (i < n) {
            const char c = xml[i];
            if (quote != 0) {
                tag += c;
                if (c == quote) {
                    quote = 0;
                }
                ++i;
                continue;
            }
            if (c == '"' || c == '\'') {
                pendingSpace = false;  // quotes only follow '=' inside a tag
                quote = c;
                tag += c;
                ++i;
                continue;
            }
            if (isXmlWs(c)) {
                pendingSpace = true;
                ++i;
                continue;
            }
            if (c == '=' || c == '>' || (c == '/' && i + 1 < n && xml[i + 1] == '>')) {
                pendingSpace = false;  // no space around '=', before '>' or '/>'
            } else if (pendingSpace) {
                tag += ' ';
                pendingSpace = false;
            }
            tag += c;
            ++i;
            if (c == '>') {
                break;
            }
        }
        out += tag;
    }
    return out;
}

class SpecExampleRoundTrip : public ::testing::TestWithParam<std::string> {};

fs::path fixturesDir() {
    return fs::path{IMOS_FIXTURES_DIR};
}

std::vector<std::string> collectExamples() {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(fixturesDir() / "spec-examples")) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() == ".xml" &&
            name.find(".illustrative.") == std::string::npos) {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace

TEST(SpecExamples, InventoryFloor) {
    // Guards against silently losing fixtures (59 well-formed at extraction).
    EXPECT_GE(collectExamples().size(), 50U);
}

TEST_P(SpecExampleRoundTrip, ByteClean) {
    const std::string original = readFile(fixturesDir() / "spec-examples" / GetParam());
    ASSERT_FALSE(original.empty());

    imos::Result<Message> parsed = parseMessage(original);
    ASSERT_TRUE(parsed.ok()) << parsed.error().message;
    EXPECT_NE(parsed.value().type(), imos::codec::MessageType::Unknown)
        << "unrecognized message element in " << GetParam();

    const std::string serialized = serializeMessage(parsed.value());
    EXPECT_EQ(normalizeTagWhitespace(original), normalizeTagWhitespace(serialized))
        << "round-trip diverged for " << GetParam();

    // Already-canonical sources must round-trip exactly, byte for byte.
    if (normalizeTagWhitespace(original) == original) {
        EXPECT_EQ(original, serialized) << "byte-clean round-trip failed for " << GetParam();
    }

    // Serialization must be a fixed point: parse(serialize(x)) == serialize(x).
    imos::Result<Message> reparsed = parseMessage(serialized);
    ASSERT_TRUE(reparsed.ok()) << reparsed.error().message;
    EXPECT_EQ(serialized, serializeMessage(reparsed.value()));
}

INSTANTIATE_TEST_SUITE_P(AllSpecExamples, SpecExampleRoundTrip,
                         ::testing::ValuesIn(collectExamples()),
                         [](const ::testing::TestParamInfo<std::string>& paramInfo) {
                             std::string name = paramInfo.param;
                             for (char& c : name) {
                                 if (!std::isalnum(static_cast<unsigned char>(c))) {
                                     c = '_';
                                 }
                             }
                             return name;
                         });

TEST(UnknownContentRetention, VendorExtensionsSurvive) {
    const std::string original =
        "<mos>\n"
        "  <mosID>a.b.c</mosID>\n"
        "  <ncsID>x.y.z</ncsID>\n"
        "  <messageID>7</messageID>\n"
        "  <mosObj>\n"
        "    <objID>M1</objID>\n"
        "    <vendorExtra flavor=\"secret\"><nested>keep me</nested></vendorExtra>\n"
        "    <objSlug>slug</objSlug>\n"
        "    <emptyKept/>\n"
        "    <alsoKept></alsoKept>\n"
        "  </mosObj>\n"
        "</mos>\n";
    imos::Result<Message> parsed = parseMessage(original);
    ASSERT_TRUE(parsed.ok()) << parsed.error().message;
    EXPECT_EQ(serializeMessage(parsed.value()), original);
}

TEST(UnknownContentRetention, MosPayloadStaysOpaque) {
    const std::string original =
        "<mos><mosID>a</mosID><ncsID>b</ncsID><messageID>1</messageID>"
        "<roStorySend><roID>R</roID><storyID>S</storyID>"
        "<storyBody Read1stMEMasBody=\"true\"/>"
        "<mosExternalMetadata><mosSchema>urn:x</mosSchema>"
        "<mosPayload><NewsML Duid=\"d1\"><Weird space=\"  kept  \">text</Weird>"
        "<!-- comment kept --></NewsML></mosPayload>"
        "</mosExternalMetadata></roStorySend></mos>";
    imos::Result<Message> parsed = parseMessage(original);
    ASSERT_TRUE(parsed.ok()) << parsed.error().message;
    // Opaque payloads are never re-normalized: byte-identical re-emission,
    // including the attribute value's inner spaces and the comment.
    EXPECT_EQ(serializeMessage(parsed.value()), original);
}

TEST(ParseHardening, RejectsDoctypeAndOversizeAndDepth) {
    EXPECT_FALSE(parseMessage("<!DOCTYPE mos [<!ENTITY x \"y\">]><mos><keepAlive/></mos>").ok());

    imos::codec::XmlParseLimits tiny;
    tiny.maxDocumentBytes = 16;
    EXPECT_FALSE(parseMessage("<mos><keepAlive/></mos>", tiny).ok());

    std::string deep = "<mos>";
    for (int i = 0; i < 100; ++i) {
        deep += "<a>";
    }
    for (int i = 0; i < 100; ++i) {
        deep += "</a>";
    }
    deep += "</mos>";
    EXPECT_FALSE(parseMessage(deep).ok());
}
