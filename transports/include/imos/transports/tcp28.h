// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// MOS 2.8.x TCP transport (plan §5.2, milestone M6; design of record
// reference/m6-plan.md §1). A dedicated cross-platform socket layer — no
// IXWebSocket involvement — carrying the classic port scheme: the NCS
// listens on the lower port 10540 (mom), the MOS listens on the upper port
// 10541 (ro) and the query port 10542 (aux), and each side dials the
// channels it originates on. Sockets are bidirectional: acknowledgments
// return on the socket the message arrived on, listeners accept multiple
// simultaneous connections (the spec's discrete-socket-per-message
// tolerance), and the byte stream has no length framing — a per-connection
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
#include <imos/transports_export.h>

namespace imos::transports::tcp28 {

/// Which side of the peer relationship this endpoint plays. The role fixes
/// the dial/listen direction per channel (2.8.5 "Message Transport"): the
/// device dials mom and listens on ro/aux; the NCS listens on mom and dials
/// ro/aux.
enum class Role : std::uint8_t { Device, Ncs };

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
    /// Host the dialed channels connect to.
    std::string peerHost = "127.0.0.1";
    /// Address the listening channels bind on.
    std::string bindAddress = "0.0.0.0";
    ChannelPorts ports;
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
    // encoding; on a listening channel the reply rides the connection the
    // last inbound message arrived on.
    Result<void> send(Channel channel, std::span<const std::uint8_t> frame) override;
    [[nodiscard]] bool isConnected(Channel channel) const override;

    /// Drain queued connectivity changes and deframed documents into the
    /// session and drive redials. Call from the host's poll loop, before
    /// session.poll().
    void pump(PeerSession& session);

    void setErrorHandler(Tcp28ErrorHandler handler);

    /// The bound port of a listening channel (resolves ephemeral requests
    /// after start()); nullopt for dialed or disabled channels.
    [[nodiscard]] std::optional<int> listenPort(Channel channel) const;

    /// Earliest instant pump() has scheduled work (redial attempts), for
    /// host scheduling alongside PeerSession::nextDeadline().
    [[nodiscard]] std::optional<MonotonicTime> nextDeadline() const;

    /// True when the role dials this channel (else it listens).
    [[nodiscard]] static bool dialsChannel(Role role, Channel channel) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace imos::transports::tcp28
