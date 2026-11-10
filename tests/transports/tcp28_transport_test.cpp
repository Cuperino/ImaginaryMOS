// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// MOS 2.8.x TCP transport integration over loopback (plan §13 M6, design of
// record reference/m6-plan.md §1.3): the classic port scheme with real
// sockets — device dials mom (the NCS's lower-port listener), NCS dials ro
// (the device's upper-port listener) — Profile 0 both directions, the
// legacy-13 accept-and-normalize battery on the wire, the 2.8.5 tolerance
// rules (discrete socket per message, split and coalesced stream delivery),
// redial after a listener restart, and conformance re-runs of every wire
// profile over TCP: P1/P2 workflows, the P3 three-channel walk with real
// query-port traffic, the P4 agency-byte gate in both UCS-2 byte orders,
// P5 item control, and P7 story actions. Deterministic deframer/reliability
// logic stays in the fake-clock unit suites.

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/gen/views.h>
#include <imos/codec/message.h>
#include <imos/device.h>
#include <imos/encoding/encoding.h>
#include <imos/profiles/profile0.h>
#include <imos/profiles/profile1.h>
#include <imos/session/in_memory_store.h>
#include <imos/session/peer_session.h>
#include <imos/transports/tcp28.h>
#include <incs/ncs.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
using imos::profiles::DeviceType;
using imos::profiles::MachineInfo;
using imos::profiles::Profile0Responder;
using imos::transports::tcp28::ChannelPorts;
using imos::transports::tcp28::EndpointConfig;
using imos::transports::tcp28::Role;
using imos::transports::tcp28::Tcp28Endpoint;
using imos::transports::tcp28::Topology;

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

SessionConfig sessionConfig(
    imos::encoding::WireEncoding encoding = imos::encoding::WireEncoding::Ucs2Be) {
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
    info.profiles[1] = true;
    info.profiles[2] = true;
    return info;
}

/// The 2.8 ports are fixed by deployment configuration, so tests must pick
/// free ones up front (both endpoints need the pair at construction). Bind
/// an ephemeral listener, note the port, release it — same probe race the
/// ws40 server documents; acceptable over loopback.
int pickFreePort(imos::IClock& clock) {
    EndpointConfig config;
    config.role = Role::Ncs;  // listens on mom
    config.bindAddress = "127.0.0.1";
    config.ports.mom = 0;
    Tcp28Endpoint probe{config, &clock};
    if (!probe.start().ok()) {
        return 0;
    }
    const int port = probe.listenPort(Channel::Mom).value_or(0);
    probe.stop();
    return port;
}

/// Two endpoints on one host cannot share the site-wide port numbers: each
/// listens on its own mom/ro pair and dials the other's. `primary` holds the
/// traditional listeners (NCS mom, device ro/aux); the reverse pair (device
/// mom, NCS ro) is picked here.
struct PortPlan {
    ChannelPorts deviceListen;
    ChannelPorts ncsListen;
};

PortPlan planPorts(const ChannelPorts& primary, imos::IClock& clock) {
    PortPlan plan;
    plan.ncsListen.mom = primary.mom;
    plan.ncsListen.ro = pickFreePort(clock);
    plan.deviceListen.mom = pickFreePort(clock);
    plan.deviceListen.ro = primary.ro;
    plan.deviceListen.aux = primary.aux;
    return plan;
}

std::vector<std::uint8_t> encodeWire(const std::string& utf8) {
    imos::Result<std::vector<std::uint8_t>> wire = imos::encoding::encodeFromUtf8(utf8, {});
    EXPECT_TRUE(wire.ok());
    return wire.ok() ? std::move(wire).value() : std::vector<std::uint8_t>{};
}

std::string envelope(const std::string& messageId, const std::string& body) {
    return "<mos><mosID>" + std::string{kMosID} + "</mosID><ncsID>" + std::string{kNcsID} +
           "</ncsID><messageID>" + messageId + "</messageID>" + body + "</mos>";
}

/// Device + NCS PeerSessions over a real loopback TCP pair in the classic
/// port scheme, with Profile 0 responders on both ends.
struct Rig {
    SystemClock clock;
    imos::InMemoryMessageStore deviceStore;
    imos::InMemoryMessageStore ncsStore;

    ChannelPorts ports;
    // Per-test knobs, set before start().
    bool startNcs = true;                          ///< false: the device endpoint stands alone
    Topology ncsTopology = Topology::FourSockets;  ///< the NCS's socket scheme
    bool deviceAcceptOnlyPeerHost = true;
    std::unique_ptr<Tcp28Endpoint> deviceEndpoint;
    std::unique_ptr<Tcp28Endpoint> ncsEndpoint;
    std::unique_ptr<PeerSession> deviceSession;
    std::unique_ptr<PeerSession> ncsSession;
    std::unique_ptr<Profile0Responder> deviceResponder;
    std::unique_ptr<Profile0Responder> ncsResponder;

    std::vector<SessionEvent> deviceEvents;
    std::vector<SessionEvent> ncsEvents;
    std::vector<std::pair<Channel, std::string>> deviceDiagnostics;
    std::vector<std::pair<Channel, std::string>> ncsDiagnostics;

    void start(bool withResponders = true, bool withAux = false) {
        ports.mom = pickFreePort(clock);
        ports.ro = pickFreePort(clock);
        ASSERT_NE(ports.mom, 0);
        ASSERT_NE(ports.ro, 0);
        if (withAux) {
            ports.aux = pickFreePort(clock);
            ASSERT_NE(ports.aux, 0);
        }

        EndpointConfig ncsConfig;
        ncsConfig.role = Role::Ncs;
        ncsConfig.peerHost = "127.0.0.1";
        ncsConfig.bindAddress = "127.0.0.1";
        const PortPlan plan = planPorts(ports, clock);
        ncsConfig.ports = plan.ncsListen;
        ncsConfig.peerPorts = plan.deviceListen;
        ncsConfig.enableAux = withAux;
        ncsConfig.topology = ncsTopology;
        ncsConfig.reconnect.initialDelayMillis = 50;
        ncsConfig.reconnect.maxDelayMillis = 200;
        if (startNcs) {
            ncsEndpoint = std::make_unique<Tcp28Endpoint>(ncsConfig, &clock);
            ncsEndpoint->setErrorHandler([this](Channel channel, const std::string& diagnostic) {
                ncsDiagnostics.emplace_back(channel, diagnostic);
            });
            ncsSession = std::make_unique<PeerSession>(
                sessionConfig(), PeerSession::Dependencies{&clock, ncsEndpoint.get(), &ncsStore});
            if (withResponders) {
                ncsResponder = std::make_unique<Profile0Responder>(machineInfo(DeviceType::Ncs),
                                                                   ncsSession.get(), &clock);
            }
        }

        EndpointConfig deviceConfig;
        deviceConfig.role = Role::Device;
        deviceConfig.peerHost = "127.0.0.1";
        deviceConfig.bindAddress = "127.0.0.1";
        deviceConfig.ports = plan.deviceListen;
        deviceConfig.peerPorts = plan.ncsListen;
        deviceConfig.enableAux = withAux;
        deviceConfig.acceptOnlyPeerHost = deviceAcceptOnlyPeerHost;
        deviceConfig.reconnect.initialDelayMillis = 50;
        deviceConfig.reconnect.maxDelayMillis = 200;
        deviceEndpoint = std::make_unique<Tcp28Endpoint>(deviceConfig, &clock);
        deviceEndpoint->setErrorHandler([this](Channel channel, const std::string& diagnostic) {
            deviceDiagnostics.emplace_back(channel, diagnostic);
        });
        deviceSession = std::make_unique<PeerSession>(
            sessionConfig(), PeerSession::Dependencies{&clock, deviceEndpoint.get(), &deviceStore});
        if (withResponders) {
            deviceResponder = std::make_unique<Profile0Responder>(machineInfo(DeviceType::Mos),
                                                                  deviceSession.get(), &clock);
        }

        if (startNcs) {
            ASSERT_TRUE(ncsEndpoint->start().ok());
        }
        ASSERT_TRUE(deviceEndpoint->start().ok());
    }

    void pumpOnce() {
        if (deviceEndpoint != nullptr && deviceSession != nullptr) {
            deviceEndpoint->pump(*deviceSession);
            deviceSession->poll();
            while (auto event = deviceSession->nextEvent()) {
                if (deviceResponder == nullptr || !deviceResponder->handle(*event)) {
                    deviceEvents.push_back(std::move(*event));
                }
            }
        }
        if (ncsEndpoint != nullptr && ncsSession != nullptr) {
            ncsEndpoint->pump(*ncsSession);
            ncsSession->poll();
            while (auto event = ncsSession->nextEvent()) {
                if (ncsResponder == nullptr || !ncsResponder->handle(*event)) {
                    ncsEvents.push_back(std::move(*event));
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

    bool waitForLink() {
        return waitFor([&] {
            return deviceEndpoint->isConnected(Channel::Mom) &&
                   deviceEndpoint->isConnected(Channel::Ro) &&
                   ncsEndpoint->isConnected(Channel::Mom) && ncsEndpoint->isConnected(Channel::Ro);
        });
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

int countReceived(const std::vector<SessionEvent>& events, MessageType type) {
    int count = 0;
    for (const SessionEvent& event : events) {
        if (event.type == SessionEventType::MessageReceived && event.message.type() == type) {
            ++count;
        }
    }
    return count;
}

}  // namespace

// The classic port scheme end to end: the device's dialed mom channel and
// the NCS's dialed ro channel both come up, and Profile 0 request/reply
// works in each direction — replies riding the arrival socket (the 2.8.5
// ack-on-same-socket rule is what routes them).
TEST(Tcp28Transport, ClassicPortSchemeProfile0BothRoles) {
    Rig rig;
    rig.start();
    ASSERT_TRUE(rig.waitForLink());

    ASSERT_TRUE(
        rig.deviceSession->enqueue(Channel::Mom, makeMessage(MessageType::ReqMachInfo)).ok());
    ASSERT_TRUE(rig.waitFor(
        [&] { return findAck(rig.deviceEvents, MessageType::ListMachInfo) != nullptr; }));

    ASSERT_TRUE(rig.ncsSession->enqueue(Channel::Ro, makeMessage(MessageType::ReqMachInfo)).ok());
    ASSERT_TRUE(
        rig.waitFor([&] { return findAck(rig.ncsEvents, MessageType::ListMachInfo) != nullptr; }));

    rig.deviceEndpoint->stop();
    rig.ncsEndpoint->stop();
}

// The thirteen removed messages, written raw onto the ro stream in UCS-2 BE
// exactly as a 2.8-era NCS would: every one is accepted and normalized onto
// roElementAction/roElementStat before the application sees it, with the
// normalization diagnostic attached (plan §3.2 policy).
TEST(Tcp28Transport, LegacyThirteenNormalizedOnTheWire) {
    Rig rig;
    rig.start(/*withResponders=*/false);
    ASSERT_TRUE(rig.waitForLink());

    const std::vector<std::string> legacyBodies = {
        "<roStoryAppend><roID>R</roID><story><storyID>S9</storyID></story></roStoryAppend>",
        "<roStoryInsert><roID>R</roID><storyID>S2</storyID>"
        "<story><storyID>S9</storyID></story></roStoryInsert>",
        "<roStoryReplace><roID>R</roID><storyID>S2</storyID>"
        "<story><storyID>S9</storyID></story></roStoryReplace>",
        "<roStoryMove><roID>R</roID><storyID>S7</storyID><storyID>S2</storyID></roStoryMove>",
        "<roStoryMoveMultiple><roID>R</roID><storyID>A</storyID><storyID>B</storyID>"
        "<storyID>T</storyID></roStoryMoveMultiple>",
        "<roStorySwap><roID>R</roID><storyID>S3</storyID><storyID>S5</storyID></roStorySwap>",
        "<roStoryDelete><roID>R</roID><storyID>S3</storyID><storyID>S5</storyID></roStoryDelete>",
        "<roItemInsert><roID>R</roID><storyID>S2</storyID><itemID>I4</itemID>"
        "<item><itemID>I9</itemID></item></roItemInsert>",
        "<roItemReplace><roID>R</roID><storyID>S2</storyID><itemID>I4</itemID>"
        "<item><itemID>I9</itemID></item></roItemReplace>",
        "<roItemMoveMultiple><roID>R</roID><storyID>S2</storyID><itemID>I1</itemID>"
        "<itemID>I2</itemID><itemID>I9</itemID></roItemMoveMultiple>",
        "<roItemDelete><roID>R</roID><storyID>S2</storyID><itemID>I1</itemID>"
        "<itemID>I2</itemID></roItemDelete>",
        "<roStat><roID>R</roID><status>PLAY</status><time>2009-04-11T14:13:53</time></roStat>",
        "<roItemStat><roID>R</roID><storyID>S</storyID><itemID>I</itemID><objID>O</objID>"
        "<itemChannel>B</itemChannel><status>PLAY</status><time>T</time></roItemStat>",
    };

    int messageId = 100;
    for (const std::string& body : legacyBodies) {
        const std::vector<std::uint8_t> wire =
            encodeWire(envelope(std::to_string(++messageId), body));
        ASSERT_TRUE(rig.ncsEndpoint->send(Channel::Ro, wire).ok());
    }

    ASSERT_TRUE(rig.waitFor([&] {
        return countReceived(rig.deviceEvents, MessageType::RoElementAction) +
                   countReceived(rig.deviceEvents, MessageType::RoElementStat) ==
               static_cast<int>(legacyBodies.size());
    }));
    EXPECT_EQ(countReceived(rig.deviceEvents, MessageType::RoElementAction), 11);
    EXPECT_EQ(countReceived(rig.deviceEvents, MessageType::RoElementStat), 2);
    for (const SessionEvent& event : rig.deviceEvents) {
        if (event.type == SessionEventType::MessageReceived) {
            EXPECT_NE(event.diagnostic.find("normalized from legacy"), std::string::npos)
                << event.diagnostic;
        }
    }

    rig.deviceEndpoint->stop();
    rig.ncsEndpoint->stop();
}

// 2.8.5 explicitly tolerates "each message arrives in a separate, discrete
// socket session": drop the ro connection between messages and both still
// reach the application. (Reply-on-arrival-socket routing is covered by the
// Profile 0 test — the ListMachInfo ack rides the dialed socket back.)
TEST(Tcp28Transport, DiscreteSocketPerMessageIsTolerated) {
    Rig rig;
    rig.start(/*withResponders=*/false);
    ASSERT_TRUE(rig.waitForLink());

    const std::string readyToAir =
        "<roReadyToAir><roID>R</roID><roAir>READY</roAir>"
        "</roReadyToAir>";
    ASSERT_TRUE(rig.ncsEndpoint->send(Channel::Ro, encodeWire(envelope("201", readyToAir))).ok());
    ASSERT_TRUE(rig.waitFor(
        [&] { return countReceived(rig.deviceEvents, MessageType::RoReadyToAir) == 1; }));

    // Fresh socket session for the next message.
    rig.ncsEndpoint->stop();
    ASSERT_TRUE(rig.waitFor([&] { return !rig.deviceEndpoint->isConnected(Channel::Ro); }));
    ASSERT_TRUE(rig.ncsEndpoint->start().ok());
    ASSERT_TRUE(rig.waitFor([&] { return rig.ncsEndpoint->isConnected(Channel::Ro); }));

    ASSERT_TRUE(rig.ncsEndpoint->send(Channel::Ro, encodeWire(envelope("202", readyToAir))).ok());
    ASSERT_TRUE(rig.waitFor(
        [&] { return countReceived(rig.deviceEvents, MessageType::RoReadyToAir) == 2; }));

    rig.deviceEndpoint->stop();
    rig.ncsEndpoint->stop();
}

// No length framing on a 2.8 stream: documents split across arbitrary
// writes and coalesced back-to-back in one write must both deframe cleanly.
TEST(Tcp28Transport, SplitAndCoalescedStreamDelivery) {
    Rig rig;
    rig.start(/*withResponders=*/false);
    ASSERT_TRUE(rig.waitForLink());

    const auto readyToAir = [](const char* messageId) {
        return envelope(messageId,
                        "<roReadyToAir><roID>R</roID><roAir>READY</roAir></roReadyToAir>");
    };

    // One document dribbled across two TCP writes.
    const std::vector<std::uint8_t> split = encodeWire(readyToAir("301"));
    const std::size_t half = (split.size() / 2) & ~std::size_t{1};  // keep UCS-2 alignment honest
    ASSERT_TRUE(
        rig.ncsEndpoint->send(Channel::Ro, std::span<const std::uint8_t>{split.data(), half}).ok());
    rig.pumpOnce();
    ASSERT_TRUE(rig.ncsEndpoint
                    ->send(Channel::Ro,
                           std::span<const std::uint8_t>{split.data() + half, split.size() - half})
                    .ok());

    // Two complete documents coalesced into a single write.
    std::vector<std::uint8_t> coalesced = encodeWire(readyToAir("302"));
    const std::vector<std::uint8_t> second = encodeWire(readyToAir("303"));
    coalesced.insert(coalesced.end(), second.begin(), second.end());
    ASSERT_TRUE(rig.ncsEndpoint->send(Channel::Ro, coalesced).ok());

    ASSERT_TRUE(rig.waitFor(
        [&] { return countReceived(rig.deviceEvents, MessageType::RoReadyToAir) == 3; }));

    rig.deviceEndpoint->stop();
    rig.ncsEndpoint->stop();
}

// A dialed channel redials after the peer's listener restarts (the 2.8
// analogue of the ws40 passive-reconnect gate; ReconnectPolicy backoff).
TEST(Tcp28Transport, DialedChannelRedialsAfterListenerRestart) {
    Rig rig;
    rig.start();
    ASSERT_TRUE(rig.waitForLink());

    rig.ncsEndpoint->stop();  // takes the mom listener down
    ASSERT_TRUE(rig.waitFor([&] { return !rig.deviceEndpoint->isConnected(Channel::Mom); }));

    ASSERT_TRUE(rig.ncsEndpoint->start().ok());  // rebinds the same port
    ASSERT_TRUE(rig.waitFor([&] { return rig.deviceEndpoint->isConnected(Channel::Mom); }));

    rig.deviceEvents.clear();
    ASSERT_TRUE(
        rig.deviceSession->enqueue(Channel::Mom, makeMessage(MessageType::ReqMachInfo)).ok());
    ASSERT_TRUE(rig.waitFor(
        [&] { return findAck(rig.deviceEvents, MessageType::ListMachInfo) != nullptr; }));

    rig.deviceEndpoint->stop();
    rig.ncsEndpoint->stop();
}

// ---- Profile 1 and 2 conformance workflows re-run over 2.8 TCP ------------
//
// The same facade-level walks the ws40 suite runs on WebSocket, on the
// legacy transport (plan §10 "re-executed parameterized over the real
// transports"): P1 push/fetch/batch on mom, P2 authoring convergence and a
// device status report on ro.

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

/// Facade pair (imos::Device / incs::Ncs) over the TCP endpoints.
struct FacadeRig {
    SystemClock clock;
    imos::InMemoryMessageStore deviceStore;
    imos::InMemoryMessageStore ncsStore;
    std::unique_ptr<Tcp28Endpoint> deviceEndpoint;
    std::unique_ptr<Tcp28Endpoint> ncsEndpoint;
    std::unique_ptr<imos::Device> device;
    std::unique_ptr<incs::Ncs> ncs;

    void start(imos::encoding::WireEncoding encoding = imos::encoding::WireEncoding::Ucs2Be,
               bool withAux = false) {
        ChannelPorts ports;
        ports.mom = pickFreePort(clock);
        ports.ro = pickFreePort(clock);
        ASSERT_NE(ports.mom, 0);
        ASSERT_NE(ports.ro, 0);
        if (withAux) {
            ports.aux = pickFreePort(clock);
            ASSERT_NE(ports.aux, 0);
        }

        EndpointConfig ncsConfig;
        ncsConfig.role = Role::Ncs;
        ncsConfig.peerHost = "127.0.0.1";
        ncsConfig.bindAddress = "127.0.0.1";
        const PortPlan plan = planPorts(ports, clock);
        ncsConfig.ports = plan.ncsListen;
        ncsConfig.peerPorts = plan.deviceListen;
        ncsConfig.enableAux = withAux;
        ncsConfig.wireEncoding = encoding;
        ncsConfig.reconnect.initialDelayMillis = 50;
        ncsEndpoint = std::make_unique<Tcp28Endpoint>(ncsConfig, &clock);
        ncs = std::make_unique<incs::Ncs>(
            incs::NcsConfig{sessionConfig(encoding), machineInfo(DeviceType::Ncs)},
            PeerSession::Dependencies{&clock, ncsEndpoint.get(), &ncsStore});

        EndpointConfig deviceConfig;
        deviceConfig.role = Role::Device;
        deviceConfig.peerHost = "127.0.0.1";
        deviceConfig.bindAddress = "127.0.0.1";
        deviceConfig.ports = plan.deviceListen;
        deviceConfig.peerPorts = plan.ncsListen;
        deviceConfig.enableAux = withAux;
        deviceConfig.wireEncoding = encoding;
        deviceConfig.reconnect.initialDelayMillis = 50;
        deviceEndpoint = std::make_unique<Tcp28Endpoint>(deviceConfig, &clock);
        device = std::make_unique<imos::Device>(
            imos::DeviceConfig{sessionConfig(encoding), machineInfo(DeviceType::Mos)},
            PeerSession::Dependencies{&clock, deviceEndpoint.get(), &deviceStore});

        ASSERT_TRUE(ncsEndpoint->start().ok());
        ASSERT_TRUE(deviceEndpoint->start().ok());
    }

    void pumpOnce() {
        deviceEndpoint->pump(device->session());
        device->poll();
        ncsEndpoint->pump(ncs->session());
        ncs->poll();
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

    void stop() {
        deviceEndpoint->stop();
        ncsEndpoint->stop();
    }
};

}  // namespace

TEST(Tcp28Transport, Profile1ObjectWorkflowOverTcp) {
    FacadeRig rig;
    rig.start();
    VectorObjectSource source;
    source.put("W2", makeObject("W2", "UPDATED"));
    source.put("W3", makeObject("W3", "UPDATED"));
    rig.device->attach(source);

    std::vector<imos::profiles::DirectoryEvent> dirEvents;
    ASSERT_TRUE(rig.waitFor([&] { return rig.deviceEndpoint->isConnected(Channel::Mom); }));

    // Push: an unsolicited object announcement is mirrored and acked.
    ASSERT_TRUE(rig.device->announceObject(makeObject("P1", "NEW")).ok());
    ASSERT_TRUE(rig.waitFor([&] { return rig.ncs->object("P1") != nullptr; }));

    // Single fetch (hit), then a NACK miss surfacing as a directory event.
    ASSERT_TRUE(rig.ncs->requestObject("W2").ok());
    ASSERT_TRUE(rig.waitFor([&] { return rig.ncs->object("W2") != nullptr; }));
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

    // Full sync (batch): the whole catalog arrives in one mosListAll.
    ASSERT_TRUE(rig.ncs->requestAll(0).ok());
    ASSERT_TRUE(rig.waitFor([&] { return rig.ncs->object("W3") != nullptr; }));

    rig.stop();
}

// The aux/query port (10542, the mosReqObjList family's home in 2.8.5):
// NCS dials, MOS listens, and traffic flows — proven with Profile 0
// messages, which are valid on any channel.
TEST(Tcp28Transport, AuxQueryPortCarriesTraffic) {
    Rig rig;
    rig.start(/*withResponders=*/true, /*withAux=*/true);
    ASSERT_TRUE(rig.waitFor([&] {
        return rig.ncsEndpoint->isConnected(Channel::Aux) &&
               rig.deviceEndpoint->isConnected(Channel::Aux);
    }));

    ASSERT_TRUE(rig.ncsSession->enqueue(Channel::Aux, makeMessage(MessageType::ReqMachInfo)).ok());
    ASSERT_TRUE(
        rig.waitFor([&] { return findAck(rig.ncsEvents, MessageType::ListMachInfo) != nullptr; }));

    rig.deviceEndpoint->stop();
    rig.ncsEndpoint->stop();
}

// ---- Profile 4 byte-preservation gate over 2.8 TCP -------------------------
//
// The agency-NewsML verbatim rule on the legacy stream in both UCS-2 byte
// orders (BE is the spec wire; LE is the documented field deviation the
// encoding engine supports as a dialect knob). The multi-kilobyte payload
// also gives the deframer a realistic large document to tokenize.

namespace {

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

TEST(Tcp28Transport, Profile4AgencyPayloadPreservedOverTcpBothByteOrders) {
    for (const imos::encoding::WireEncoding encoding :
         {imos::encoding::WireEncoding::Ucs2Be, imos::encoding::WireEncoding::Ucs2Le}) {
        SCOPED_TRACE(imos::encoding::wireEncodingName(encoding));
        FacadeRig rig;
        rig.start(encoding);
        CapturingStorySink sink;
        rig.device->attach(sink);
        ASSERT_TRUE(rig.waitFor([&] { return rig.ncsEndpoint->isConnected(Channel::Ro); }));

        ASSERT_TRUE(rig.ncs->author().create("TCP-RO4", "TCP Rundown").ok());
        ASSERT_TRUE(rig.ncs->author()
                        .insertStory("TCP-RO4", incs::RunningOrderAuthor::makeStory("s1"))
                        .ok());
        ASSERT_TRUE(rig.ncs->author().commit("TCP-RO4").ok());
        ASSERT_TRUE(rig.waitFor([&] { return rig.device->runningOrderModel().has("TCP-RO4"); }));

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

        ASSERT_TRUE(rig.ncs->author().setAgencyStoryBody("TCP-RO4", "s1", metadata).ok());
        ASSERT_TRUE(rig.ncs->sendStory("TCP-RO4", "s1").ok());
        ASSERT_TRUE(rig.waitFor([&] { return !sink.agencyBytes.empty(); }));
        EXPECT_EQ(sink.agencyBytes.front(), authoredBytes);

        rig.stop();
    }
}

// ---- Profile 5 item control over 2.8 TCP -----------------------------------

namespace {

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

}  // namespace

TEST(Tcp28Transport, Profile5ItemControlOverTcp) {
    FacadeRig rig;
    rig.start();
    CapturingControlSink sink;
    rig.device->attach(sink);
    ASSERT_TRUE(rig.waitFor([&] { return rig.ncsEndpoint->isConnected(Channel::Ro); }));

    // Converge one RO with an item so every scope level has a target.
    ASSERT_TRUE(rig.ncs->author().create("TCP-RO5", "TCP Rundown").ok());
    imos::codec::XmlNode story = incs::RunningOrderAuthor::makeStory("s1", "Lead");
    story.children.push_back(incs::RunningOrderAuthor::makeItem("i1", "OBJ1", kMosID));
    ASSERT_TRUE(rig.ncs->author().insertStory("TCP-RO5", story).ok());
    ASSERT_TRUE(rig.ncs->author().commit("TCP-RO5").ok());
    ASSERT_TRUE(rig.waitFor([&] { return rig.device->runningOrderModel().has("TCP-RO5"); }));

    // roCtrl at item scope: the sink sees the resolved request, the roAck
    // resolves on the NCS side.
    ASSERT_TRUE(rig.ncs
                    ->sendCtrl(imos::profiles::CtrlScope::Item, "TCP-RO5", "s1", "i1",
                               imos::profiles::CtrlCommand::Execute)
                    .ok());
    ASSERT_TRUE(rig.waitFor([&] { return !sink.ctrls.empty(); }));
    EXPECT_EQ(sink.ctrls.front().scope, imos::profiles::CtrlScope::Item);
    EXPECT_EQ(sink.ctrls.front().itemID, "i1");
    std::optional<imos::profiles::CtrlResult> result;
    ASSERT_TRUE(rig.waitFor([&] { return (result = rig.ncs->nextCtrlResult()).has_value(); }));
    EXPECT_TRUE(result->ok) << result->roStatus;

    // Device-originated cue reaches the NCS.
    imos::profiles::ItemCue cue;
    cue.mosID = kMosID;
    cue.roID = "TCP-RO5";
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

// ---- Profile 7 story action over 2.8 TCP -----------------------------------

TEST(Tcp28Transport, Profile7StoryActionOverTcp) {
    FacadeRig rig;
    rig.start();
    ASSERT_TRUE(rig.waitFor([&] { return rig.ncsEndpoint->isConnected(Channel::Ro); }));

    ASSERT_TRUE(rig.ncs->author().create("TCP-RO7", "TCP Rundown").ok());
    ASSERT_TRUE(rig.ncs->author().commit("TCP-RO7").ok());
    ASSERT_TRUE(rig.waitFor([&] { return rig.device->runningOrderModel().has("TCP-RO7"); }));

    imos::profiles::StoryActionRequest request{};
    request.operation = imos::profiles::StoryAction::New;
    request.roID = "TCP-RO7";
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
        return rig.device->runningOrderModel().storyIDs("TCP-RO7") ==
               std::vector<std::string>{"s7"};
    }));

    rig.stop();
}

// ---- Profile 3 three-channel workflow over 2.8 TCP -------------------------
//
// The aux/query port carrying its real Profile 3 traffic alongside mom and
// ro: schema + search on 10542, an object fetch on 10540, a construction
// commit on 10541, and the placeholder-create flow end-to-end.

namespace {

class AdvancedVectorSource : public VectorObjectSource {
public:
    [[nodiscard]] bool supportsProfile3() const noexcept override { return true; }
    imos::Result<std::string> createObject(
        const imos::profiles::ObjectCreateRequest& request) override {
        const std::string objID = "TCPOBJ-" + std::to_string(nextID_++);
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
        return imos::profiles::SchemaList{"http://tcp.example/schemas/meta1"};
    }

private:
    int nextID_ = 1;
};

}  // namespace

TEST(Tcp28Transport, Profile3ThreeChannelWorkflowOverTcp) {
    FacadeRig rig;
    rig.start(imos::encoding::WireEncoding::Ucs2Be, /*withAux=*/true);
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

    ASSERT_TRUE(rig.waitFor([&] {
        return rig.deviceEndpoint->isConnected(Channel::Mom) &&
               rig.ncsEndpoint->isConnected(Channel::Ro) &&
               rig.ncsEndpoint->isConnected(Channel::Aux);
    }));

    // Fire one exchange per channel before any completes: mom fetch, ro
    // construction, aux schema request — then let all three land.
    ASSERT_TRUE(rig.ncs->requestObject("W1").ok());
    ASSERT_TRUE(rig.ncs->author().create("TCP-RO3", "TCP Rundown").ok());
    ASSERT_TRUE(rig.ncs->author().commit("TCP-RO3").ok());
    ASSERT_TRUE(rig.ncs->requestSearchableSchemas().ok());

    ASSERT_TRUE(rig.waitFor([&] { return drain() || rig.ncs->object("W1") != nullptr; }));
    ASSERT_TRUE(rig.waitFor([&] { return rig.device->runningOrderModel().has("TCP-RO3"); }));
    ASSERT_TRUE(rig.waitFor([&] {
        return rig.ncs->searchableSchemas() ==
               std::vector<std::string>{"http://tcp.example/schemas/meta1"};
    }));

    // The search family on aux: a match-all query pages the catalog back.
    imos::profiles::ObjectQuery query;
    query.queryID = "tcp-q1";
    ASSERT_TRUE(rig.ncs->queryObjects(query).ok());
    ASSERT_TRUE(rig.waitFor([&] { return drain() || !pages.empty(); }));
    EXPECT_EQ(pages.front().listReturnTotal, 1U);

    // Placeholder create end-to-end: ACK carries the objID, the push follows.
    ASSERT_TRUE(
        rig.ncs->createObject(imos::profiles::makeMosObjCreate("TCP Fire", "VIDEO", "59.94")).ok());
    ASSERT_TRUE(rig.waitFor([&] { return drain() || rig.ncs->object("TCPOBJ-1") != nullptr; }));
    bool sawCreated = false;
    for (const auto& event : dirEvents) {
        sawCreated =
            sawCreated || (event.type == imos::profiles::DirectoryEventType::ObjectCreated &&
                           event.objID == "TCPOBJ-1");
    }
    EXPECT_TRUE(sawCreated);

    rig.stop();
}

TEST(Tcp28Transport, Profile2RunningOrderWorkflowOverTcp) {
    FacadeRig rig;
    rig.start();
    ASSERT_TRUE(rig.waitFor([&] {
        return rig.ncsEndpoint->isConnected(Channel::Ro) &&
               rig.deviceEndpoint->isConnected(Channel::Ro);
    }));

    // Author and commit; the device's shadow model converges over the wire.
    ASSERT_TRUE(rig.ncs->author().create("TCP-RO", "TCP Rundown").ok());
    imos::codec::XmlNode story = incs::RunningOrderAuthor::makeStory("s1", "Lead");
    story.children.push_back(incs::RunningOrderAuthor::makeItem("i1", "OBJ1", kMosID));
    ASSERT_TRUE(rig.ncs->author().insertStory("TCP-RO", story).ok());
    ASSERT_TRUE(
        rig.ncs->author().insertStory("TCP-RO", incs::RunningOrderAuthor::makeStory("s2")).ok());
    ASSERT_TRUE(rig.ncs->author().commit("TCP-RO").ok());
    ASSERT_TRUE(rig.waitFor([&] {
        return rig.device->runningOrderModel().storyIDs("TCP-RO") ==
               std::vector<std::string>{"s1", "s2"};
    }));

    // A mutation converges after its ack round.
    ASSERT_TRUE(rig.ncs->author().moveStory("TCP-RO", "s2", "s1").ok());
    ASSERT_TRUE(rig.ncs->author().commit("TCP-RO").ok());
    ASSERT_TRUE(rig.waitFor([&] {
        return rig.device->runningOrderModel().storyIDs("TCP-RO") ==
               std::vector<std::string>{"s2", "s1"};
    }));

    // Device-originated roElementStat reaches the NCS.
    imos::profiles::ElementStatusReport play;
    play.element = imos::profiles::RoElement::Item;
    play.roID = "TCP-RO";
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

// ---- 2.8.5 "Two Ports - Four Sockets" ---------------------------------------
//
// "MOS uses two ports bi-directionally. Applications will simultaneously
// listen for messages on both ports": every role accepts connections on the
// lower and the upper port, and dials the peer's for the messages it
// originates. Only the query port is one-way (the MOS listens, the NCS
// dials).
TEST(Tcp28Transport, EachRoleListensOnBothPorts) {
    SystemClock clock;

    EndpointConfig deviceConfig;
    deviceConfig.role = Role::Device;
    deviceConfig.peerHost = "127.0.0.1";
    deviceConfig.bindAddress = "127.0.0.1";
    deviceConfig.enableAux = true;
    deviceConfig.ports = ChannelPorts{0, 0, 0};
    Tcp28Endpoint device{deviceConfig, &clock};
    ASSERT_TRUE(device.start().ok());
    EXPECT_TRUE(device.listenPort(Channel::Ro).has_value());
    EXPECT_TRUE(device.listenPort(Channel::Mom).has_value())
        << "the device accepts nothing on the lower port (NCS-originated mosReqObj/mosReqAll)";
    EXPECT_TRUE(device.listenPort(Channel::Aux).has_value());
    device.stop();

    EndpointConfig ncsConfig;
    ncsConfig.role = Role::Ncs;
    ncsConfig.peerHost = "127.0.0.1";
    ncsConfig.bindAddress = "127.0.0.1";
    ncsConfig.enableAux = true;
    ncsConfig.ports = ChannelPorts{0, 0, 0};
    Tcp28Endpoint ncs{ncsConfig, &clock};
    ASSERT_TRUE(ncs.start().ok());
    EXPECT_TRUE(ncs.listenPort(Channel::Mom).has_value());
    EXPECT_TRUE(ncs.listenPort(Channel::Ro).has_value())
        << "the NCS accepts nothing on the upper port (MOS-originated roReq/roElementStat)";
    EXPECT_FALSE(ncs.listenPort(Channel::Aux).has_value());  // queries are NCS → MOS only
    ncs.stop();

    // The opt-out keeps one socket per port, opened by its traditional
    // originator.
    EXPECT_TRUE(Tcp28Endpoint::dialsChannel(Role::Device, Channel::Mom, Topology::SingleDirection));
    EXPECT_FALSE(Tcp28Endpoint::dialsChannel(Role::Device, Channel::Ro, Topology::SingleDirection));
    EXPECT_FALSE(
        Tcp28Endpoint::listensOnChannel(Role::Device, Channel::Mom, Topology::SingleDirection));
    EXPECT_TRUE(
        Tcp28Endpoint::listensOnChannel(Role::Ncs, Channel::Mom, Topology::SingleDirection));
    EXPECT_FALSE(
        Tcp28Endpoint::listensOnChannel(Role::Ncs, Channel::Ro, Topology::SingleDirection));
}

// A peer that runs one socket per port (it listens on mom only and dials ro
// only) is still served in both directions on both ports: its own
// connection carries our requests where it does not accept ours, and its
// requests arriving down our dialed socket are answered there.
TEST(Tcp28Transport, SingleDirectionPeerIsServedBothWaysOnBothPorts) {
    Rig rig;
    rig.ncsTopology = Topology::SingleDirection;
    rig.start();
    ASSERT_TRUE(rig.waitForLink());

    for (const Channel channel : {Channel::Mom, Channel::Ro}) {
        rig.deviceEvents.clear();
        rig.ncsEvents.clear();
        ASSERT_TRUE(
            rig.deviceSession->enqueue(channel, makeMessage(MessageType::ReqMachInfo)).ok());
        ASSERT_TRUE(rig.waitFor([&] {
            return findAck(rig.deviceEvents, MessageType::ListMachInfo) != nullptr;
        })) << "device → NCS on "
            << imos::channelName(channel);
        ASSERT_TRUE(rig.ncsSession->enqueue(channel, makeMessage(MessageType::ReqMachInfo)).ok());
        ASSERT_TRUE(rig.waitFor([&] {
            return findAck(rig.ncsEvents, MessageType::ListMachInfo) != nullptr;
        })) << "NCS → device on "
            << imos::channelName(channel);
    }
    rig.deviceEndpoint->stop();
    rig.ncsEndpoint->stop();
}

#if !defined(_WIN32)

/// A bare TCP client standing in for a foreign implementation: connects to
/// a loopback port (optionally from a chosen source address), writes, and
/// reads whatever comes back.
class RawClient {
public:
    RawClient() = default;
    RawClient(const RawClient&) = delete;
    RawClient& operator=(const RawClient&) = delete;
    ~RawClient() { close(); }

    bool connectTo(int port, const char* sourceAddress = nullptr) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return false;
        }
        if (sourceAddress != nullptr) {
            sockaddr_in local = {};
            local.sin_family = AF_INET;
            ::inet_pton(AF_INET, sourceAddress, &local.sin_addr);
            if (::bind(fd_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
                return false;
            }
        }
        sockaddr_in remote = {};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(static_cast<std::uint16_t>(port));
        ::inet_pton(AF_INET, "127.0.0.1", &remote.sin_addr);
        return ::connect(fd_, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) == 0;
    }

    bool write(const std::vector<std::uint8_t>& bytes) const {
        return ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(bytes.size());
    }

    /// Non-blocking drain into `received`; false once the peer has closed.
    bool poll() {
        pollfd entry{fd_, POLLIN, 0};
        while (!closed_ && ::poll(&entry, 1, 0) > 0) {
            std::uint8_t buffer[4096];
            const ssize_t got = ::recv(fd_, buffer, sizeof(buffer), 0);
            if (got <= 0) {
                closed_ = true;
                break;
            }
            received.insert(received.end(), buffer, buffer + got);
        }
        return !closed_;
    }

    [[nodiscard]] bool closedByPeer() const { return closed_; }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    std::vector<std::uint8_t> received;

private:
    int fd_ = -1;
    bool closed_ = false;
};

// An NCS that dials the device's lower port for the messages it originates
// there (mosReqObj, mosReqAll, reqMachInfo) is accepted, and the answer
// returns "down the same socket on which the original message was received".
TEST(Tcp28Transport, PeerDialingTheLowerPortIsAnsweredOnItsOwnSocket) {
    Rig rig;
    rig.startNcs = false;
    rig.start();
    const int lowerPort = rig.deviceEndpoint->listenPort(Channel::Mom).value_or(0);
    ASSERT_NE(lowerPort, 0);

    RawClient ncs;
    ASSERT_TRUE(ncs.connectTo(lowerPort));
    ASSERT_TRUE(ncs.write(encodeWire(envelope("77", "<reqMachInfo/>"))));
    ASSERT_TRUE(rig.waitFor([&] {
        ncs.poll();
        const auto decoded = imos::encoding::decodeToUtf8(ncs.received, {});
        return decoded.ok() && decoded.value().find("</mos>") != std::string::npos;
    }));
    const auto reply =
        imos::codec::parseMessage(imos::encoding::decodeToUtf8(ncs.received, {}).value());
    ASSERT_TRUE(reply.ok());
    EXPECT_EQ(reply.value().type(), MessageType::ListMachInfo);
    EXPECT_EQ(reply.value().messageID().value_or(""), "77");
    ncs.close();
    rig.deviceEndpoint->stop();
}

#if defined(__linux__)
// MOS 2.8 carries no credentials, so the peer's address is all that tells
// it from anyone else who can reach the port. A connection from a host
// other than the configured peer is refused before a byte is read. (Linux
// only: it routes all of 127.0.0.0/8 to loopback, which gives the test a
// second source address.)
TEST(Tcp28Transport, ConnectionsFromOtherHostsAreRefused) {
    const std::string readyToAir =
        envelope("88", "<roReadyToAir><roID>R</roID><roAir>READY</roAir></roReadyToAir>");
    {
        Rig rig;
        rig.startNcs = false;
        rig.start(/*withResponders=*/false);
        const int upperPort = rig.deviceEndpoint->listenPort(Channel::Ro).value_or(0);
        ASSERT_NE(upperPort, 0);

        RawClient stranger;
        ASSERT_TRUE(stranger.connectTo(upperPort, "127.0.0.2"));
        (void)stranger.write(encodeWire(readyToAir));
        ASSERT_TRUE(rig.waitFor([&] {
            stranger.poll();
            return stranger.closedByPeer();
        })) << "a connection from 127.0.0.2 was kept although the peer is 127.0.0.1";
        EXPECT_EQ(countReceived(rig.deviceEvents, MessageType::RoReadyToAir), 0);
        EXPECT_FALSE(rig.deviceEndpoint->isConnected(Channel::Ro));
        ASSERT_TRUE(rig.waitFor([&] { return !rig.deviceDiagnostics.empty(); }));
        EXPECT_NE(rig.deviceDiagnostics.front().second.find("127.0.0.2"), std::string::npos);

        // The configured peer itself is of course let in.
        RawClient peer;
        ASSERT_TRUE(peer.connectTo(upperPort));
        ASSERT_TRUE(peer.write(encodeWire(readyToAir)));
        ASSERT_TRUE(rig.waitFor(
            [&] { return countReceived(rig.deviceEvents, MessageType::RoReadyToAir) == 1; }));
        rig.deviceEndpoint->stop();
    }
    {
        // The opt-out (NAT, a multi-homed NCS): any source is accepted.
        Rig rig;
        rig.startNcs = false;
        rig.deviceAcceptOnlyPeerHost = false;
        rig.start(/*withResponders=*/false);
        const int upperPort = rig.deviceEndpoint->listenPort(Channel::Ro).value_or(0);
        RawClient other;
        ASSERT_TRUE(other.connectTo(upperPort, "127.0.0.2"));
        ASSERT_TRUE(other.write(encodeWire(readyToAir)));
        ASSERT_TRUE(rig.waitFor(
            [&] { return countReceived(rig.deviceEvents, MessageType::RoReadyToAir) == 1; }));
        rig.deviceEndpoint->stop();
    }
}
#endif  // defined(__linux__)

#endif  // !defined(_WIN32)
