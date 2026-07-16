// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// MOS 4.0 WebSocket transport integration over loopback (plan §13 M2 exit
// criteria): Profile 0 end-to-end in both roles, the §3.4 wire-encoding
// policy exercised on the wire (binary UCS-2 default, per-peer UTF-8 text,
// mismatches surfaced as named misconfigurations), Basic-auth acceptance
// and rejection, duplicate-channel-claim rejection, and passive-mode
// reconnect after a listener restart. Real sockets, real threads — the
// deterministic reliability logic is pinned separately in session tests.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ixwebsocket/IXGetFreePort.h>

#include <imos/codec/gen/views.h>
#include <imos/codec/message.h>
#include <imos/device.h>
#include <imos/profiles/profile0.h>
#include <imos/profiles/profile1.h>
#include <imos/session/in_memory_store.h>
#include <imos/session/peer_session.h>
#include <imos/transports/ws40.h>
#include <incs/ncs.h>

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
using imos::encoding::WireEncoding;
using imos::profiles::DeviceType;
using imos::profiles::MachineInfo;
using imos::profiles::parseListMachInfo;
using imos::profiles::Profile0Responder;
using imos::transports::ws40::ClientConfig;
using imos::transports::ws40::FixedCredentialVerifier;
using imos::transports::ws40::ServerConfig;
using imos::transports::ws40::ServerEvent;
using imos::transports::ws40::ServerPeerConfig;
using imos::transports::ws40::Ws40Client;
using imos::transports::ws40::Ws40Server;

namespace {

constexpr const char* kMosID = "vtr7.playout.example.mos";
constexpr const char* kNcsID = "ncs.example.com";

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

SessionConfig sessionConfig(WireEncoding encoding = WireEncoding::Ucs2Be) {
    SessionConfig config;
    config.mosID = kMosID;
    config.ncsID = kNcsID;
    config.retryTimeoutMillis = 2000;
    config.keepAliveIntervalMillis = 0;  // cadence is unit-tested; keep the wire quiet
    config.wireEncode.encoding = encoding;
    config.wireDecode.encoding = encoding;
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
    return info;
}

/// One device-role client + one NCS-role server pair over loopback, with
/// the pump plumbing both M2 topologies need.
struct Rig {
    SystemClock clock;
    imos::InMemoryMessageStore deviceStore;
    imos::InMemoryMessageStore ncsStore;

    std::unique_ptr<Ws40Server> server;
    std::unique_ptr<PeerSession> ncsSession;
    std::unique_ptr<Profile0Responder> ncsResponder;

    std::unique_ptr<Ws40Client> client;
    std::unique_ptr<PeerSession> deviceSession;
    std::unique_ptr<Profile0Responder> deviceResponder;

    std::vector<SessionEvent> deviceEvents;
    std::vector<SessionEvent> ncsEvents;
    std::vector<ServerEvent> rejectedEvents;
    std::vector<ServerEvent> serverProtocolErrors;
    std::vector<std::string> clientProtocolErrors;

    int port = 0;

    void startServer(WireEncoding peerEncoding = WireEncoding::Ucs2Be,
                     imos::transports::ws40::ICredentialVerifier* credentials = nullptr) {
        ServerConfig config;
        config.host = "127.0.0.1";
        config.port = port == 0 ? ix::getFreePort() : port;
        config.credentials = credentials;
        server = std::make_unique<Ws40Server>(config);
        ServerPeerConfig peer;
        peer.mosID = kMosID;
        peer.ncsID = kNcsID;
        peer.wireEncoding = peerEncoding;
        ASSERT_TRUE(server->addPeer(peer).ok());
        ASSERT_TRUE(server->start().ok());
        port = config.port;

        ncsSession = std::make_unique<PeerSession>(
            sessionConfig(peerEncoding),
            PeerSession::Dependencies{&clock, &server->transportFor(kMosID, kNcsID), &ncsStore});
        ncsResponder = std::make_unique<Profile0Responder>(machineInfo(DeviceType::Ncs),
                                                           ncsSession.get(), &clock);
    }

    void startClient(std::vector<Channel> channels, bool passive,
                     WireEncoding encoding = WireEncoding::Ucs2Be,
                     std::optional<imos::ws40::BasicCredentials> auth = std::nullopt) {
        ClientConfig config;
        config.endpointUrl = "ws://127.0.0.1:" + std::to_string(port) + "/mos/Communication";
        config.mosID = kMosID;
        config.ncsID = kNcsID;
        config.channels = std::move(channels);
        config.passive = passive;
        config.wireEncoding = encoding;
        config.auth = std::move(auth);
        config.reconnect.initialDelayMillis = 50;
        config.reconnect.maxDelayMillis = 200;
        client = std::make_unique<Ws40Client>(config, &clock);
        client->setErrorHandler([this](Channel, const std::string& diagnostic) {
            clientProtocolErrors.push_back(diagnostic);
        });
        deviceSession = std::make_unique<PeerSession>(
            sessionConfig(encoding), PeerSession::Dependencies{&clock, client.get(), &deviceStore});
        deviceResponder = std::make_unique<Profile0Responder>(machineInfo(DeviceType::Mos),
                                                              deviceSession.get(), &clock);
        ASSERT_TRUE(client->start().ok());
    }

    /// One scheduling turn for both roles: transport queues → sessions →
    /// responders, exactly the shape a pull-mode host would run.
    void pumpOnce() {
        if (client != nullptr && deviceSession != nullptr) {
            client->pump(*deviceSession);
            deviceSession->poll();
            while (auto event = deviceSession->nextEvent()) {
                if (deviceResponder == nullptr || !deviceResponder->handle(*event)) {
                    deviceEvents.push_back(std::move(*event));
                }
            }
        }
        if (server != nullptr) {
            while (auto event = server->nextEvent()) {
                switch (event->kind) {
                    case ServerEvent::Kind::ChannelConnected:
                        ncsSession->onChannelConnected(event->channel);
                        break;
                    case ServerEvent::Kind::ChannelDisconnected:
                        ncsSession->onChannelDisconnected(event->channel);
                        break;
                    case ServerEvent::Kind::Frame:
                        ncsSession->onFrameReceived(event->channel, event->frame);
                        break;
                    case ServerEvent::Kind::ProtocolError:
                        serverProtocolErrors.push_back(std::move(*event));
                        break;
                    case ServerEvent::Kind::Rejected:
                        rejectedEvents.push_back(std::move(*event));
                        break;
                }
            }
            if (ncsSession != nullptr) {
                ncsSession->poll();
                while (auto event = ncsSession->nextEvent()) {
                    if (ncsResponder == nullptr || !ncsResponder->handle(*event)) {
                        ncsEvents.push_back(std::move(*event));
                    }
                }
            }
        }
    }

    bool waitFor(const std::function<bool()>& predicate, int timeoutMillis = 5000) {
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

    bool waitForConnected(Channel channel) {
        return waitFor([&] { return client->isConnected(channel); });
    }
};

Message makeReqMachInfo() {
    return makeMessage(MessageType::ReqMachInfo);
}

const SessionEvent* findAck(const std::vector<SessionEvent>& events, MessageType replyType) {
    for (const SessionEvent& event : events) {
        if (event.type == SessionEventType::AckReceived && event.message.type() == replyType) {
            return &event;
        }
    }
    return nullptr;
}

}  // namespace

TEST(Ws40Transport, StandardTopologyProfile0BothRoles) {
    Rig rig;
    rig.startServer();
    rig.startClient({Channel::Ro}, /*passive=*/false);
    ASSERT_TRUE(rig.waitForConnected(Channel::Ro));

    // Device role asks; NCS role answers (reqMachInfo → listMachInfo).
    ASSERT_TRUE(rig.deviceSession->enqueue(Channel::Ro, makeReqMachInfo()).ok());
    ASSERT_TRUE(rig.waitFor(
        [&] { return findAck(rig.deviceEvents, MessageType::ListMachInfo) != nullptr; }));
    const SessionEvent* deviceAck = findAck(rig.deviceEvents, MessageType::ListMachInfo);
    const auto ncsInfo = parseListMachInfo(deviceAck->message);
    ASSERT_TRUE(ncsInfo.ok());
    EXPECT_EQ(ncsInfo.value().info.deviceType, DeviceType::Ncs);
    EXPECT_EQ(ncsInfo.value().info.model, "NCS-1");
    EXPECT_TRUE(ncsInfo.value().info.profiles[0]);

    // NCS role asks back down the same connection; device role answers.
    ASSERT_TRUE(rig.ncsSession->enqueue(Channel::Ro, makeReqMachInfo()).ok());
    ASSERT_TRUE(
        rig.waitFor([&] { return findAck(rig.ncsEvents, MessageType::ListMachInfo) != nullptr; }));
    const SessionEvent* ncsAck = findAck(rig.ncsEvents, MessageType::ListMachInfo);
    const auto deviceInfo = parseListMachInfo(ncsAck->message);
    ASSERT_TRUE(deviceInfo.ok());
    EXPECT_EQ(deviceInfo.value().info.deviceType, DeviceType::Mos);
    EXPECT_EQ(deviceInfo.value().info.model, "VTR-7");

    // Heartbeat exchange rides the session machinery over the real wire.
    Message heartbeat = makeMessage(MessageType::Heartbeat);
    ASSERT_TRUE(rig.ncsSession->enqueue(Channel::Ro, std::move(heartbeat)).ok());
    ASSERT_TRUE(
        rig.waitFor([&] { return findAck(rig.ncsEvents, MessageType::Heartbeat) != nullptr; }));
}

TEST(Ws40Transport, Utf8TextFramesPerPeerOptionWorksEndToEnd) {
    Rig rig;
    rig.startServer(WireEncoding::Utf8);
    rig.startClient({Channel::Ro}, /*passive=*/false, WireEncoding::Utf8);
    ASSERT_TRUE(rig.waitForConnected(Channel::Ro));

    ASSERT_TRUE(rig.deviceSession->enqueue(Channel::Ro, makeReqMachInfo()).ok());
    ASSERT_TRUE(rig.waitFor(
        [&] { return findAck(rig.deviceEvents, MessageType::ListMachInfo) != nullptr; }));
    EXPECT_TRUE(rig.serverProtocolErrors.empty());
    EXPECT_TRUE(rig.clientProtocolErrors.empty());
}

TEST(Ws40Transport, FrameTypeMismatchIsSurfacedNotDelivered) {
    // Client believes this relationship speaks UTF-8 (text frames); the
    // server expects the UCS-2 default (binary). §3.4: the receive side
    // surfaces the misconfiguration instead of guessing.
    Rig rig;
    rig.startServer(WireEncoding::Ucs2Be);
    rig.startClient({Channel::Ro}, /*passive=*/false, WireEncoding::Utf8);
    ASSERT_TRUE(rig.waitForConnected(Channel::Ro));

    ASSERT_TRUE(rig.deviceSession->enqueue(Channel::Ro, makeReqMachInfo()).ok());
    ASSERT_TRUE(rig.waitFor([&] { return !rig.serverProtocolErrors.empty(); }));
    EXPECT_NE(rig.serverProtocolErrors[0].diagnostic.find("text (UTF-8)"), std::string::npos);
    EXPECT_NE(rig.serverProtocolErrors[0].diagnostic.find("§3.4"), std::string::npos);
    EXPECT_TRUE(rig.ncsEvents.empty());  // never reached the session

    // And the reverse: the NCS replies in its configured binary UCS-2; the
    // UTF-8-configured client must flag rather than mis-decode it.
    ASSERT_TRUE(rig.ncsSession->enqueue(Channel::Ro, makeReqMachInfo()).ok());
    ASSERT_TRUE(rig.waitFor([&] { return !rig.clientProtocolErrors.empty(); }));
    EXPECT_NE(rig.clientProtocolErrors[0].find("binary"), std::string::npos);
}

TEST(Ws40Transport, BasicAuthAcceptAndReject) {
    FixedCredentialVerifier verifier{{"vtr7", "correct horse battery staple"}};
    Rig rig;
    rig.startServer(WireEncoding::Ucs2Be, &verifier);

    // Wrong password: rejected before any channel claim; never connects.
    rig.startClient({Channel::Ro}, false, WireEncoding::Ucs2Be,
                    imos::ws40::BasicCredentials{"vtr7", "wrong"});
    ASSERT_TRUE(rig.waitFor([&] { return !rig.rejectedEvents.empty(); }));
    EXPECT_NE(rig.rejectedEvents[0].diagnostic.find("invalid credentials"), std::string::npos);
    // The channel tears down asynchronously after the reject callback fires;
    // poll rather than sampling the instant the event lands (races under ASan).
    EXPECT_TRUE(rig.waitFor([&] { return !rig.client->isConnected(Channel::Ro); }));
    rig.client->stop();

    // Missing credentials: also rejected.
    rig.rejectedEvents.clear();
    rig.startClient({Channel::Ro}, false, WireEncoding::Ucs2Be, std::nullopt);
    ASSERT_TRUE(rig.waitFor([&] { return !rig.rejectedEvents.empty(); }));
    EXPECT_NE(rig.rejectedEvents[0].diagnostic.find("Authorization"), std::string::npos);
    rig.client->stop();

    // Correct credentials connect and traffic flows.
    rig.startClient({Channel::Ro}, false, WireEncoding::Ucs2Be,
                    imos::ws40::BasicCredentials{"vtr7", "correct horse battery staple"});
    ASSERT_TRUE(rig.waitForConnected(Channel::Ro));
    ASSERT_TRUE(rig.deviceSession->enqueue(Channel::Ro, makeReqMachInfo()).ok());
    ASSERT_TRUE(rig.waitFor(
        [&] { return findAck(rig.deviceEvents, MessageType::ListMachInfo) != nullptr; }));
}

TEST(Ws40Transport, DuplicateChannelClaimIsRejected) {
    Rig rig;
    rig.startServer();
    rig.startClient({Channel::Ro}, /*passive=*/false);
    ASSERT_TRUE(rig.waitForConnected(Channel::Ro));

    // A second connection claiming the same peer's ro channel is refused;
    // the first connection keeps working.
    ClientConfig second;
    second.endpointUrl = "ws://127.0.0.1:" + std::to_string(rig.port) + "/mos/Communication";
    second.mosID = kMosID;
    second.ncsID = kNcsID;
    second.channels = {Channel::Ro};
    SystemClock clock;
    Ws40Client duplicate{second, &clock};
    imos::InMemoryMessageStore store;
    PeerSession dummy{sessionConfig(), PeerSession::Dependencies{&clock, &duplicate, &store}};
    ASSERT_TRUE(duplicate.start().ok());
    ASSERT_TRUE(rig.waitFor([&] {
        duplicate.pump(dummy);
        return !rig.rejectedEvents.empty();
    }));
    EXPECT_NE(rig.rejectedEvents[0].diagnostic.find("already claimed"), std::string::npos);
    duplicate.stop();

    ASSERT_TRUE(rig.deviceSession->enqueue(Channel::Ro, makeReqMachInfo()).ok());
    ASSERT_TRUE(rig.waitFor(
        [&] { return findAck(rig.deviceEvents, MessageType::ListMachInfo) != nullptr; }));
}

TEST(Ws40Transport, UnknownPeerPairingIsRejected) {
    Rig rig;
    rig.startServer();
    ClientConfig config;
    config.endpointUrl = "ws://127.0.0.1:" + std::to_string(rig.port) + "/mos/Communication";
    config.mosID = "imposter.example.mos";
    config.ncsID = kNcsID;
    config.channels = {Channel::Ro};
    SystemClock clock;
    Ws40Client imposter{config, &clock};
    imos::InMemoryMessageStore store;
    PeerSession dummy{sessionConfig(), PeerSession::Dependencies{&clock, &imposter, &store}};
    ASSERT_TRUE(imposter.start().ok());
    ASSERT_TRUE(rig.waitFor([&] {
        imposter.pump(dummy);
        return !rig.rejectedEvents.empty();
    }));
    EXPECT_NE(rig.rejectedEvents[0].diagnostic.find("unknown mosID/ncsID"), std::string::npos);
    imposter.stop();
}

TEST(Ws40Transport, PassiveTopologyServerOriginatesAndClientReconnects) {
    Rig rig;
    rig.startServer();
    // Passive mode: the inner party opens every channel outward and holds
    // them; the external party originates back down those connections.
    rig.startClient({Channel::Mom, Channel::Ro, Channel::Aux}, /*passive=*/true);
    ASSERT_TRUE(rig.waitForConnected(Channel::Mom));
    ASSERT_TRUE(rig.waitForConnected(Channel::Ro));
    ASSERT_TRUE(rig.waitForConnected(Channel::Aux));

    // External (NCS) side originates on the held passive connection.
    ASSERT_TRUE(rig.ncsSession->enqueue(Channel::Ro, makeReqMachInfo()).ok());
    ASSERT_TRUE(
        rig.waitFor([&] { return findAck(rig.ncsEvents, MessageType::ListMachInfo) != nullptr; }));

    // Drop the listener: the inner party must re-establish "as quickly as
    // possible" once it returns (spec §3.1; ReconnectPolicy backoff). The
    // Ws40Server object survives the blip — its peer registry, transports,
    // and the NCS session's messageID sequence stay intact. (Recreating the
    // session would reset its messageID counter and the peer's dedupe
    // window would rightly suppress the reused IDs.)
    rig.server->stop();
    ASSERT_TRUE(rig.waitFor([&] { return !rig.client->isConnected(Channel::Ro); }));

    rig.ncsEvents.clear();
    ASSERT_TRUE(rig.server->start().ok());
    ASSERT_TRUE(rig.waitForConnected(Channel::Ro));

    ASSERT_TRUE(rig.ncsSession->enqueue(Channel::Ro, makeReqMachInfo()).ok());
    ASSERT_TRUE(
        rig.waitFor([&] { return findAck(rig.ncsEvents, MessageType::ListMachInfo) != nullptr; }));
}

// ---- Profile 1 object workflow over the real WebSocket transport ----------
//
// The imos::Device / incs::Ncs facades (Profile 0 + Profile 1) driven over a
// live loopback WebSocket: unsolicited push, single-object fetch (hit and NACK
// miss), and full-catalog batch sync — the plan §10 conformance walk re-run on
// real sockets (paced timing stays in the fake-clock unit suite).

namespace {

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

TEST(Ws40Transport, Profile1ObjectWorkflowOverWebSocket) {
    SystemClock clock;
    imos::InMemoryMessageStore deviceStore;
    imos::InMemoryMessageStore ncsStore;

    // NCS role on the server side.
    ServerConfig serverConfig;
    serverConfig.host = "127.0.0.1";
    serverConfig.port = ix::getFreePort();
    Ws40Server server{serverConfig};
    ServerPeerConfig peer;
    peer.mosID = kMosID;
    peer.ncsID = kNcsID;
    ASSERT_TRUE(server.addPeer(peer).ok());
    ASSERT_TRUE(server.start().ok());
    incs::Ncs ncs{
        {sessionConfig(), machineInfo(DeviceType::Ncs)},
        PeerSession::Dependencies{&clock, &server.transportFor(kMosID, kNcsID), &ncsStore}};

    // Device role on the client side, with a small object catalog.
    ClientConfig clientConfig;
    clientConfig.endpointUrl =
        "ws://127.0.0.1:" + std::to_string(serverConfig.port) + "/mos/Communication";
    clientConfig.mosID = kMosID;
    clientConfig.ncsID = kNcsID;
    clientConfig.channels = {Channel::Mom};
    Ws40Client client{clientConfig, &clock};
    imos::Device device{{sessionConfig(), machineInfo(DeviceType::Mos)},
                        PeerSession::Dependencies{&clock, &client, &deviceStore}};
    VectorObjectSource source;
    source.put("W2", makeObject("W2", "UPDATED"));
    source.put("W3", makeObject("W3", "UPDATED"));
    device.attach(source);
    ASSERT_TRUE(client.start().ok());

    std::vector<imos::profiles::DirectoryEvent> dirEvents;
    const auto pump = [&] {
        client.pump(device.session());
        device.poll();
        while (auto event = server.nextEvent()) {
            switch (event->kind) {
                case ServerEvent::Kind::ChannelConnected:
                    ncs.onChannelConnected(event->channel);
                    break;
                case ServerEvent::Kind::ChannelDisconnected:
                    ncs.onChannelDisconnected(event->channel);
                    break;
                case ServerEvent::Kind::Frame:
                    ncs.onFrameReceived(event->channel, event->frame);
                    break;
                default:
                    break;
            }
        }
        ncs.poll();
        while (auto directoryEvent = ncs.nextEvent()) {
            dirEvents.push_back(std::move(*directoryEvent));
        }
    };
    const auto waitFor = [&](const std::function<bool()>& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return false;
    };

    ASSERT_TRUE(waitFor([&] { return client.isConnected(Channel::Mom); }));

    // Push: an unsolicited object announcement is mirrored and acked.
    ASSERT_TRUE(device.announceObject(makeObject("P1", "NEW")).ok());
    ASSERT_TRUE(waitFor([&] { return ncs.object("P1") != nullptr; }));

    // Single fetch (hit): the full mosObj comes back for a catalog object.
    ASSERT_TRUE(ncs.requestObject("W2").ok());
    ASSERT_TRUE(waitFor([&] { return ncs.object("W2") != nullptr; }));

    // Single fetch (miss): a NACK surfaces as a RequestFailed directory event.
    ASSERT_TRUE(ncs.requestObject("NOPE").ok());
    ASSERT_TRUE(waitFor([&] {
        for (const auto& event : dirEvents) {
            if (event.type == imos::profiles::DirectoryEventType::RequestFailed &&
                event.objID == "NOPE") {
                return true;
            }
        }
        return false;
    }));
    EXPECT_EQ(ncs.object("NOPE"), nullptr);

    // Full sync (batch): the whole catalog arrives in one mosListAll.
    ASSERT_TRUE(ncs.requestAll(0).ok());
    ASSERT_TRUE(waitFor([&] { return ncs.object("W3") != nullptr; }));

    client.stop();
}

// ---- Profile 2 running-order workflow over the real WebSocket transport ----
//
// The authoring/diff walk re-run on real sockets on the ro channel: author →
// commit → the device's RunningOrderModel converges, a mutation converges
// after its ack round, and a device status report reaches the NCS. Resync,
// diff-quality, and failure branches stay in the fake-clock unit suite.

TEST(Ws40Transport, Profile2RunningOrderWorkflowOverWebSocket) {
    SystemClock clock;
    imos::InMemoryMessageStore deviceStore;
    imos::InMemoryMessageStore ncsStore;

    ServerConfig serverConfig;
    serverConfig.host = "127.0.0.1";
    serverConfig.port = ix::getFreePort();
    Ws40Server server{serverConfig};
    ServerPeerConfig peer;
    peer.mosID = kMosID;
    peer.ncsID = kNcsID;
    ASSERT_TRUE(server.addPeer(peer).ok());
    ASSERT_TRUE(server.start().ok());
    incs::Ncs ncs{
        {sessionConfig(), machineInfo(DeviceType::Ncs)},
        PeerSession::Dependencies{&clock, &server.transportFor(kMosID, kNcsID), &ncsStore}};

    ClientConfig clientConfig;
    clientConfig.endpointUrl =
        "ws://127.0.0.1:" + std::to_string(serverConfig.port) + "/mos/Communication";
    clientConfig.mosID = kMosID;
    clientConfig.ncsID = kNcsID;
    clientConfig.channels = {Channel::Ro};
    Ws40Client client{clientConfig, &clock};
    imos::Device device{{sessionConfig(), machineInfo(DeviceType::Mos)},
                        PeerSession::Dependencies{&clock, &client, &deviceStore}};
    ASSERT_TRUE(client.start().ok());

    std::vector<imos::profiles::ElementStatusReport> statuses;
    const auto pump = [&] {
        client.pump(device.session());
        device.poll();
        while (auto event = server.nextEvent()) {
            switch (event->kind) {
                case ServerEvent::Kind::ChannelConnected:
                    ncs.onChannelConnected(event->channel);
                    break;
                case ServerEvent::Kind::ChannelDisconnected:
                    ncs.onChannelDisconnected(event->channel);
                    break;
                case ServerEvent::Kind::Frame:
                    ncs.onFrameReceived(event->channel, event->frame);
                    break;
                default:
                    break;
            }
        }
        ncs.poll();
        while (auto status = ncs.nextElementStatus()) {
            statuses.push_back(std::move(*status));
        }
    };
    const auto waitFor = [&](const std::function<bool()>& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
        return false;
    };

    ASSERT_TRUE(waitFor([&] { return client.isConnected(Channel::Ro); }));

    // Author and commit; the device's shadow model converges over the wire.
    ASSERT_TRUE(ncs.author().create("WS-RO", "WS Rundown").ok());
    imos::codec::XmlNode story = incs::RunningOrderAuthor::makeStory("s1", "Lead");
    story.children.push_back(incs::RunningOrderAuthor::makeItem("i1", "OBJ1", kMosID));
    ASSERT_TRUE(ncs.author().insertStory("WS-RO", story).ok());
    ASSERT_TRUE(ncs.author().insertStory("WS-RO", incs::RunningOrderAuthor::makeStory("s2")).ok());
    ASSERT_TRUE(ncs.author().commit("WS-RO").ok());
    ASSERT_TRUE(waitFor([&] {
        return device.runningOrderModel().storyIDs("WS-RO") == std::vector<std::string>{"s1", "s2"};
    }));

    // A mutation converges after its ack round.
    ASSERT_TRUE(ncs.author().moveStory("WS-RO", "s2", "s1").ok());
    ASSERT_TRUE(ncs.author().commit("WS-RO").ok());
    ASSERT_TRUE(waitFor([&] {
        return device.runningOrderModel().storyIDs("WS-RO") == std::vector<std::string>{"s2", "s1"};
    }));

    // Device-originated roElementStat reaches the NCS.
    imos::profiles::ElementStatusReport play;
    play.element = imos::profiles::RoElement::Item;
    play.roID = "WS-RO";
    play.storyID = "s1";
    play.itemID = "i1";
    play.status = "PLAY";
    ASSERT_TRUE(device.reportElementStatus(play).ok());
    ASSERT_TRUE(waitFor([&] { return !statuses.empty(); }));
    EXPECT_EQ(statuses.front().status, "PLAY");
    EXPECT_EQ(statuses.front().itemID, "i1");

    client.stop();
}

// ---- Profile 4 over real sockets (plan §13 M4 exit criterion) -------------
//
// The NewsML-preservation gate on the wire: an agency story authored as a
// Read1stMEMasBody payload crosses a real WebSocket in each §3.4 wire
// encoding — UCS-2 BE binary frames and UTF-8 text frames — and the bytes
// the device's story sink sees must equal the authored bytes exactly.
// Hydration, resync, and the diff rule stay in the fake-clock unit suite.

namespace {

/// Minimal capturing sink: serialized agency-MEM bytes per accepted story.
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

}  // namespace

TEST(Ws40Transport, Profile4AgencyPayloadPreservedOverWebSocketBothEncodings) {
    for (const WireEncoding encoding : {WireEncoding::Ucs2Be, WireEncoding::Utf8}) {
        SCOPED_TRACE(imos::encoding::wireEncodingName(encoding));
        SystemClock clock;
        imos::InMemoryMessageStore deviceStore;
        imos::InMemoryMessageStore ncsStore;

        ServerConfig serverConfig;
        serverConfig.host = "127.0.0.1";
        serverConfig.port = ix::getFreePort();
        Ws40Server server{serverConfig};
        ServerPeerConfig peer;
        peer.mosID = kMosID;
        peer.ncsID = kNcsID;
        peer.wireEncoding = encoding;
        ASSERT_TRUE(server.addPeer(peer).ok());
        ASSERT_TRUE(server.start().ok());
        incs::Ncs ncs{
            {sessionConfig(encoding), machineInfo(DeviceType::Ncs)},
            PeerSession::Dependencies{&clock, &server.transportFor(kMosID, kNcsID), &ncsStore}};

        ClientConfig clientConfig;
        clientConfig.endpointUrl =
            "ws://127.0.0.1:" + std::to_string(serverConfig.port) + "/mos/Communication";
        clientConfig.mosID = kMosID;
        clientConfig.ncsID = kNcsID;
        clientConfig.channels = {Channel::Ro};
        clientConfig.wireEncoding = encoding;
        Ws40Client client{clientConfig, &clock};
        imos::Device device{{sessionConfig(encoding), machineInfo(DeviceType::Mos)},
                            PeerSession::Dependencies{&clock, &client, &deviceStore}};
        CapturingStorySink sink;
        device.attach(sink);
        ASSERT_TRUE(client.start().ok());

        const auto pump = [&] {
            client.pump(device.session());
            device.poll();
            while (auto event = server.nextEvent()) {
                switch (event->kind) {
                    case ServerEvent::Kind::ChannelConnected:
                        ncs.onChannelConnected(event->channel);
                        break;
                    case ServerEvent::Kind::ChannelDisconnected:
                        ncs.onChannelDisconnected(event->channel);
                        break;
                    case ServerEvent::Kind::Frame:
                        ncs.onFrameReceived(event->channel, event->frame);
                        break;
                    default:
                        break;
                }
            }
            ncs.poll();
        };
        const auto waitFor = [&](const std::function<bool()>& predicate) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds{5000};
            while (std::chrono::steady_clock::now() < deadline) {
                pump();
                if (predicate()) {
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{2});
            }
            return false;
        };

        ASSERT_TRUE(waitFor([&] { return client.isConnected(Channel::Ro); }));

        ASSERT_TRUE(ncs.author().create("WS-RO4", "WS Rundown").ok());
        ASSERT_TRUE(
            ncs.author().insertStory("WS-RO4", incs::RunningOrderAuthor::makeStory("s1")).ok());
        ASSERT_TRUE(ncs.author().commit("WS-RO4").ok());
        ASSERT_TRUE(waitFor([&] { return device.runningOrderModel().has("WS-RO4"); }));

        // Multi-kilobyte NewsML with non-ASCII BMP characters, parsed into a
        // retained MEM subtree exactly as an NCS host would author it.
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

        ASSERT_TRUE(ncs.author().setAgencyStoryBody("WS-RO4", "s1", metadata).ok());
        ASSERT_TRUE(ncs.sendStory("WS-RO4", "s1").ok());
        ASSERT_TRUE(waitFor([&] { return !sink.agencyBytes.empty(); }));
        EXPECT_EQ(sink.agencyBytes.front(), authoredBytes);

        client.stop();
    }
}

// ---- WASM client smoke (plan §13 M2: "P0 conformance … + WASM client") ----
//
// Runs the Ws40EmscriptenClient (built by the wasm preset as
// imos-ws40-wasm-smoke) under node against this process's Ws40Server, in
// passive mode: the NCS originates reqMachInfo down the held connection and
// the WASM device answers. Skips when the wasm artifact or a
// WebSocket-capable node is unavailable; CI wires both.

namespace {

std::string wasmSmokeJsPath() {
    if (const char* env = std::getenv("IMOS_WASM_SMOKE_JS")) {
        return env;
    }
    return std::string{IMOS_SOURCE_DIR} + "/build/wasm/bin/imos-ws40-wasm-smoke.js";
}

std::string nodeBinary() {
    if (const char* env = std::getenv("IMOS_NODE")) {
        return env;
    }
    if (const char* emsdk = std::getenv("EMSDK")) {
        namespace fs = std::filesystem;
        const fs::path root = fs::path{emsdk} / "node";
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator{root, ec}) {
            const fs::path candidate = entry.path() / "bin" / "node";
            if (fs::exists(candidate, ec)) {
                return candidate.string();
            }
        }
    }
    return "node";
}

}  // namespace

TEST(Ws40Transport, WasmClientProfile0Smoke) {
    // Locally, missing prerequisites skip; in CI (IMOS_REQUIRE_WASM_SMOKE=1)
    // they fail — a vacuously green smoke job would hide regressions.
    const bool required = std::getenv("IMOS_REQUIRE_WASM_SMOKE") != nullptr;
    const std::string js = wasmSmokeJsPath();
    if (!std::filesystem::exists(js)) {
        ASSERT_FALSE(required) << "wasm smoke client missing at " << js;
        GTEST_SKIP() << "wasm smoke client not built (expected " << js
                     << "); build the wasm preset first";
    }
    const std::string node = nodeBinary();
    const bool nodeRuns = std::system((node + " --version >/dev/null 2>&1").c_str()) == 0;
    if (!nodeRuns) {
        ASSERT_FALSE(required) << "no usable node (tried " << node << ")";
        GTEST_SKIP() << "node not available (set IMOS_NODE or EMSDK)";
    }
    // Node ≥22 has WebSocket by default; 20/21 need the experimental flag.
    const std::string wsFlag =
        std::system((node + " --experimental-websocket -e 0 >/dev/null 2>&1").c_str()) == 0
            ? " --experimental-websocket"
            : "";

    Rig rig;
    rig.startServer();

    std::atomic<int> nodeExit{-1};
    std::thread runner{[&] {
        const std::string command =
            node + wsFlag + " " + js + " " + std::to_string(rig.port) + " >/dev/null 2>&1";
        nodeExit = std::system(command.c_str());
    }};

    // The WASM device connects passively; the NCS originates the request.
    imos::ITransport& toDevice = rig.server->transportFor(kMosID, kNcsID);
    ASSERT_TRUE(rig.waitFor([&] { return toDevice.isConnected(Channel::Ro); }, 15000));
    ASSERT_TRUE(rig.ncsSession->enqueue(Channel::Ro, makeReqMachInfo()).ok());
    ASSERT_TRUE(rig.waitFor(
        [&] { return findAck(rig.ncsEvents, MessageType::ListMachInfo) != nullptr; }, 15000));

    const SessionEvent* ack = findAck(rig.ncsEvents, MessageType::ListMachInfo);
    const auto info = parseListMachInfo(ack->message);
    ASSERT_TRUE(info.ok());
    EXPECT_EQ(info.value().info.model, "WASM-1");
    EXPECT_EQ(info.value().info.deviceType, DeviceType::Mos);

    runner.join();
    EXPECT_EQ(nodeExit.load(), 0) << "wasm client did not exit cleanly";
}

// ---- TLS (wss) ------------------------------------------------------------
//
// The spec's certificate policy (plan §12): CA-issued always accepted,
// self-signed acceptance as an explicit switch, optional pinning on top.
// Compiled unconditionally; skips on builds without TLS (IMOS_ENABLE_TLS=OFF
// — the local no-OpenSSL configuration) and runs under the linux-tls preset
// and in CI. Certificates are generated with the openssl CLI.

#include <imos/transports.h>

namespace {

struct TlsCerts {
    std::string dir;
    std::string cert;
    std::string key;
    bool ok = false;
};

TlsCerts makeSelfSignedCert() {
    TlsCerts certs;
    certs.dir = ::testing::TempDir() + "imos-ws40-tls";
    std::filesystem::create_directories(certs.dir);
    certs.cert = certs.dir + "/cert.pem";
    certs.key = certs.dir + "/key.pem";
    const std::string command = "openssl req -x509 -newkey rsa:2048 -keyout '" + certs.key +
                                "' -out '" + certs.cert +
                                "' -days 1 -nodes -subj '/CN=127.0.0.1' "
                                "-addext 'subjectAltName = IP:127.0.0.1' >/dev/null 2>&1";
    certs.ok = std::system(command.c_str()) == 0;
    return certs;
}

}  // namespace

TEST(Ws40Transport, TlsSelfSignedPolicyAndPinning) {
    if (!imos::transports::tlsEnabled()) {
        GTEST_SKIP() << "built without TLS (IMOS_ENABLE_TLS=OFF); use the linux-tls preset";
    }
    const TlsCerts certs = makeSelfSignedCert();
    if (!certs.ok) {
        GTEST_SKIP() << "openssl CLI unavailable; cannot generate a test certificate";
    }

    Rig rig;
    {
        ServerConfig config;
        config.host = "127.0.0.1";
        config.port = ix::getFreePort();
        config.tls.enabled = true;
        config.tls.certFile = certs.cert;
        config.tls.keyFile = certs.key;
        rig.server = std::make_unique<Ws40Server>(config);
        ServerPeerConfig peer;
        peer.mosID = kMosID;
        peer.ncsID = kNcsID;
        ASSERT_TRUE(rig.server->addPeer(peer).ok());
        ASSERT_TRUE(rig.server->start().ok());
        rig.port = config.port;
        rig.ncsSession = std::make_unique<PeerSession>(
            sessionConfig(),
            PeerSession::Dependencies{&rig.clock, &rig.server->transportFor(kMosID, kNcsID),
                                      &rig.ncsStore});
        rig.ncsResponder = std::make_unique<Profile0Responder>(machineInfo(DeviceType::Ncs),
                                                               rig.ncsSession.get(), &rig.clock);
    }
    const std::string endpoint =
        "wss://127.0.0.1:" + std::to_string(rig.port) + "/mos/Communication";

    // The spec-recommended disable option (allowSelfSignedPeers=false,
    // CA bundle only): a self-signed listener must be refused.
    {
        ClientConfig config;
        config.endpointUrl = endpoint;
        config.mosID = kMosID;
        config.ncsID = kNcsID;
        config.channels = {Channel::Ro};
        config.tls.allowSelfSignedPeers = false;
        SystemClock clock;
        Ws40Client strict{config, &clock};
        imos::InMemoryMessageStore store;
        PeerSession session{sessionConfig(), PeerSession::Dependencies{&clock, &strict, &store}};
        ASSERT_TRUE(strict.start().ok());
        EXPECT_FALSE(rig.waitFor(
            [&] {
                strict.pump(session);
                return strict.isConnected(Channel::Ro);
            },
            1000));
        strict.stop();
    }

    // The default admits self-signed certificates (the spec expects MOS
    // devices/NCSes to accept them), and Profile 0 flows over wss.
    {
        ClientConfig config;
        config.endpointUrl = endpoint;
        config.mosID = kMosID;
        config.ncsID = kNcsID;
        config.channels = {Channel::Ro};
        rig.client = std::make_unique<Ws40Client>(config, &rig.clock);
        rig.deviceSession = std::make_unique<PeerSession>(
            sessionConfig(),
            PeerSession::Dependencies{&rig.clock, rig.client.get(), &rig.deviceStore});
        rig.deviceResponder = std::make_unique<Profile0Responder>(
            machineInfo(DeviceType::Mos), rig.deviceSession.get(), &rig.clock);
        ASSERT_TRUE(rig.client->start().ok());
        ASSERT_TRUE(rig.waitForConnected(Channel::Ro));
        ASSERT_TRUE(rig.deviceSession->enqueue(Channel::Ro, makeReqMachInfo()).ok());
        ASSERT_TRUE(rig.waitFor(
            [&] { return findAck(rig.deviceEvents, MessageType::ListMachInfo) != nullptr; }));
        rig.client->stop();
        rig.client.reset();
        rig.deviceSession.reset();
    }

    // Pinning: trust exactly this certificate (hostname validation off —
    // OpenSSL matches IP SANs via a different path than hostnames, and the
    // pin itself is the trust decision here).
    {
        ClientConfig config;
        config.endpointUrl = endpoint;
        config.mosID = kMosID;
        config.ncsID = kNcsID;
        config.channels = {Channel::Ro};
        config.tls.pinnedPeerCertFile = certs.cert;
        config.tls.disableHostnameValidation = true;
        rig.client = std::make_unique<Ws40Client>(config, &rig.clock);
        rig.deviceSession = std::make_unique<PeerSession>(
            sessionConfig(),
            PeerSession::Dependencies{&rig.clock, rig.client.get(), &rig.deviceStore});
        rig.deviceResponder = std::make_unique<Profile0Responder>(
            machineInfo(DeviceType::Mos), rig.deviceSession.get(), &rig.clock);
        ASSERT_TRUE(rig.client->start().ok());
        ASSERT_TRUE(rig.waitForConnected(Channel::Ro));
    }
}
