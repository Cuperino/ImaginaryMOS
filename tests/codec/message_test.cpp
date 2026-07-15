// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Message model: envelope shapes, the generated registry metadata, the
// generated typed views, and canonical construction via makeMessage.

#include <string>

#include <gtest/gtest.h>

#include <imos/codec/gen/views.h>
#include <imos/codec/message.h>

using imos::Channel;
using imos::ChannelAffinity;
using imos::EnvelopeKind;
using imos::codec::makeMessage;
using imos::codec::Message;
using imos::codec::messageInfo;
using imos::codec::MessageType;
using imos::codec::messageTypeFromName;
using imos::codec::parseMessage;
using imos::codec::serializeMessage;

TEST(MessageRegistry, WireNameLookupIsExactAndComplete) {
    EXPECT_EQ(messageTypeFromName("roElementAction"), MessageType::RoElementAction);
    EXPECT_EQ(messageTypeFromName("mosReqObjList"), MessageType::MosReqObjList);
    EXPECT_EQ(messageTypeFromName("roelementaction"), MessageType::Unknown);
    EXPECT_EQ(messageTypeFromName(""), MessageType::Unknown);
    // 45 network messages (32 current + 13 legacy) + 9 plug-in + Unknown.
    EXPECT_EQ(imos::codec::kMessageTypeCount, 55U);
}

TEST(MessageRegistry, ChannelAffinitiesFollowThePortAnnotations) {
    EXPECT_EQ(messageInfo(MessageType::MosObj).channel, ChannelAffinity::Mom);
    EXPECT_EQ(messageInfo(MessageType::MosItemReplace).channel, ChannelAffinity::Ro);
    EXPECT_EQ(messageInfo(MessageType::MosReqObjList).channel, ChannelAffinity::Aux);
    EXPECT_EQ(messageInfo(MessageType::RoElementStat).channel, ChannelAffinity::Ro);
    EXPECT_EQ(messageInfo(MessageType::KeepAlive).channel, ChannelAffinity::AnyChannel);
    EXPECT_EQ(messageInfo(MessageType::NcsAck).channel, ChannelAffinity::NotNetwork);
}

TEST(MessageRegistry, ResponsePairingAndFlags) {
    const auto& roCreate = messageInfo(MessageType::RoCreate);
    ASSERT_EQ(roCreate.responses.size(), 1U);
    EXPECT_EQ(roCreate.responses[0], MessageType::RoAck);
    EXPECT_EQ(roCreate.profile, 2);

    const auto& roReq = messageInfo(MessageType::RoReq);
    ASSERT_EQ(roReq.responses.size(), 2U);
    EXPECT_EQ(roReq.responses[0], MessageType::RoList);

    EXPECT_TRUE(messageInfo(MessageType::MosAck).isAck);
    EXPECT_TRUE(messageInfo(MessageType::RoStorySwap).legacyRemoved);
    EXPECT_TRUE(messageInfo(MessageType::NcsItem).plugin);
    EXPECT_EQ(messageInfo(MessageType::KeepAlive).envelope, EnvelopeKind::IdsOnly);
    EXPECT_EQ(messageInfo(MessageType::NcsAck).envelope, EnvelopeKind::None);
    EXPECT_EQ(messageInfo(MessageType::Heartbeat).envelope, EnvelopeKind::Full);
    EXPECT_EQ(messageInfo(MessageType::RoStat).profile, -1);
}

TEST(MessageEnvelope, LenientAcrossShapes) {
    // keepAlive: mosID/ncsID, no messageID (spec §4.1.1).
    imos::Result<Message> keepAlive =
        parseMessage("<mos><mosID>m</mosID><ncsID>n</ncsID><keepAlive/></mos>");
    ASSERT_TRUE(keepAlive.ok());
    EXPECT_EQ(keepAlive.value().type(), MessageType::KeepAlive);
    EXPECT_EQ(keepAlive.value().mosID().value_or(""), "m");
    EXPECT_FALSE(keepAlive.value().messageID().has_value());

    // Plug-in family: bare wrapper.
    imos::Result<Message> ncsAck = parseMessage("<mos><ncsAck><status>ACK</status></ncsAck></mos>");
    ASSERT_TRUE(ncsAck.ok());
    EXPECT_EQ(ncsAck.value().type(), MessageType::NcsAck);
    EXPECT_FALSE(ncsAck.value().mosID().has_value());
    ASSERT_NE(ncsAck.value().body(), nullptr);
    EXPECT_EQ(ncsAck.value().body()->name, "ncsAck");
}

TEST(MessageEnvelope, SettersInsertInModelOrder) {
    Message message = makeMessage(MessageType::MosObj);
    message.setNcsID("ncs.example.com");
    message.setMosID("mos.example.com");
    message.setMessageID("42");
    const std::string xml = serializeMessage(message);
    // mosID before ncsID before messageID before the body element,
    // regardless of setter call order.
    EXPECT_EQ(xml,
              "<mos><mosID>mos.example.com</mosID><ncsID>ncs.example.com</ncsID>"
              "<messageID>42</messageID><mosObj/></mos>");
}

TEST(MessageEnvelope, MakeMessageMatchesEnvelopeKind) {
    EXPECT_EQ(serializeMessage(makeMessage(MessageType::KeepAlive)),
              "<mos><mosID/><ncsID/><keepAlive/></mos>");
    EXPECT_EQ(serializeMessage(makeMessage(MessageType::NcsReqAppInfo)),
              "<mos><ncsReqAppInfo/></mos>");
}

TEST(GeneratedViews, ReadAndMutateThroughTheTree) {
    imos::Result<Message> parsed = parseMessage(
        "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>9</messageID>"
        "<roElementAction operation=\"MOVE\"><roID>5PM</roID>"
        "<element_target><storyID>2</storyID></element_target>"
        "<element_source><storyID>7</storyID><storyID>12</storyID></element_source>"
        "</roElementAction></mos>");
    ASSERT_TRUE(parsed.ok());
    imos::codec::XmlNode* body = parsed.value().body();
    ASSERT_NE(body, nullptr);

    imos::codec::view::RoElementAction action{*body};
    EXPECT_EQ(action.attrOperation().value_or(""), "MOVE");
    EXPECT_EQ(action.roID().value_or(""), "5PM");
    ASSERT_NE(action.elementSource(), nullptr);

    imos::codec::view::ElementSource source{*action.elementSource()};
    EXPECT_EQ(source.storyIDCount(), 2U);
    EXPECT_EQ(source.storyID(1).value_or(""), "12");
    source.addStoryID("13");
    EXPECT_EQ(source.storyIDCount(), 3U);

    action.setAttrOperation("SWAP");
    EXPECT_NE(serializeMessage(parsed.value()).find("operation=\"SWAP\""), std::string::npos);
    EXPECT_NE(serializeMessage(parsed.value()).find("<storyID>13</storyID>"), std::string::npos);
}

TEST(GeneratedViews, OrderedInsertionOnEmptyBody) {
    Message message = makeMessage(MessageType::RoCreate);
    imos::codec::view::RoCreate ro{*message.body()};
    ro.setRoSlug("5PM Show");  // second in the model
    ro.setRoID("RO1");         // first in the model — must land before slug
    const std::string xml = serializeMessage(message);
    EXPECT_NE(xml.find("<roID>RO1</roID><roSlug>5PM Show</roSlug>"), std::string::npos) << xml;
}
