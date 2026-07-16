// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// storyBody helper coverage (Profile 4, plan §4.5): document-order
// paragraph-run iteration over the spec's roStorySend example (run
// boundaries, inline vocabulary, unknown tags as Raw), read-only behavior
// (byte-clean re-serialization after iteration), Read1stMEMasBody
// resolution (literal vs agency, pointer identity — never a copy), and the
// two illustrative roStorySend fixtures pinned as parser leniency probes:
// the production-lenient policy still requires well-formedness, so the
// spec's own broken fragments fail with these exact diagnostics.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/message.h>
#include <imos/codec/story_body.h>
#include <imos/codec/xml.h>

namespace fs = std::filesystem;
using imos::ErrorCode;
using imos::codec::Message;
using imos::codec::ParagraphRuns;
using imos::codec::parseMessage;
using imos::codec::ResolvedStoryBody;
using imos::codec::resolveStoryBody;
using imos::codec::serializeMessage;
using imos::codec::serializeXmlNode;
using imos::codec::XmlNode;
using Kind = imos::codec::ParagraphRuns::Kind;

namespace {

std::string readFixture(const char* name) {
    const fs::path path = fs::path{IMOS_FIXTURES_DIR} / "spec-examples" / name;
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << path;
    std::ostringstream out;
    out << in.rdbuf();
    return std::move(out).str();
}

std::vector<Kind> kindsOf(const ParagraphRuns& runs) {
    std::vector<Kind> kinds;
    for (const ParagraphRuns::Run& run : runs) {
        kinds.push_back(run.kind);
    }
    return kinds;
}

}  // namespace

// 1. The spec example walks in document order: the first <p> is whitespace
//    around one presenter instruction, the prose paragraphs are single text
//    runs, and iteration never mutates the tree (byte-clean re-emit).
TEST(StoryBody, ParagraphRunsWalkTheSpecExampleInDocumentOrder) {
    const std::string raw = readFixture("roStorySend.xml");
    auto parsed = parseMessage(raw);
    ASSERT_TRUE(parsed.ok());
    Message& message = parsed.value();

    auto resolved = resolveStoryBody(message);
    ASSERT_TRUE(resolved.ok());
    ASSERT_NE(resolved.value().storyBody, nullptr);
    EXPECT_FALSE(resolved.value().bodyIsAgencyPayload());
    XmlNode* storyBody = message.body()->firstChildElement("storyBody");
    ASSERT_EQ(storyBody, resolved.value().storyBody);
    ASSERT_EQ(storyBody->countChildElements("p"), 5U);

    // <p>\n            <pi> Smile </pi>\n         </p>
    const ParagraphRuns first{*storyBody->childElementAt("p", 0)};
    ASSERT_EQ(first.size(), 3U);
    EXPECT_EQ(first[0].kind, Kind::Text);
    EXPECT_EQ(first[1].kind, Kind::Pi);
    EXPECT_EQ(first[1].text, " Smile ");
    EXPECT_EQ(first[2].kind, Kind::Text);

    // <p> Good Evening, I'm Suzie Humpries </p> — one untrimmed text run.
    const ParagraphRuns second{*storyBody->childElementAt("p", 1)};
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(second[0].kind, Kind::Text);
    EXPECT_EQ(second[0].text, " Good Evening, I'm Suzie Humpries ");

    // Iteration is read-only: the message still re-serializes byte-cleanly.
    EXPECT_EQ(serializeMessage(message), raw);
}

// 2. Mixed content with the full inline vocabulary plus a vendor tag: every
//    child is exactly one run, unknown markup surfaces as Raw, and the Raw
//    node re-serializes verbatim.
TEST(StoryBody, UnknownInlineMarkupSurfacesAsRawRuns) {
    const std::string xml =
        "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>1</messageID>"
        "<roStorySend><roID>RO</roID><storyID>S1</storyID>"
        "<storyBody><p>before<vendor a=\"1\">v<x/></vendor><em>hot</em>"
        "<tab/> after<pi>cue</pi></p></storyBody></roStorySend></mos>";
    auto parsed = parseMessage(xml);
    ASSERT_TRUE(parsed.ok());

    XmlNode* p = parsed.value().body()->firstChildElement("storyBody")->firstChildElement("p");
    ASSERT_NE(p, nullptr);
    const ParagraphRuns runs{*p};
    EXPECT_EQ(kindsOf(runs), (std::vector<Kind>{Kind::Text, Kind::Raw, Kind::Em, Kind::Tab,
                                                Kind::Text, Kind::Pi}));
    EXPECT_EQ(runs[0].text, "before");
    EXPECT_EQ(runs[1].text, "");  // Raw content is read through the node
    EXPECT_EQ(serializeXmlNode(*runs[1].node), "<vendor a=\"1\">v<x/></vendor>");
    EXPECT_EQ(runs[2].text, "hot");
    EXPECT_EQ(runs[3].text, "");
    EXPECT_EQ(runs[4].text, " after");
    EXPECT_EQ(runs[5].text, "cue");

    EXPECT_EQ(serializeMessage(parsed.value()), xml);
}

// 3. Read1stMEMasBody="true": the body resolves to the roStorySend's first
//    MEM block — the very node in the tree, never a re-serialized copy.
TEST(StoryBody, Read1stMEMasBodyResolvesToTheFirstMemBlockByReference) {
    const std::string xml =
        "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>1</messageID>"
        "<roStorySend><roID>RO</roID><storyID>S1</storyID>"
        "<storyBody Read1stMEMasBody=\"true\"></storyBody>"
        "<mosExternalMetadata><mosScope>STORY</mosScope>"
        "<mosSchema>http://agency.example/newsml</mosSchema>"
        "<mosPayload><NewsML><NewsItem>agency copy</NewsItem></NewsML></mosPayload>"
        "</mosExternalMetadata>"
        "<mosExternalMetadata><mosScope>PLAYLIST</mosScope></mosExternalMetadata>"
        "</roStorySend></mos>";
    auto parsed = parseMessage(xml);
    ASSERT_TRUE(parsed.ok());

    auto resolved = resolveStoryBody(parsed.value());
    ASSERT_TRUE(resolved.ok());
    EXPECT_TRUE(resolved.value().bodyIsAgencyPayload());
    // Pointer identity into the message's own tree (the *first* MEM).
    EXPECT_EQ(resolved.value().agencyMetadata,
              parsed.value().body()->firstChildElement("mosExternalMetadata"));
    ASSERT_NE(resolved.value().agencyMetadata->firstChildElement("mosPayload"), nullptr);

    // A false attribute means the literal paragraphs are the body.
    parsed.value()
        .body()
        ->firstChildElement("storyBody")
        ->setAttribute("Read1stMEMasBody", "false");
    auto literal = resolveStoryBody(parsed.value());
    ASSERT_TRUE(literal.ok());
    EXPECT_FALSE(literal.value().bodyIsAgencyPayload());
}

// 4. Resolution failure modes: not a roStorySend, no storyBody at all, and
//    an agency promise without any MEM block to fulfil it.
TEST(StoryBody, ResolveStoryBodyRejectsMalformedShapes) {
    auto heartbeat = parseMessage(
        "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>1</messageID>"
        "<heartbeat/></mos>");
    ASSERT_TRUE(heartbeat.ok());
    EXPECT_EQ(resolveStoryBody(heartbeat.value()).error().code, ErrorCode::InvalidArgument);

    auto noBody = parseMessage(
        "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>1</messageID>"
        "<roStorySend><roID>RO</roID><storyID>S1</storyID></roStorySend></mos>");
    ASSERT_TRUE(noBody.ok());
    EXPECT_EQ(resolveStoryBody(noBody.value()).error().code, ErrorCode::ProtocolViolation);

    auto noMem = parseMessage(
        "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>1</messageID>"
        "<roStorySend><roID>RO</roID><storyID>S1</storyID>"
        "<storyBody Read1stMEMasBody=\"true\"/></roStorySend></mos>");
    ASSERT_TRUE(noMem.ok());
    const auto resolved = resolveStoryBody(noMem.value());
    ASSERT_FALSE(resolved.ok());
    EXPECT_EQ(resolved.error().code, ErrorCode::ProtocolViolation);
}

// 5. Leniency probes: the spec's own non-well-formed roStorySend fragments.
//    Production leniency forgives invalidity, not ill-formedness — both are
//    rejected, with these diagnostics pinned as the decision of record.
TEST(StoryBody, IllustrativeFixturesStayRejectedAsNotWellFormed) {
    const auto embeddedDecl = parseMessage(readFixture("roStorySend-2.illustrative.xml"));
    ASSERT_FALSE(embeddedDecl.ok());
    EXPECT_EQ(embeddedDecl.error().code, ErrorCode::ParseError);
    EXPECT_NE(embeddedDecl.error().message.find("XML declaration"), std::string::npos)
        << embeddedDecl.error().message;

    const auto tagMismatch = parseMessage(readFixture("roStorySend-3.illustrative.xml"));
    ASSERT_FALSE(tagMismatch.ok());
    EXPECT_EQ(tagMismatch.error().code, ErrorCode::ParseError);
    EXPECT_NE(tagMismatch.error().message.find("tag mismatch"), std::string::npos)
        << tagMismatch.error().message;
}
