// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// MOS 3.x WebService transport (plan §13 M6, design of record
// reference/m6-plan.md §2): the pinned-bindings gate over the vendored
// 3.8.2 WSDL fixture, the classic ⇄ SOAP transform round-trips (hoists,
// the ArrayOfX containers, the renames, the listMachInfo flattening, and
// the dual-result/NACK response shapes), and the live loopback HTTP pair —
// request/response-as-ack end to end, including a data reply, the
// parked-exchange fault path, and conformance re-runs of every wire
// profile over SOAP (P1 incl. batch sync, P2, the P4 agency-byte gate,
// P5, P7, and the P3 advanced workflow with search paging and
// placeholder create).

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/gen/views.h>
#include <imos/codec/message.h>
#include <imos/codec/xml.h>
#include <imos/device.h>
#include <imos/profiles/profile0.h>
#include <imos/profiles/profile1.h>
#include <imos/session/in_memory_store.h>
#include <imos/session/peer_session.h>
#include <imos/transports/ws3.h>
#include <imos/transports/ws3_transform.h>
#include <incs/ncs.h>

#include "../support/activity_waiter.h"

using imos::Channel;
using imos::MonotonicTime;
using imos::PeerSession;
using imos::SessionConfig;
using imos::SessionEvent;
using imos::SessionEventType;
using imos::WallClockTime;
using imos::codec::makeMessage;
using imos::codec::Message;
using imos::codec::MessageType;
using imos::codec::parseMessage;
using imos::codec::XmlNode;
using imos::encoding::WireEncoding;
using imos::profiles::DeviceType;
using imos::profiles::MachineInfo;
using imos::profiles::Profile0Responder;
using imos::transports::ws3::buildSoapRequest;
using imos::transports::ws3::buildSoapResponse;
using imos::transports::ws3::EndpointConfig;
using imos::transports::ws3::InboundCall;
using imos::transports::ws3::isWs3Operation;
using imos::transports::ws3::parseSoapRequest;
using imos::transports::ws3::parseSoapResponse;
using imos::transports::ws3::SoapRequest;
using imos::transports::ws3::Ws3Endpoint;

namespace {

constexpr const char* kMosID = "vtr7.playout.example.mos";
constexpr const char* kNcsID = "ncs.example.com";

Message parseOrDie(const std::string& xml) {
    imos::Result<Message> parsed = parseMessage(xml);
    EXPECT_TRUE(parsed.ok()) << parsed.error().message;
    return parsed.ok() ? std::move(parsed).value() : Message{};
}

std::string envelope(const std::string& body, const std::string& messageId = "77") {
    return "<mos><mosID>" + std::string{kMosID} + "</mosID><ncsID>" + std::string{kNcsID} +
           "</ncsID><messageID>" + messageId + "</messageID>" + body + "</mos>";
}

}  // namespace

// ---- pinned-bindings gate over the vendored WSDL fixture -------------------

TEST(Ws3Wsdl, VendoredFixtureMatchesThePinnedSurface) {
    std::ifstream file{std::string{IMOS_FIXTURES_DIR} + "/wsdl/MOSWebService-3.8.2.wsdl"};
    ASSERT_TRUE(file.good()) << "run tools/extract_wsdl.py";
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    // Strip the XML declaration and leading comment; our parser wants a
    // single element document.
    const std::size_t start = text.find("<wsdl:definitions");
    ASSERT_NE(start, std::string::npos);
    imos::Result<imos::codec::XmlDocument> parsed =
        imos::codec::parseXmlDocument(std::string_view{text}.substr(start));
    ASSERT_TRUE(parsed.ok()) << parsed.error().message;

    XmlNode& root = parsed.value().root;
    XmlNode* portType = root.firstChildElement("wsdl:portType");
    ASSERT_NE(portType, nullptr);
    std::vector<std::string> operations;
    for (const XmlNode& child : portType->children) {
        if (child.isElement("wsdl:operation")) {
            operations.emplace_back(child.attribute("name").value_or(""));
        }
    }
    EXPECT_EQ(operations.size(), 24U);

    // Every WSDL operation is a classic message type the transform accepts;
    // pure replies are not operations and the transform refuses to open
    // exchanges with them.
    for (const std::string& operation : operations) {
        const MessageType type = imos::codec::messageTypeFromName(operation);
        EXPECT_NE(type, MessageType::Unknown) << operation;
        EXPECT_TRUE(isWs3Operation(type)) << operation;
    }
    for (const MessageType reply :
         {MessageType::MosAck, MessageType::RoAck, MessageType::ListMachInfo,
          MessageType::MosObjList, MessageType::RoList, MessageType::RoListAll,
          MessageType::MosListSearchableSchema, MessageType::KeepAlive}) {
        EXPECT_FALSE(isWs3Operation(reply));
    }
}

// ---- transform round-trips -------------------------------------------------

TEST(Ws3Transform, MosObjRequestGroupsMetadataAndRoundTrips) {
    const Message classic = parseOrDie(envelope(
        "<mosObj><objID>M1</objID><objSlug>clip</objSlug>"
        "<mosExternalMetadata><mosSchema>urn:a</mosSchema></mosExternalMetadata>"
        "<mosExternalMetadata><mosSchema>urn:b</mosSchema></mosExternalMetadata></mosObj>"));
    imos::Result<SoapRequest> request = buildSoapRequest(classic);
    ASSERT_TRUE(request.ok()) << request.error().message;
    EXPECT_EQ(request.value().operation, "mosObj");
    EXPECT_EQ(request.value().soapAction, "http://mosprotocol.com/webservices/mosObj");
    EXPECT_EQ(request.value().messageId, "77");
    // The ArrayOfX quirk on the wire: one container, _type members.
    EXPECT_NE(request.value().envelope.find("<mosObj_input>"), std::string::npos);
    EXPECT_NE(request.value().envelope.find("<mosExternalMetadata><mosExternalMetadata_type>"),
              std::string::npos);
    EXPECT_NE(request.value().envelope.find("<mosHeader_input>"), std::string::npos);

    imos::Result<InboundCall> back = parseSoapRequest(request.value().envelope);
    ASSERT_TRUE(back.ok()) << back.error().message;
    EXPECT_EQ(back.value().message.type(), MessageType::MosObj);
    EXPECT_EQ(back.value().messageId, "77");
    const XmlNode* body = back.value().message.body();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->childText("objID"), "M1");
    EXPECT_EQ(body->countChildElements("mosExternalMetadata"), 2U);
}

TEST(Ws3Transform, RoElementActionHoistsOperationAndRenamesTarget) {
    const Message classic = parseOrDie(
        envelope("<roElementAction operation=\"MOVE\"><roID>R</roID>"
                 "<element_target><storyID>S2</storyID></element_target>"
                 "<element_source><storyID>S7</storyID></element_source></roElementAction>"));
    imos::Result<SoapRequest> request = buildSoapRequest(classic);
    ASSERT_TRUE(request.ok()) << request.error().message;
    EXPECT_NE(request.value().envelope.find("<operation>MOVE</operation>"), std::string::npos);
    EXPECT_NE(request.value().envelope.find("<element_target_type>"), std::string::npos);

    imos::Result<InboundCall> back = parseSoapRequest(request.value().envelope);
    ASSERT_TRUE(back.ok()) << back.error().message;
    const XmlNode* body = back.value().message.body();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->attribute("operation").value_or(""), "MOVE");
    EXPECT_NE(body->firstChildElement("element_target"), nullptr);
    EXPECT_EQ(back.value().message.type(), MessageType::RoElementAction);
}

TEST(Ws3Transform, RoDeleteUsesTheBareMosHeaderQuirk) {
    const Message classic = parseOrDie(envelope("<roDelete><roID>R9</roID></roDelete>"));
    imos::Result<SoapRequest> request = buildSoapRequest(classic);
    ASSERT_TRUE(request.ok()) << request.error().message;
    EXPECT_NE(request.value().envelope.find("<mosHeader>"), std::string::npos);
    EXPECT_EQ(request.value().envelope.find("<mosHeader_input>"), std::string::npos);
    EXPECT_NE(request.value().envelope.find("<roID>R9</roID>"), std::string::npos);

    imos::Result<InboundCall> back = parseSoapRequest(request.value().envelope);
    ASSERT_TRUE(back.ok()) << back.error().message;
    EXPECT_EQ(back.value().message.type(), MessageType::RoDelete);
    EXPECT_EQ(back.value().message.body()->childText("roID"), "R9");
}

TEST(Ws3Transform, MosItemReplaceHoistsIdsAndWrapsTheItem) {
    const Message classic =
        parseOrDie(envelope("<mosItemReplace><roID>R</roID><storyID>S</storyID>"
                            "<item><itemID>I</itemID><objID>O</objID></item></mosItemReplace>"));
    imos::Result<SoapRequest> request = buildSoapRequest(classic);
    ASSERT_TRUE(request.ok()) << request.error().message;
    EXPECT_NE(request.value().envelope.find("<item_input>"), std::string::npos);
    EXPECT_NE(request.value().envelope.find("<storyID>S</storyID>"), std::string::npos);

    imos::Result<InboundCall> back = parseSoapRequest(request.value().envelope);
    ASSERT_TRUE(back.ok()) << back.error().message;
    const XmlNode* body = back.value().message.body();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->childText("roID"), "R");
    const XmlNode* item = body->firstChildElement("item");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->childText("itemID"), "I");
}

TEST(Ws3Transform, AckResponsesRoundTrip) {
    const Message roAck =
        parseOrDie(envelope("<roAck><roID>R</roID><roStatus>OK</roStatus></roAck>", "42"));
    imos::Result<std::string> response = buildSoapResponse(roAck, "roCreate");
    ASSERT_TRUE(response.ok()) << response.error().message;
    EXPECT_NE(response.value().find("<roCreateResponse"), std::string::npos);
    EXPECT_NE(response.value().find("<roCreateResult>"), std::string::npos);

    imos::Result<std::vector<Message>> replies =
        parseSoapResponse(response.value(), "roCreate", kMosID, kNcsID, "42");
    ASSERT_TRUE(replies.ok()) << replies.error().message;
    ASSERT_EQ(replies.value().size(), 1U);
    EXPECT_EQ(replies.value().front().type(), MessageType::RoAck);
    EXPECT_EQ(replies.value().front().messageID().value_or(""), "42");
    EXPECT_EQ(replies.value().front().body()->childText("roStatus"), "OK");
}

TEST(Ws3Transform, RoReqDataResponseCarriesRoListAndImplicitAck) {
    const Message roList =
        parseOrDie(envelope("<roList><roID>R</roID><roSlug>Five</roSlug>"
                            "<story><storyID>S1</storyID><item><itemID>I1</itemID></item></story>"
                            "<story><storyID>S2</storyID></story></roList>",
                            "43"));
    imos::Result<std::string> response = buildSoapResponse(roList, "roReq");
    ASSERT_TRUE(response.ok()) << response.error().message;
    // Data result + synthesized ack result, with the story array grouped
    // (and the capitalized Item container inside each story_type).
    EXPECT_NE(response.value().find("<roReqResult>"), std::string::npos);
    EXPECT_NE(response.value().find("<story><story_type>"), std::string::npos);
    EXPECT_NE(response.value().find("<Item><item_type>"), std::string::npos);
    EXPECT_NE(response.value().find("<roAckResult>"), std::string::npos);

    imos::Result<std::vector<Message>> replies =
        parseSoapResponse(response.value(), "roReq", kMosID, kNcsID, "43");
    ASSERT_TRUE(replies.ok()) << replies.error().message;
    ASSERT_EQ(replies.value().size(), 1U);
    const Message& reply = replies.value().front();
    EXPECT_EQ(reply.type(), MessageType::RoList);
    const XmlNode* body = reply.body();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->countChildElements("story"), 2U);
    const XmlNode* story = body->firstChildElement("story");
    ASSERT_NE(story, nullptr);
    EXPECT_EQ(story->countChildElements("item"), 1U);
}

TEST(Ws3Transform, RoReqNackRidesTheAckResult) {
    const Message nack =
        parseOrDie(envelope("<roAck><roID>R</roID><roStatus>NACK</roStatus></roAck>", "44"));
    imos::Result<std::string> response = buildSoapResponse(nack, "roReq");
    ASSERT_TRUE(response.ok()) << response.error().message;

    imos::Result<std::vector<Message>> replies =
        parseSoapResponse(response.value(), "roReq", kMosID, kNcsID, "44");
    ASSERT_TRUE(replies.ok()) << replies.error().message;
    ASSERT_EQ(replies.value().size(), 1U);
    EXPECT_EQ(replies.value().front().type(), MessageType::RoAck);
    EXPECT_EQ(replies.value().front().body()->childText("roStatus"), "NACK");
}

TEST(Ws3Transform, ListMachInfoFlattensProfilesBothWays) {
    const Message machInfo = parseOrDie(envelope(
        "<listMachInfo><manufacturer>Imaginary</manufacturer><model>VTR-7</model>"
        "<mosRev>2.8.5</mosRev><supportedProfiles deviceType=\"MOS\">"
        "<mosProfile number=\"0\">YES</mosProfile><mosProfile number=\"1\">YES</mosProfile>"
        "<mosProfile number=\"2\">NO</mosProfile></supportedProfiles></listMachInfo>",
        "45"));
    imos::Result<std::string> response = buildSoapResponse(machInfo, "reqMachInfo");
    ASSERT_TRUE(response.ok()) << response.error().message;
    EXPECT_NE(response.value().find("<mosProfile0>YES</mosProfile0>"), std::string::npos);
    EXPECT_NE(response.value().find("<mosProfile2>NO</mosProfile2>"), std::string::npos);

    imos::Result<std::vector<Message>> replies =
        parseSoapResponse(response.value(), "reqMachInfo", kMosID, kNcsID, "45");
    ASSERT_TRUE(replies.ok()) << replies.error().message;
    const Message& reply = replies.value().front();
    EXPECT_EQ(reply.type(), MessageType::ListMachInfo);
    const XmlNode* profiles = reply.body()->firstChildElement("supportedProfiles");
    ASSERT_NE(profiles, nullptr);
    EXPECT_EQ(profiles->countChildElements("mosProfile"), 3U);
}

TEST(Ws3Transform, PureRepliesAndLegacyCannotOpenExchanges) {
    const Message ack = parseOrDie(envelope("<mosAck><objID>O</objID></mosAck>"));
    EXPECT_FALSE(buildSoapRequest(ack).ok());
    const Message keepAlive = parseOrDie(envelope("<keepAlive/>"));
    EXPECT_FALSE(buildSoapRequest(keepAlive).ok());
}

// ---- live loopback HTTP pair -----------------------------------------------

namespace {

class SystemClock : public imos::IClock {
public:
    MonotonicTime now() override {
        const auto steady = std::chrono::steady_clock::now().time_since_epoch();
        return MonotonicTime{std::chrono::duration_cast<std::chrono::milliseconds>(steady).count()};
    }
    WallClockTime wallClock() override {
        const auto system = std::chrono::system_clock::now().time_since_epoch();
        return WallClockTime{std::chrono::duration_cast<std::chrono::milliseconds>(system).count()};
    }
};

SessionConfig sessionConfig() {
    SessionConfig config;
    config.mosID = kMosID;
    config.ncsID = kNcsID;
    config.retryTimeoutMillis = 2000;
    config.keepAliveIntervalMillis = 0;  // keepAlive is not a 3.x operation
    config.wireEncode.encoding = WireEncoding::Utf8;
    config.wireDecode.encoding = WireEncoding::Utf8;
    return config;
}

MachineInfo machineInfo(DeviceType type) {
    MachineInfo info;
    info.manufacturer = "Imaginary";
    info.model = type == DeviceType::Mos ? "VTR-7" : "NCS-1";
    info.swRev = "1.0.0";
    info.id = type == DeviceType::Mos ? kMosID : kNcsID;
    info.deviceType = type;
    info.profiles[0] = true;
    info.profiles[1] = true;
    info.profiles[2] = true;
    return info;
}

/// Device + NCS PeerSessions over two Ws3Endpoints pointed at each other.
struct Rig {
    SystemClock clock;
    imos::InMemoryMessageStore deviceStore;
    imos::InMemoryMessageStore ncsStore;
    std::unique_ptr<Ws3Endpoint> deviceEndpoint;
    std::unique_ptr<Ws3Endpoint> ncsEndpoint;
    std::unique_ptr<PeerSession> deviceSession;
    std::unique_ptr<PeerSession> ncsSession;
    std::unique_ptr<Profile0Responder> deviceResponder;
    std::unique_ptr<Profile0Responder> ncsResponder;
    std::vector<SessionEvent> deviceEvents;
    std::vector<SessionEvent> ncsEvents;
    std::vector<std::pair<Channel, std::string>> deviceDiagnostics;
    std::vector<std::pair<Channel, std::string>> ncsDiagnostics;
    /// Pump the NCS through a sink of the test's (pump(const
    /// ChannelEventSink&)), recording what passes through it.
    bool ncsThroughSink = false;
    std::vector<std::string> ncsSinkLog;
    imos::transports::ActivityHandler activity;  ///< installed on both endpoints when set

    void start(bool withResponders = true) {
        EndpointConfig deviceConfig;
        deviceConfig.bindHost = "127.0.0.1";
        deviceConfig.mosID = kMosID;
        deviceConfig.ncsID = kNcsID;
        deviceEndpoint = std::make_unique<Ws3Endpoint>(deviceConfig);
        deviceEndpoint->setErrorHandler([this](Channel channel, const std::string& diagnostic) {
            deviceDiagnostics.emplace_back(channel, diagnostic);
        });
        EndpointConfig ncsConfig = deviceConfig;
        ncsEndpoint = std::make_unique<Ws3Endpoint>(ncsConfig);
        ncsEndpoint->setErrorHandler([this](Channel channel, const std::string& diagnostic) {
            ncsDiagnostics.emplace_back(channel, diagnostic);
        });
        if (activity) {
            deviceEndpoint->setActivityHandler(activity);
            ncsEndpoint->setActivityHandler(activity);
        }
        ASSERT_TRUE(deviceEndpoint->start().ok());
        ASSERT_TRUE(ncsEndpoint->start().ok());
        deviceEndpoint->setPeerServiceUrl(
            "http://127.0.0.1:" + std::to_string(ncsEndpoint->port()) + "/mos");
        ncsEndpoint->setPeerServiceUrl(
            "http://127.0.0.1:" + std::to_string(deviceEndpoint->port()) + "/mos");

        deviceSession = std::make_unique<PeerSession>(
            sessionConfig(), PeerSession::Dependencies{&clock, deviceEndpoint.get(), &deviceStore});
        ncsSession = std::make_unique<PeerSession>(
            sessionConfig(), PeerSession::Dependencies{&clock, ncsEndpoint.get(), &ncsStore});
        if (withResponders) {
            deviceResponder = std::make_unique<Profile0Responder>(machineInfo(DeviceType::Mos),
                                                                  deviceSession.get(), &clock);
            ncsResponder = std::make_unique<Profile0Responder>(machineInfo(DeviceType::Ncs),
                                                               ncsSession.get(), &clock);
        }
    }

    void pumpOnce() {
        deviceEndpoint->pump(*deviceSession);
        deviceSession->poll();
        while (auto event = deviceSession->nextEvent()) {
            if (deviceResponder == nullptr || !deviceResponder->handle(*event)) {
                deviceEvents.push_back(std::move(*event));
            }
        }
        if (ncsThroughSink) {
            imos::transports::ChannelEventSink sink;
            sink.connected = [&](Channel channel) {
                ncsSinkLog.push_back("up " + std::string{channelName(channel)});
                ncsSession->onChannelConnected(channel);
            };
            sink.disconnected = [&](Channel channel) {
                ncsSinkLog.push_back("down " + std::string{channelName(channel)});
                ncsSession->onChannelDisconnected(channel);
            };
            sink.frame = [&](Channel channel, std::span<const std::uint8_t> frame) {
                ncsSinkLog.push_back("frame " + std::string{channelName(channel)});
                ncsSession->onFrameReceived(channel, frame);
            };
            ncsEndpoint->pump(sink);
        } else {
            ncsEndpoint->pump(*ncsSession);
        }
        ncsSession->poll();
        while (auto event = ncsSession->nextEvent()) {
            if (ncsResponder == nullptr || !ncsResponder->handle(*event)) {
                ncsEvents.push_back(std::move(*event));
            }
        }
    }

    bool waitFor(const std::function<bool()>& predicate, int timeoutMillis = 8000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMillis};
        while (std::chrono::steady_clock::now() < deadline) {
            pumpOnce();
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return false;
    }

    void stop() {
        if (deviceEndpoint != nullptr) {
            deviceEndpoint->stop();
        }
        if (ncsEndpoint != nullptr) {
            ncsEndpoint->stop();
        }
    }
};

const SessionEvent* findAck(const std::vector<SessionEvent>& events, MessageType replyType) {
    for (const SessionEvent& event : events) {
        if (event.type == SessionEventType::AckReceived && event.message.type() == replyType) {
            return &event;
        }
    }
    return nullptr;
}

}  // namespace

namespace {

/// Facade pair (imos::Device / incs::Ncs) over two Ws3Endpoints, for the
/// all-profile conformance re-runs on the 3.x pipe.
struct FacadeRig {
    SystemClock clock;
    imos::InMemoryMessageStore deviceStore;
    imos::InMemoryMessageStore ncsStore;
    std::unique_ptr<Ws3Endpoint> deviceEndpoint;
    std::unique_ptr<Ws3Endpoint> ncsEndpoint;
    std::unique_ptr<imos::Device> device;
    std::unique_ptr<incs::Ncs> ncs;
    std::vector<std::pair<Channel, std::string>> deviceDiagnostics;
    std::vector<std::pair<Channel, std::string>> ncsDiagnostics;

    void start() {
        EndpointConfig config;
        config.bindHost = "127.0.0.1";
        config.mosID = kMosID;
        config.ncsID = kNcsID;
        deviceEndpoint = std::make_unique<Ws3Endpoint>(config);
        deviceEndpoint->setErrorHandler([this](Channel channel, const std::string& diagnostic) {
            deviceDiagnostics.emplace_back(channel, diagnostic);
        });
        ncsEndpoint = std::make_unique<Ws3Endpoint>(config);
        ncsEndpoint->setErrorHandler([this](Channel channel, const std::string& diagnostic) {
            ncsDiagnostics.emplace_back(channel, diagnostic);
        });
        ASSERT_TRUE(deviceEndpoint->start().ok());
        ASSERT_TRUE(ncsEndpoint->start().ok());
        deviceEndpoint->setPeerServiceUrl(
            "http://127.0.0.1:" + std::to_string(ncsEndpoint->port()) + "/mos");
        ncsEndpoint->setPeerServiceUrl(
            "http://127.0.0.1:" + std::to_string(deviceEndpoint->port()) + "/mos");

        device = std::make_unique<imos::Device>(
            imos::DeviceConfig{sessionConfig(), machineInfo(DeviceType::Mos)},
            PeerSession::Dependencies{&clock, deviceEndpoint.get(), &deviceStore});
        ncs = std::make_unique<incs::Ncs>(
            incs::NcsConfig{sessionConfig(), machineInfo(DeviceType::Ncs)},
            PeerSession::Dependencies{&clock, ncsEndpoint.get(), &ncsStore});
    }

    void pumpOnce() {
        deviceEndpoint->pump(device->session());
        device->poll();
        ncsEndpoint->pump(ncs->session());
        ncs->poll();
    }

    bool waitFor(const std::function<bool()>& predicate, int timeoutMillis = 8000) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds{timeoutMillis};
        while (std::chrono::steady_clock::now() < deadline) {
            pumpOnce();
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return false;
    }

    void stop() {
        deviceEndpoint->stop();
        ncsEndpoint->stop();
    }
};

class VectorObjectSource : public imos::profiles::IObjectSource {
public:
    std::optional<Message> object(std::string_view objID) override {
        const auto it = objects_.find(std::string{objID});
        return it == objects_.end() ? std::nullopt : std::optional<Message>{it->second};
    }
    std::vector<std::string> objectIDs() override {
        std::vector<std::string> ids;
        for (const auto& [id, message] : objects_) {
            ids.push_back(id);
        }
        return ids;
    }
    void put(const std::string& id, Message message) {
        objects_.insert_or_assign(id, std::move(message));
    }

private:
    std::map<std::string, Message> objects_;
};

Message makeObject(std::string_view objID, std::string_view status) {
    Message message = makeMessage(MessageType::MosObj);
    imos::codec::view::MosObj view{*message.body()};
    view.setObjID(objID);
    view.setObjSlug("clip");
    view.setObjType("VIDEO");
    view.setStatus(status);
    return message;
}

}  // namespace

// Profile 0 both directions over live HTTP: the reqMachInfo call's HTTP
// response carries listMachInfo, which clears the in-flight exchange —
// request/response IS the ack.
TEST(Ws3Transport, Profile0BothRolesOverLoopbackHttp) {
    Rig rig;
    rig.start();

    ASSERT_TRUE(
        rig.deviceSession->enqueue(Channel::Mom, makeMessage(MessageType::ReqMachInfo)).ok());
    ASSERT_TRUE(rig.waitFor(
        [&] { return findAck(rig.deviceEvents, MessageType::ListMachInfo) != nullptr; }));

    ASSERT_TRUE(rig.ncsSession->enqueue(Channel::Ro, makeMessage(MessageType::ReqMachInfo)).ok());
    ASSERT_TRUE(
        rig.waitFor([&] { return findAck(rig.ncsEvents, MessageType::ListMachInfo) != nullptr; }));

    rig.stop();
}

// A host that pumps into a sink of its own sees the pipe come up and each
// inbound call and synthesized reply on its way to the session, and the
// exchanges complete as with pump(PeerSession&).
TEST(Ws3Transport, PumpIntoAHostSink) {
    Rig rig;
    rig.ncsThroughSink = true;
    rig.start();

    ASSERT_TRUE(
        rig.deviceSession->enqueue(Channel::Mom, makeMessage(MessageType::ReqMachInfo)).ok());
    ASSERT_TRUE(rig.waitFor(
        [&] { return findAck(rig.deviceEvents, MessageType::ListMachInfo) != nullptr; }));
    ASSERT_TRUE(rig.ncsSession->enqueue(Channel::Ro, makeMessage(MessageType::ReqMachInfo)).ok());
    ASSERT_TRUE(
        rig.waitFor([&] { return findAck(rig.ncsEvents, MessageType::ListMachInfo) != nullptr; }));

    const auto count = [&](const std::string& prefix) {
        return std::count_if(rig.ncsSinkLog.begin(), rig.ncsSinkLog.end(),
                             [&](const std::string& entry) { return entry.starts_with(prefix); });
    };
    EXPECT_EQ(count("up "), 3);     // HTTP is connectionless: every channel is up at once
    EXPECT_GE(count("frame "), 2);  // the device's call, and the reply to ours
    rig.stop();
}

// The activity hook wakes a host that does not poll: a request each way and
// its answer (an HTTP call, a parked exchange, the synthesized reply) are
// delivered with the host sleeping on the hook alone.
TEST(Ws3Transport, ActivityHookWakesAHostThatNeverPolls) {
    imos::test::ActivityWaiter waiter;
    Rig rig;
    rig.activity = waiter.handler();
    rig.start();
    const auto pump = [&] { rig.pumpOnce(); };

    ASSERT_TRUE(
        rig.deviceSession->enqueue(Channel::Mom, makeMessage(MessageType::ReqMachInfo)).ok());
    ASSERT_TRUE(waiter.drive(
        pump, [&] { return findAck(rig.deviceEvents, MessageType::ListMachInfo) != nullptr; }));
    ASSERT_TRUE(rig.ncsSession->enqueue(Channel::Ro, makeMessage(MessageType::ReqMachInfo)).ok());
    ASSERT_TRUE(waiter.drive(
        pump, [&] { return findAck(rig.ncsEvents, MessageType::ListMachInfo) != nullptr; }));

    rig.stop();
}

// A device-originated mosObj push: the SOAP response carries the mosAck.
TEST(Ws3Transport, MosObjPushIsAckedThroughTheHttpResponse) {
    Rig rig;
    rig.start(/*withResponders=*/false);

    Message push = makeMessage(MessageType::MosObj);
    imos::codec::view::MosObj view{*push.body()};
    view.setObjID("M1");
    view.setObjSlug("clip");
    view.setStatus("NEW");
    ASSERT_TRUE(rig.deviceSession->enqueue(Channel::Mom, std::move(push)).ok());

    // The NCS session surfaces the inbound mosObj; reply with a mosAck.
    ASSERT_TRUE(rig.waitFor([&] {
        for (SessionEvent& event : rig.ncsEvents) {
            if (event.type == SessionEventType::MessageReceived &&
                event.message.type() == MessageType::MosObj) {
                Message ack = makeMessage(MessageType::MosAck);
                imos::codec::echoMessageID(event.message, ack);
                XmlNode* body = ack.body();
                body->appendElement("objID").setText("M1");
                body->appendElement("objRev").setText("1");
                body->appendElement("status").setText("ACK");
                EXPECT_TRUE(rig.ncsSession->enqueueReply(event.channel, std::move(ack)).ok());
                return true;
            }
        }
        return false;
    }));
    ASSERT_TRUE(
        rig.waitFor([&] { return findAck(rig.deviceEvents, MessageType::MosAck) != nullptr; }));

    rig.stop();
}

// A parked inbound exchange whose session never answers times out into a
// SOAP fault, surfaced as a diagnostic on the caller's side.
TEST(Ws3Transport, UnansweredInboundCallFaultsAfterTheTimeout) {
    Rig rig;
    rig.start(/*withResponders=*/false);
    // Shrink the parked timeout by restarting the NCS endpoint with a tiny
    // window (config is fixed at construction).
    rig.ncsEndpoint->stop();
    EndpointConfig quick;
    quick.bindHost = "127.0.0.1";
    quick.mosID = kMosID;
    quick.ncsID = kNcsID;
    quick.inboundReplyTimeoutMillis = 200;
    rig.ncsEndpoint = std::make_unique<Ws3Endpoint>(quick);
    rig.ncsEndpoint->setErrorHandler([&](Channel channel, const std::string& diagnostic) {
        rig.ncsDiagnostics.emplace_back(channel, diagnostic);
    });
    ASSERT_TRUE(rig.ncsEndpoint->start().ok());
    rig.deviceEndpoint->setPeerServiceUrl(
        "http://127.0.0.1:" + std::to_string(rig.ncsEndpoint->port()) + "/mos");

    // The NCS session is never pumped, so the parked exchange starves.
    Message push = makeMessage(MessageType::MosObj);
    imos::codec::view::MosObj view{*push.body()};
    view.setObjID("M-starved");
    ASSERT_TRUE(rig.deviceSession->enqueue(Channel::Mom, std::move(push)).ok());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{8};
    bool faulted = false;
    while (std::chrono::steady_clock::now() < deadline && !faulted) {
        rig.deviceEndpoint->pump(*rig.deviceSession);
        rig.deviceSession->poll();
        while (auto event = rig.deviceSession->nextEvent()) {
            rig.deviceEvents.push_back(std::move(*event));
        }
        for (const auto& [channel, diagnostic] : rig.deviceDiagnostics) {
            if (diagnostic.find("SOAP Fault") != std::string::npos ||
                diagnostic.find("call failed") != std::string::npos ||
                diagnostic.find("response rejected") != std::string::npos) {
                faulted = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    EXPECT_TRUE(faulted);

    rig.stop();
}

// ---- All-profile conformance re-runs over the 3.x pipe ---------------------
//
// The facade-level walks the ws40 and tcp28 suites run, re-executed over
// SOAP request/response: every classic exchange becomes an HTTP call whose
// response carries the ack (or the data reply), and unsolicited pushes are
// calls of their own.

TEST(Ws3Transport, Profile1ObjectWorkflowOverWs3) {
    FacadeRig rig;
    rig.start();
    VectorObjectSource source;
    source.put("W2", makeObject("W2", "UPDATED"));
    source.put("W3", makeObject("W3", "UPDATED"));
    rig.device->attach(source);

    std::vector<imos::profiles::DirectoryEvent> dirEvents;

    // Push: an unsolicited object announcement is mirrored and acked.
    ASSERT_TRUE(rig.device->announceObject(makeObject("P1", "NEW")).ok());
    ASSERT_TRUE(rig.waitFor([&] { return rig.ncs->object("P1") != nullptr; }));

    // Single fetch (hit): the mosObj rides the mosReqObj HTTP response.
    ASSERT_TRUE(rig.ncs->requestObject("W2").ok());
    ASSERT_TRUE(rig.waitFor([&] { return rig.ncs->object("W2") != nullptr; }));

    // Single fetch (miss): the NACK rides the mosAckResult slot.
    ASSERT_TRUE(rig.ncs->requestObject("NOPE").ok());
    ASSERT_TRUE(rig.waitFor([&] {
        while (auto event = rig.ncs->nextDirectoryEvent()) {
            dirEvents.push_back(std::move(*event));
        }
        for (const auto& event : dirEvents) {
            if (event.type == imos::profiles::DirectoryEventType::RequestFailed &&
                event.objID == "NOPE") {
                return true;
            }
        }
        return false;
    }));

    // Full sync (batch): mosReqAll is acked, then the device's mosListAll
    // call delivers the catalog (the ArrayOfMosObj container on the wire).
    ASSERT_TRUE(rig.ncs->requestAll(0).ok());
    ASSERT_TRUE(rig.waitFor([&] { return rig.ncs->object("W3") != nullptr; }));

    rig.stop();
}

TEST(Ws3Transport, Profile2RunningOrderWorkflowOverWs3) {
    FacadeRig rig;
    rig.start();

    // Author and commit; the device's shadow model converges call by call.
    ASSERT_TRUE(rig.ncs->author().create("WS3-RO", "SOAP Rundown").ok());
    imos::codec::XmlNode story = incs::RunningOrderAuthor::makeStory("s1", "Lead");
    story.children.push_back(incs::RunningOrderAuthor::makeItem("i1", "OBJ1", kMosID));
    ASSERT_TRUE(rig.ncs->author().insertStory("WS3-RO", story).ok());
    ASSERT_TRUE(
        rig.ncs->author().insertStory("WS3-RO", incs::RunningOrderAuthor::makeStory("s2")).ok());
    ASSERT_TRUE(rig.ncs->author().commit("WS3-RO").ok());
    ASSERT_TRUE(rig.waitFor([&] {
        return rig.device->runningOrderModel().storyIDs("WS3-RO") ==
               std::vector<std::string>{"s1", "s2"};
    }));

    // A mutation converges after its ack round (roElementAction MOVE with
    // the operation attribute hoisted to a SOAP element).
    ASSERT_TRUE(rig.ncs->author().moveStory("WS3-RO", "s2", "s1").ok());
    ASSERT_TRUE(rig.ncs->author().commit("WS3-RO").ok());
    ASSERT_TRUE(rig.waitFor([&] {
        return rig.device->runningOrderModel().storyIDs("WS3-RO") ==
               std::vector<std::string>{"s2", "s1"};
    }));

    // Device-originated roElementStat reaches the NCS.
    imos::profiles::ElementStatusReport play;
    play.element = imos::profiles::RoElement::Item;
    play.roID = "WS3-RO";
    play.storyID = "s1";
    play.itemID = "i1";
    play.status = "PLAY";
    ASSERT_TRUE(rig.device->reportElementStatus(play).ok());
    std::vector<imos::profiles::ElementStatusReport> statuses;
    ASSERT_TRUE(rig.waitFor([&] {
        while (auto status = rig.ncs->nextElementStatus()) {
            statuses.push_back(std::move(*status));
        }
        return !statuses.empty();
    }));
    EXPECT_EQ(statuses.front().status, "PLAY");
    EXPECT_EQ(statuses.front().itemID, "i1");

    rig.stop();
}

// The Profile 4 byte gate on the SOAP pipe: the agency MEM subtree must
// survive the roStorySend_input transform (the mosExternalMetadata array
// container wraps it, but the mosPayload subtree itself is moved, never
// re-normalized).
TEST(Ws3Transport, Profile4AgencyPayloadPreservedOverWs3) {
    FacadeRig rig;
    rig.start();

    class CapturingStorySink : public imos::profiles::IStorySink {
    public:
        std::vector<std::string> agencyBytes;
        imos::Result<void> onStory(const imos::profiles::StoryView& story) override {
            if (story.body.bodyIsAgencyPayload()) {
                agencyBytes.push_back(imos::codec::serializeXmlNode(*story.body.agencyMetadata));
            }
            return {};
        }
    };
    CapturingStorySink sink;
    rig.device->attach(sink);

    ASSERT_TRUE(rig.ncs->author().create("WS3-RO4", "SOAP Rundown").ok());
    ASSERT_TRUE(
        rig.ncs->author().insertStory("WS3-RO4", incs::RunningOrderAuthor::makeStory("s1")).ok());
    ASSERT_TRUE(rig.ncs->author().commit("WS3-RO4").ok());
    ASSERT_TRUE(rig.waitFor([&] { return rig.device->runningOrderModel().has("WS3-RO4"); }));

    std::string newsml = "<NewsML>\n<NewsItem attente=\"vérifiée\">\n";
    for (int i = 0; i < 64; ++i) {
        newsml += "  <p>Dépêche nº" + std::to_string(i) +
                  " — l'aperçu «détaillé» pèse ±120 octets; Ωμέγα.\t</p>\n";
    }
    newsml += "</NewsItem>\n</NewsML>";
    auto wrapper = imos::codec::parseMessage(
        "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>1</messageID>"
        "<roStorySend><roID>r</roID><storyID>s</storyID><storyBody/>"
        "<mosExternalMetadata><mosScope>STORY</mosScope>"
        "<mosSchema>http://agency.example/newsml</mosSchema>"
        "<mosPayload>" +
        newsml + "</mosPayload></mosExternalMetadata></roStorySend></mos>");
    ASSERT_TRUE(wrapper.ok());
    const imos::codec::XmlNode metadata =
        *wrapper.value().body()->firstChildElement("mosExternalMetadata");
    const std::string authoredBytes = imos::codec::serializeXmlNode(metadata);
    ASSERT_GT(authoredBytes.size(), 4096U);

    ASSERT_TRUE(rig.ncs->author().setAgencyStoryBody("WS3-RO4", "s1", metadata).ok());
    ASSERT_TRUE(rig.ncs->sendStory("WS3-RO4", "s1").ok());
    ASSERT_TRUE(rig.waitFor([&] { return !sink.agencyBytes.empty(); }));
    EXPECT_EQ(sink.agencyBytes.front(), authoredBytes);

    rig.stop();
}

TEST(Ws3Transport, Profile5ItemControlOverWs3) {
    FacadeRig rig;
    rig.start();

    class CapturingControlSink : public imos::profiles::IControlSink {
    public:
        std::vector<imos::profiles::CtrlRequest> ctrls;
        imos::Result<void> onCtrl(const imos::profiles::CtrlRequest& request) override {
            imos::profiles::CtrlRequest copy = request;
            copy.message = nullptr;
            ctrls.push_back(std::move(copy));
            return {};
        }
        void onItemCue(const imos::profiles::ItemCue&) override {}
    };
    CapturingControlSink sink;
    rig.device->attach(sink);

    ASSERT_TRUE(rig.ncs->author().create("WS3-RO5", "SOAP Rundown").ok());
    imos::codec::XmlNode story = incs::RunningOrderAuthor::makeStory("s1", "Lead");
    story.children.push_back(incs::RunningOrderAuthor::makeItem("i1", "OBJ1", kMosID));
    ASSERT_TRUE(rig.ncs->author().insertStory("WS3-RO5", story).ok());
    ASSERT_TRUE(rig.ncs->author().commit("WS3-RO5").ok());
    ASSERT_TRUE(rig.waitFor([&] { return rig.device->runningOrderModel().has("WS3-RO5"); }));

    ASSERT_TRUE(rig.ncs
                    ->sendCtrl(imos::profiles::CtrlScope::Item, "WS3-RO5", "s1", "i1",
                               imos::profiles::CtrlCommand::Execute)
                    .ok());
    ASSERT_TRUE(rig.waitFor([&] { return !sink.ctrls.empty(); }));
    EXPECT_EQ(sink.ctrls.front().scope, imos::profiles::CtrlScope::Item);
    EXPECT_EQ(sink.ctrls.front().itemID, "i1");
    std::optional<imos::profiles::CtrlResult> result;
    ASSERT_TRUE(rig.waitFor([&] { return (result = rig.ncs->nextCtrlResult()).has_value(); }));
    EXPECT_TRUE(result->ok) << result->roStatus;

    imos::profiles::ItemCue cue;
    cue.mosID = kMosID;
    cue.roID = "WS3-RO5";
    cue.storyID = "s1";
    cue.itemID = "i1";
    cue.roEventType = "PLAY";
    cue.roEventTime = "2026-07-19T12:00:00";
    ASSERT_TRUE(rig.device->sendItemCue(cue).ok());
    std::optional<imos::profiles::ItemCue> received;
    ASSERT_TRUE(rig.waitFor([&] { return (received = rig.ncs->nextItemCue()).has_value(); }));
    EXPECT_EQ(received->roEventType, "PLAY");
    EXPECT_EQ(received->itemID, "i1");

    rig.stop();
}

TEST(Ws3Transport, Profile7StoryActionOverWs3) {
    FacadeRig rig;
    rig.start();

    ASSERT_TRUE(rig.ncs->author().create("WS3-RO7", "SOAP Rundown").ok());
    ASSERT_TRUE(rig.ncs->author().commit("WS3-RO7").ok());
    ASSERT_TRUE(rig.waitFor([&] { return rig.device->runningOrderModel().has("WS3-RO7"); }));

    imos::profiles::StoryActionRequest request{};
    request.operation = imos::profiles::StoryAction::New;
    request.roID = "WS3-RO7";
    request.story = incs::RunningOrderAuthor::makeStory("s7", "Wire Story");
    ASSERT_TRUE(rig.device->requestStoryAction(request).ok());

    std::optional<imos::profiles::StoryActionResult> result;
    ASSERT_TRUE(rig.waitFor([&] {
        if (!result.has_value()) {
            result = rig.device->nextStoryActionResult();
        }
        return result.has_value();
    }));
    EXPECT_TRUE(result->ok) << result->roStatus;
    EXPECT_EQ(result->assignedStoryID, "s7");
    ASSERT_TRUE(rig.waitFor([&] {
        return rig.device->runningOrderModel().storyIDs("WS3-RO7") ==
               std::vector<std::string>{"s7"};
    }));

    rig.stop();
}

namespace {

class AdvancedVectorSource : public VectorObjectSource {
public:
    [[nodiscard]] bool supportsProfile3() const noexcept override { return true; }
    imos::Result<std::string> createObject(
        const imos::profiles::ObjectCreateRequest& request) override {
        const std::string objID = "WS3OBJ-" + std::to_string(nextID_++);
        Message stored = makeMessage(MessageType::MosObj);
        imos::codec::view::MosObj view{*stored.body()};
        view.setObjID(objID);
        view.setObjSlug(request.objSlug);
        view.setObjType(request.objType);
        view.setStatus("NEW");
        put(objID, std::move(stored));
        return objID;
    }
    imos::Result<imos::profiles::SchemaList> searchableSchemas() override {
        return imos::profiles::SchemaList{"http://ws3.example/schemas/meta1"};
    }

private:
    int nextID_ = 1;
};

}  // namespace

TEST(Ws3Transport, Profile3AdvancedWorkflowOverWs3) {
    FacadeRig rig;
    rig.start();
    AdvancedVectorSource source;
    source.put("W1", makeObject("W1", "UPDATED"));
    rig.device->attach(source);

    std::vector<imos::profiles::DirectoryEvent> dirEvents;
    std::vector<imos::profiles::DirectoryQueryPage> pages;
    const auto drain = [&] {
        while (auto directoryEvent = rig.ncs->nextDirectoryEvent()) {
            dirEvents.push_back(std::move(*directoryEvent));
        }
        while (auto page = rig.ncs->nextQueryPage()) {
            pages.push_back(std::move(*page));
        }
        return false;
    };

    // One exchange per logical channel before any completes — over 3.x they
    // share the single HTTP pipe but keep independent session sequencing.
    ASSERT_TRUE(rig.ncs->requestObject("W1").ok());
    ASSERT_TRUE(rig.ncs->author().create("WS3-RO3", "SOAP Rundown").ok());
    ASSERT_TRUE(rig.ncs->author().commit("WS3-RO3").ok());
    ASSERT_TRUE(rig.ncs->requestSearchableSchemas().ok());

    ASSERT_TRUE(rig.waitFor([&] { return drain() || rig.ncs->object("W1") != nullptr; }));
    ASSERT_TRUE(rig.waitFor([&] { return rig.device->runningOrderModel().has("WS3-RO3"); }));
    ASSERT_TRUE(rig.waitFor([&] {
        return rig.ncs->searchableSchemas() ==
               std::vector<std::string>{"http://ws3.example/schemas/meta1"};
    }));

    // The search family: a match-all mosReqObjList pages the catalog back
    // through the mosObjList result (mosList ⇄ list on the wire).
    imos::profiles::ObjectQuery query;
    query.queryID = "ws3-q1";
    ASSERT_TRUE(rig.ncs->queryObjects(query).ok());
    ASSERT_TRUE(rig.waitFor([&] { return drain() || !pages.empty(); }));
    EXPECT_EQ(pages.front().listReturnTotal, 1U);

    // Placeholder create end-to-end: the ACK in the mosObjCreate response
    // carries the objID in statusDescription, then the push call follows.
    ASSERT_TRUE(
        rig.ncs->createObject(imos::profiles::makeMosObjCreate("SOAP Fire", "VIDEO", "59.94"))
            .ok());
    ASSERT_TRUE(rig.waitFor([&] { return drain() || rig.ncs->object("WS3OBJ-1") != nullptr; }));
    bool sawCreated = false;
    for (const auto& event : dirEvents) {
        sawCreated =
            sawCreated || (event.type == imos::profiles::DirectoryEventType::ObjectCreated &&
                           event.objID == "WS3OBJ-1");
    }
    EXPECT_TRUE(sawCreated);

    rig.stop();
}
