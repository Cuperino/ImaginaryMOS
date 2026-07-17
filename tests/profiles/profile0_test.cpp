// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 0 machine-information exchange (plan §3.2, M2): listMachInfo
// composition, lenient parsing (spec fixture included), and the responder
// that answers reqMachInfo on a live session — both device (MOS) and
// newsroom (NCS) roles.

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <imos/codec/message.h>
#include <imos/encoding/encoding.h>
#include <imos/profiles/profile0.h>
#include <imos/session/in_memory_store.h>
#include <imos/session/peer_session.h>

using imos::Channel;
using imos::MonotonicTime;
using imos::PeerSession;
using imos::SessionConfig;
using imos::SessionEventType;
using imos::WallClockTime;
using imos::codec::Message;
using imos::codec::MessageType;
using imos::profiles::buildListMachInfo;
using imos::profiles::DeviceType;
using imos::profiles::MachineInfo;
using imos::profiles::parseListMachInfo;
using imos::profiles::Profile0Responder;

namespace {

class FakeClock : public imos::IClock {
public:
    MonotonicTime now() override { return now_; }
    WallClockTime wallClock() override { return WallClockTime{1'750'000'000'000}; }

private:
    MonotonicTime now_{1000};
};

class CaptureTransport : public imos::ITransport {
public:
    imos::Result<void> send(Channel channel, std::span<const std::uint8_t> frame) override {
        sent[static_cast<std::size_t>(channel)].emplace_back(frame.begin(), frame.end());
        return {};
    }
    bool isConnected(Channel) const override { return true; }

    std::vector<std::vector<std::uint8_t>> sent[imos::kChannelCount];
};

MachineInfo deviceInfo() {
    MachineInfo info;
    info.manufacturer = "Imaginary";
    info.model = "VTR-7";
    info.hwRev = "A";
    info.swRev = "1.0.0";
    info.serialNumber = "927748927";
    info.id = "vtr7.playout.example.mos";
    info.deviceType = DeviceType::Mos;
    info.profiles = {true, true, true, false, false, false, false, false};
    info.mosRev = "4.0";
    return info;
}

Message parseUtf8(const std::string& xml) {
    auto parsed = imos::codec::parseMessage(xml);
    EXPECT_TRUE(parsed.ok());
    return std::move(parsed).value();
}

}  // namespace

TEST(Profile0, BuildListMachInfoCarriesDeclaredFacts) {
    const Message message = buildListMachInfo(deviceInfo(), WallClockTime{1'750'000'000'000});
    EXPECT_EQ(message.type(), MessageType::ListMachInfo);

    const std::string xml = imos::codec::serializeMessage(message);
    EXPECT_NE(xml.find("<manufacturer>Imaginary</manufacturer>"), std::string::npos);
    EXPECT_NE(xml.find("<model>VTR-7</model>"), std::string::npos);
    EXPECT_NE(xml.find("supportedProfiles deviceType=\"MOS\""), std::string::npos);
    EXPECT_NE(xml.find("<mosProfile number=\"0\">YES</mosProfile>"), std::string::npos);
    EXPECT_NE(xml.find("<mosProfile number=\"3\">NO</mosProfile>"), std::string::npos);
    EXPECT_NE(xml.find("<mosRev>4.0</mosRev>"), std::string::npos);
    EXPECT_NE(xml.find("<time>"), std::string::npos);
    EXPECT_EQ(xml.find("<opTime>"), std::string::npos);  // not set → not emitted
}

TEST(Profile0, BuildParseRoundTrip) {
    MachineInfo info = deviceInfo();
    info.deviceType = DeviceType::Ncs;
    info.opTime = WallClockTime{1'749'000'000'000};
    const Message message = buildListMachInfo(info, WallClockTime{1'750'000'000'000});

    const auto peer = parseListMachInfo(message);
    ASSERT_TRUE(peer.ok());
    EXPECT_EQ(peer.value().info.manufacturer, "Imaginary");
    EXPECT_EQ(peer.value().info.deviceType, DeviceType::Ncs);
    EXPECT_TRUE(peer.value().hasSupportedProfiles);
    EXPECT_TRUE(peer.value().info.profiles[0]);
    EXPECT_TRUE(peer.value().info.profiles[2]);
    EXPECT_FALSE(peer.value().info.profiles[7]);
    EXPECT_FALSE(peer.value().opTime.empty());
}

TEST(Profile0, ParsesSpecFixture) {
    std::ifstream file(std::string{IMOS_FIXTURES_DIR} + "/spec-examples/listMachInfo.xml");
    ASSERT_TRUE(file.is_open());
    std::stringstream buffer;
    buffer << file.rdbuf();

    const auto peer = parseListMachInfo(parseUtf8(buffer.str()));
    ASSERT_TRUE(peer.ok());
    EXPECT_EQ(peer.value().info.manufacturer, "RadioVision, Ltd.");
    EXPECT_EQ(peer.value().info.model, "TCS6000");
    EXPECT_EQ(peer.value().info.deviceType, DeviceType::Ncs);
    EXPECT_EQ(peer.value().info.mosRev, "2.8.2");
    for (int n = 0; n < imos::profiles::kProfileCount; ++n) {
        EXPECT_TRUE(peer.value().info.profiles[static_cast<std::size_t>(n)]) << "profile " << n;
    }
    EXPECT_EQ(peer.value().time, "2009-04-11T17:20:42");
}

TEST(Profile0, ParseRejectsWrongMessageType) {
    EXPECT_FALSE(parseListMachInfo(imos::codec::makeMessage(MessageType::ReqMachInfo)).ok());
}

// Originating side: requestMachineInfo() must name a channel explicitly
// (reqMachInfo is AnyChannel, so the channel-inferring enqueue rejects it —
// this regressed silently until the P7 round because nothing exercised it).
TEST(Profile0, RequestMachineInfoSendsAndCapturesReply) {
    FakeClock clock;
    CaptureTransport transport;
    imos::InMemoryMessageStore store;
    SessionConfig config;
    config.mosID = "vtr7.playout.example.mos";
    config.ncsID = "ncs.example.com";
    PeerSession session{config, PeerSession::Dependencies{&clock, &transport, &store, nullptr}};
    Profile0Responder responder{deviceInfo(), &session, &clock};

    session.onChannelConnected(Channel::Mom);
    ASSERT_TRUE(responder.requestMachineInfo().ok());

    const auto& frames = transport.sent[static_cast<std::size_t>(Channel::Mom)];
    ASSERT_EQ(frames.size(), 1U);
    const auto utf8 = imos::encoding::decodeToUtf8(frames[0], {});
    ASSERT_TRUE(utf8.ok());
    const Message request = parseUtf8(utf8.value());
    ASSERT_EQ(request.type(), MessageType::ReqMachInfo);
    const std::string messageId{request.messageID().value_or("")};
    ASSERT_FALSE(messageId.empty());

    MachineInfo ncsInfo = deviceInfo();
    ncsInfo.deviceType = DeviceType::Ncs;
    Message reply = buildListMachInfo(ncsInfo, WallClockTime{1'750'000'000'000});
    reply.setMosID(config.mosID);
    reply.setNcsID(config.ncsID);
    reply.setMessageID(messageId);
    const auto frame = imos::encoding::encodeFromUtf8(imos::codec::serializeMessage(reply), {});
    ASSERT_TRUE(frame.ok());
    session.onFrameReceived(Channel::Mom, frame.value());

    bool consumed = false;
    while (auto event = session.nextEvent()) {
        consumed = responder.handle(*event) || consumed;
    }
    ASSERT_TRUE(consumed);
    ASSERT_NE(responder.peerMachineInfo(), nullptr);
    EXPECT_EQ(responder.peerMachineInfo()->info.deviceType, DeviceType::Ncs);
}

TEST(Profile0, ResponderAnswersReqMachInfoBothRoles) {
    for (const DeviceType role : {DeviceType::Mos, DeviceType::Ncs}) {
        FakeClock clock;
        CaptureTransport transport;
        imos::InMemoryMessageStore store;
        SessionConfig config;
        config.mosID = "vtr7.playout.example.mos";
        config.ncsID = "ncs.example.com";
        PeerSession session{config, PeerSession::Dependencies{&clock, &transport, &store, nullptr}};
        MachineInfo info = deviceInfo();
        info.deviceType = role;
        Profile0Responder responder{info, &session, &clock};

        session.onChannelConnected(Channel::Ro);
        const auto request = imos::encoding::encodeFromUtf8(
            "<mos><mosID>vtr7.playout.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
            "<messageID>3940</messageID><reqMachInfo/></mos>",
            {});
        ASSERT_TRUE(request.ok());
        session.onFrameReceived(Channel::Ro, request.value());

        bool consumed = false;
        while (auto event = session.nextEvent()) {
            consumed = responder.handle(*event) || consumed;
        }
        ASSERT_TRUE(consumed);

        const auto& frames = transport.sent[static_cast<std::size_t>(Channel::Ro)];
        ASSERT_EQ(frames.size(), 1U);
        const auto utf8 = imos::encoding::decodeToUtf8(frames[0], {});
        ASSERT_TRUE(utf8.ok());
        const Message reply = parseUtf8(utf8.value());
        EXPECT_EQ(reply.type(), MessageType::ListMachInfo);
        EXPECT_EQ(reply.messageID().value_or(""), "3940");  // echoed for correlation
        const auto parsed = parseListMachInfo(reply);
        ASSERT_TRUE(parsed.ok());
        EXPECT_EQ(parsed.value().info.deviceType, role);
    }
}
