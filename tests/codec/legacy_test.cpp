// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Legacy-13 normalization (plan §3.2): every removed message parses and
// rewrites onto roElementAction / roElementStat with the target/source
// shapes the spec's own roElementAction examples document.

#include <string>

#include <gtest/gtest.h>

#include <imos/codec/legacy.h>
#include <imos/codec/message.h>

using imos::codec::Message;
using imos::codec::MessageType;
using imos::codec::normalizeLegacy;
using imos::codec::parseMessage;
using imos::codec::serializeMessage;

namespace {

Message parseLegacy(const std::string& body) {
    const std::string xml =
        "<mos><mosID>m.x.mos</mosID><ncsID>n.x.mos</ncsID>"
        "<messageID>77</messageID>" +
        body + "</mos>";
    imos::Result<Message> parsed = parseMessage(xml);
    EXPECT_TRUE(parsed.ok()) << parsed.error().message;
    return std::move(parsed).value();
}

std::string normalizedBody(const std::string& legacyBody) {
    imos::Result<Message> normalized = normalizeLegacy(parseLegacy(legacyBody));
    EXPECT_TRUE(normalized.ok()) << normalized.error().message;
    if (!normalized.ok()) {
        return {};
    }
    EXPECT_EQ(normalized.value().mosID().value_or(""), "m.x.mos");
    EXPECT_EQ(normalized.value().messageID().value_or(""), "77");
    return serializeXmlNode(*normalized.value().body());
}

using imos::codec::serializeXmlNode;

}  // namespace

TEST(LegacyNormalization, RoStoryAppendBecomesInsertAtEnd) {
    EXPECT_EQ(normalizedBody("<roStoryAppend><roID>R</roID>"
                             "<story><storyID>S9</storyID></story></roStoryAppend>"),
              "<roElementAction operation=\"INSERT\"><roID>R</roID>"
              "<element_source><story><storyID>S9</storyID></story></element_source>"
              "</roElementAction>");
}

TEST(LegacyNormalization, RoStoryInsertTargetsTheNamedStory) {
    EXPECT_EQ(normalizedBody("<roStoryInsert><roID>R</roID><storyID>S2</storyID>"
                             "<story><storyID>S9</storyID></story></roStoryInsert>"),
              "<roElementAction operation=\"INSERT\"><roID>R</roID>"
              "<element_target><storyID>S2</storyID></element_target>"
              "<element_source><story><storyID>S9</storyID></story></element_source>"
              "</roElementAction>");
}

TEST(LegacyNormalization, RoStoryReplace) {
    EXPECT_EQ(normalizedBody("<roStoryReplace><roID>R</roID><storyID>S2</storyID>"
                             "<story><storyID>S9</storyID></story></roStoryReplace>"),
              "<roElementAction operation=\"REPLACE\"><roID>R</roID>"
              "<element_target><storyID>S2</storyID></element_target>"
              "<element_source><story><storyID>S9</storyID></story></element_source>"
              "</roElementAction>");
}

TEST(LegacyNormalization, RoStoryMoveSourceThenTarget) {
    EXPECT_EQ(normalizedBody("<roStoryMove><roID>R</roID><storyID>S7</storyID>"
                             "<storyID>S2</storyID></roStoryMove>"),
              "<roElementAction operation=\"MOVE\"><roID>R</roID>"
              "<element_target><storyID>S2</storyID></element_target>"
              "<element_source><storyID>S7</storyID></element_source>"
              "</roElementAction>");
}

TEST(LegacyNormalization, RoStoryMoveMultipleLastIsTarget) {
    EXPECT_EQ(normalizedBody("<roStoryMoveMultiple><roID>R</roID><storyID>A</storyID>"
                             "<storyID>B</storyID><storyID>T</storyID>"
                             "</roStoryMoveMultiple>"),
              "<roElementAction operation=\"MOVE\"><roID>R</roID>"
              "<element_target><storyID>T</storyID></element_target>"
              "<element_source><storyID>A</storyID><storyID>B</storyID></element_source>"
              "</roElementAction>");
}

TEST(LegacyNormalization, RoStorySwapHasNoTarget) {
    EXPECT_EQ(normalizedBody("<roStorySwap><roID>R</roID><storyID>S3</storyID>"
                             "<storyID>S5</storyID></roStorySwap>"),
              "<roElementAction operation=\"SWAP\"><roID>R</roID>"
              "<element_source><storyID>S3</storyID><storyID>S5</storyID></element_source>"
              "</roElementAction>");
}

TEST(LegacyNormalization, RoStoryDeleteHasNoTarget) {
    EXPECT_EQ(normalizedBody("<roStoryDelete><roID>R</roID><storyID>S3</storyID>"
                             "<storyID>S5</storyID></roStoryDelete>"),
              "<roElementAction operation=\"DELETE\"><roID>R</roID>"
              "<element_source><storyID>S3</storyID><storyID>S5</storyID></element_source>"
              "</roElementAction>");
}

TEST(LegacyNormalization, RoItemInsertTargetsStoryAndSlot) {
    EXPECT_EQ(normalizedBody("<roItemInsert><roID>R</roID><storyID>S2</storyID>"
                             "<itemID>I4</itemID><item><itemID>I9</itemID></item>"
                             "</roItemInsert>"),
              "<roElementAction operation=\"INSERT\"><roID>R</roID>"
              "<element_target><storyID>S2</storyID><itemID>I4</itemID></element_target>"
              "<element_source><item><itemID>I9</itemID></item></element_source>"
              "</roElementAction>");
}

TEST(LegacyNormalization, RoItemInsertWithEmptySlotAppends) {
    // Empty itemID = append at the end of the story (2.8 semantics).
    EXPECT_EQ(normalizedBody("<roItemInsert><roID>R</roID><storyID>S2</storyID>"
                             "<itemID></itemID><item><itemID>I9</itemID></item>"
                             "</roItemInsert>"),
              "<roElementAction operation=\"INSERT\"><roID>R</roID>"
              "<element_target><storyID>S2</storyID><itemID/></element_target>"
              "<element_source><item><itemID>I9</itemID></item></element_source>"
              "</roElementAction>");
}

TEST(LegacyNormalization, RoItemReplace) {
    EXPECT_EQ(normalizedBody("<roItemReplace><roID>R</roID><storyID>S2</storyID>"
                             "<itemID>I4</itemID><item><itemID>I9</itemID></item>"
                             "</roItemReplace>"),
              "<roElementAction operation=\"REPLACE\"><roID>R</roID>"
              "<element_target><storyID>S2</storyID><itemID>I4</itemID></element_target>"
              "<element_source><item><itemID>I9</itemID></item></element_source>"
              "</roElementAction>");
}

TEST(LegacyNormalization, RoItemMoveMultipleLastIsTarget) {
    EXPECT_EQ(normalizedBody("<roItemMoveMultiple><roID>R</roID><storyID>S2</storyID>"
                             "<itemID>I1</itemID><itemID>I2</itemID><itemID>I9</itemID>"
                             "</roItemMoveMultiple>"),
              "<roElementAction operation=\"MOVE\"><roID>R</roID>"
              "<element_target><storyID>S2</storyID><itemID>I9</itemID></element_target>"
              "<element_source><itemID>I1</itemID><itemID>I2</itemID></element_source>"
              "</roElementAction>");
}

TEST(LegacyNormalization, RoItemDeleteTargetsStoryOnly) {
    EXPECT_EQ(normalizedBody("<roItemDelete><roID>R</roID><storyID>S2</storyID>"
                             "<itemID>I1</itemID><itemID>I2</itemID></roItemDelete>"),
              "<roElementAction operation=\"DELETE\"><roID>R</roID>"
              "<element_target><storyID>S2</storyID></element_target>"
              "<element_source><itemID>I1</itemID><itemID>I2</itemID></element_source>"
              "</roElementAction>");
}

TEST(LegacyNormalization, RoStatBecomesRoScopedElementStat) {
    EXPECT_EQ(normalizedBody("<roStat><roID>R</roID><status>PLAY</status>"
                             "<time>2009-04-11T14:13:53</time></roStat>"),
              "<roElementStat element=\"RO\"><roID>R</roID><status>PLAY</status>"
              "<time>2009-04-11T14:13:53</time></roElementStat>");
}

TEST(LegacyNormalization, RoItemStatBecomesItemScopedElementStat) {
    EXPECT_EQ(normalizedBody("<roItemStat><roID>R</roID><storyID>S</storyID>"
                             "<itemID>I</itemID><objID>O</objID><itemChannel>B</itemChannel>"
                             "<status>PLAY</status><time>T</time></roItemStat>"),
              "<roElementStat element=\"ITEM\"><roID>R</roID><storyID>S</storyID>"
              "<itemID>I</itemID><objID>O</objID><itemChannel>B</itemChannel>"
              "<status>PLAY</status><time>T</time></roElementStat>");
}

TEST(LegacyNormalization, MetadataRidesAlongVerbatim) {
    const std::string body = normalizedBody(
        "<roStoryInsert><roID>R</roID><storyID>S2</storyID>"
        "<story><storyID>S9</storyID><mosExternalMetadata><mosSchema>urn:v</mosSchema>"
        "<mosPayload><vendor a=\"1\">x</vendor></mosPayload></mosExternalMetadata>"
        "</story></roStoryInsert>");
    EXPECT_NE(body.find("<mosPayload><vendor a=\"1\">x</vendor></mosPayload>"), std::string::npos);
}

TEST(LegacyNormalization, RejectsNonLegacyAndUnusableShapes) {
    EXPECT_FALSE(normalizeLegacy(parseLegacy("<roCreate><roID>R</roID></roCreate>")).ok());
    EXPECT_FALSE(normalizeLegacy(parseLegacy("<roStoryMove><roID>R</roID><storyID>only</storyID>"
                                             "</roStoryMove>"))
                     .ok());
    EXPECT_FALSE(
        normalizeLegacy(parseLegacy("<roStoryAppend><roID>R</roID></roStoryAppend>")).ok());
}
