// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// MOS 2.8.x TCP transport (plan §5.2, milestone M6; design of record
// reference/m6-plan.md §1). A dedicated cross-platform socket layer — no
// IXWebSocket involvement — carrying the classic port scheme the way the
// 2.8.5 spec describes it: "MOS uses two ports bi-directionally.
// Applications will simultaneously listen for messages on both ports"
// ("Two Ports - Four Sockets"). Both roles listen on the lower port 10540
// (mom) and the upper port 10541 (ro), and both dial the peer's; the MOS
// additionally listens on the query port 10542 (aux), which only the NCS
// dials. A message a side originates goes out on the socket it dialed; the
// response returns "down the same socket on which the original message was
// received". Listeners accept multiple simultaneous connections (the
// spec's discrete-socket-per-message tolerance) — by default only from the
// configured peer host — and the byte stream has no length framing: a per-connection
// StreamDeframer tokenizes complete <mos> documents in the configured wire
// encoding (UCS-2 BE by default, per §3.4).
//
// Threading contract mirrors ws40: one internal I/O thread owns every
// socket and only touches sockets and queues; the host drains events into
// the sans-I/O PeerSession by calling pump() from its own poll loop, and
// send() hands frames to the I/O thread. The session is never touched from
// the I/O thread.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <imos/encoding/encoding.h>
#include <imos/error.h>
#include <imos/protocol.h>
#include <imos/session/peer_session.h>
#include <imos/session/reconnect.h>
#include <imos/session/runtime.h>
#include <imos/session/transport.h>
#include <imos/transports/activity.h>
#include <imos/transports/channel_events.h>
#include <imos/transports_export.h>

namespace imos::transports::tcp28 {

/// Which side of the peer relationship this endpoint plays. The role only
/// decides the query port (aux): the MOS listens there and the NCS dials.
enum class Role : std::uint8_t { Device, Ncs };

/// How many of the spec's sockets this endpoint runs.
enum class Topology : std::uint8_t {
    /// The spec's scheme and the default: listen on both mom and ro, and
    /// dial the peer's mom and ro for the messages this side originates.
    /// A peer that does not listen on one of them is still served — its
    /// dial to us is used in both directions, and ours just keeps retrying
    /// quietly.
    FourSockets,
    /// One socket per port, opened by its traditional originator: the
    /// device dials mom and listens on ro/aux; the NCS listens on mom and
    /// dials ro/aux. For sites that must not open the reverse listeners.
    SingleDirection,
};

/// Per-channel TCP ports. 10540/10541/10542 are the spec's examples and the
/// de-facto convention, but they have been "vendor selectable but site
/// specific" since 2.5 — hence knobs. A listening channel may use 0 to bind
/// an ephemeral port (tests); listenPort() reports the choice.
struct ChannelPorts {
    int mom = 10540;
    int ro = 10541;
    int aux = 10542;
};

struct EndpointConfig {
    Role role = Role::Device;
    Topology topology = Topology::FourSockets;
    /// Host the dialed channels connect to — and, with acceptOnlyPeerHost,
    /// the only host the listening channels accept connections from.
    std::string peerHost = "127.0.0.1";
    /// Address the listening channels bind on.
    std::string bindAddress = "0.0.0.0";
    /// Ports this endpoint listens on.
    ChannelPorts ports;
    /// Ports dialed on the peer. The spec has every machine at a site use
    /// the same numbers, so these default to the same scheme; they are
    /// separate so two endpoints can share one host.
    ChannelPorts peerPorts;
    /// MOS 2.8 has no authentication: whoever can reach a listening port
    /// can drive the running order. Accept connections only from the
    /// address(es) peerHost resolves to, and refuse (with a diagnostic)
    /// everyone else. Turn off only when the peer legitimately connects
    /// from an address peerHost does not name (NAT, a multi-homed NCS).
    bool acceptOnlyPeerHost = true;
    /// The aux channel (query port 10542, mosReqObjList family) is opened
    /// only when the deployment uses it — Profile 3 peers.
    bool enableAux = false;
    /// §3.4 wire encoding: UCS-2 BE per the 2.8.5 spec; Ucs2Le exists in
    /// the field as a dialect deviation, Utf8 for lab use.
    encoding::WireEncoding wireEncoding = encoding::WireEncoding::Ucs2Be;
    /// Hard per-message size cap fed to the deframer (plan §12).
    std::size_t maxMessageBytes = 4U * 1024U * 1024U * 2U;
    /// Simultaneous accepted connections per listening channel. The spec
    /// requires tolerating at least two; the cap bounds resource use.
    std::size_t maxConnectionsPerListener = 8;
    ReconnectConfig reconnect;
    int connectTimeoutSecs = 10;
};

/// Diagnostic sink for transport-level trouble that is not a connectivity
/// change: deframer resyncs/overflows, oversized messages, listener
/// saturation. (channel, diagnostic).
using Tcp28ErrorHandler = std::function<void(Channel, const std::string&)>;

class IMOS_TRANSPORTS_API Tcp28Endpoint : public ITransport {
public:
    Tcp28Endpoint(EndpointConfig config, IClock* clock, IRandom* random = nullptr,
                  ILogger* logger = nullptr);
    ~Tcp28Endpoint() override;

    Tcp28Endpoint(const Tcp28Endpoint&) = delete;
    Tcp28Endpoint& operator=(const Tcp28Endpoint&) = delete;

    /// Bind and listen on this role's listening channels (errors surface
    /// here), then launch the I/O thread and begin dialing the rest
    /// (asynchronous, driven by the reconnect policy).
    Result<void> start();

    /// Orderly shutdown: every socket gets the four-way close the 2.8.5
    /// spec insists on (shutdown-write, bounded drain to EOF, close) so no
    /// FIN_WAIT_2/CLOSE_WAIT remnants pin the ports; then the I/O thread
    /// is joined.
    void stop();

    // ITransport. Frames are complete <mos> documents already in the wire
    // encoding. send() carries a message this side originates: it goes out
    // on the socket this endpoint dialed (or, when the peer does not accept
    // on that port, on the peer's own connection). sendReply() carries a
    // response: it returns on the accepted connection the peer's last
    // message arrived on (or, when the peer sent it down our dialed socket,
    // on that).
    Result<void> send(Channel channel, std::span<const std::uint8_t> frame) override;
    Result<void> sendReply(Channel channel, std::span<const std::uint8_t> frame) override;
    [[nodiscard]] bool isConnected(Channel channel) const override;

    /// Drain queued connectivity changes and deframed documents into the
    /// session and drive redials. Call from the host's poll loop, before
    /// session.poll().
    void pump(PeerSession& session);
    /// The same, into a sink of the host's: connectivity changes and
    /// inbound frames, for a host that sees the traffic on its way to its
    /// session (a facade such as incs::Ncs, or a traffic log).
    void pump(const ChannelEventSink& sink);

    void setErrorHandler(Tcp28ErrorHandler handler);
    /// Wake-up hook: called on a transport thread whenever pump() has
    /// something new to drain. Install before start(); see
    /// <imos/transports/activity.h> for the contract.
    void setActivityHandler(ActivityHandler handler);

    /// The bound port of a listening channel (resolves ephemeral requests
    /// after start()); nullopt for channels this endpoint does not listen
    /// on, or disabled ones.
    [[nodiscard]] std::optional<int> listenPort(Channel channel) const;

    /// Earliest instant pump() has scheduled work (redial attempts), for
    /// host scheduling alongside PeerSession::nextDeadline().
    [[nodiscard]] std::optional<MonotonicTime> nextDeadline() const;

    /// Whether a role dials / listens on a channel under a topology.
    [[nodiscard]] static bool dialsChannel(Role role, Channel channel,
                                           Topology topology = Topology::FourSockets) noexcept;
    [[nodiscard]] static bool listensOnChannel(Role role, Channel channel,
                                               Topology topology = Topology::FourSockets) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace imos::transports::tcp28
