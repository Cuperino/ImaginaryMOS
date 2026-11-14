// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// MOS 4.0 WebSocket transport over IXWebSocket (plan §5.1, milestone M2).
// Client and server roles, and the two topologies the spec defines:
//
//  - Standard: "both the MOS and the NCS will open up one or two web socket
//    connections ... to the other side's web socket URL". Each side dials
//    the other and each side listens. A connection carries the requests of
//    the side that opened it and the other side's responses to them —
//    nothing else. Compose a Ws40Client (this side's dialed connections)
//    with this peer's Ws40Server transport through Ws40Endpoint.
//  - Passive: only the party inside the firewall dials, adding passive=true
//    to the URL, which "instructs the receiving device to respond to the
//    connection through the temporary opening": the listening party then
//    originates its own messages down those same connections. Ws40Client
//    with passive=true on one side, Ws40Server alone on the other.
//
// The server therefore originates on an accepted connection only when that
// connection's URL carried passive=true; on any other it may only answer.
// Threading
// contract: IXWebSocket delivers callbacks on its own background threads;
// this layer queues them, and the host drains the queue into the sans-I/O
// PeerSession by calling pump() from its own poll loop — the session itself
// is never touched from a transport thread. send() is safe from the pump
// thread (IXWebSocket serializes writes internally).
//
// Frame-type policy (plan §3.4): UCS-2 payloads travel as *binary* WebSocket
// frames (the default); UTF-8 is a per-peer option carried as *text* frames.
// The receive side expects the configured type — a mismatch is surfaced
// through the error handler with a diagnostic naming the likely
// misconfiguration, and the frame is not delivered.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imos/encoding/encoding.h>
#include <imos/error.h>
#include <imos/protocol.h>
#include <imos/session/peer_session.h>
#include <imos/session/reconnect.h>
#include <imos/session/runtime.h>
#include <imos/session/transport.h>
#include <imos/transports/activity.h>
#include <imos/transports_export.h>
#include <imos/ws40/basic_auth.h>

namespace imos::transports::ws40 {

/// Certificate policy per the spec: CA-issued certificates are always
/// accepted; self-signed certificates are admissible by default (the spec
/// expects devices to accept them, and recommends a disable option when
/// they are on by default — that option is allowSelfSignedPeers=false);
/// pinning is available on top (trust exactly one certificate file).
struct TlsOptions {
    /// Server side: serve wss. Client side: inferred from the endpoint URL
    /// scheme; this flag is ignored.
    bool enabled = false;
    std::string certFile;  ///< local identity (required for a wss server)
    std::string keyFile;
    /// Trust anchors for peer verification: "SYSTEM" (default CA bundle),
    /// a file path, or "NONE" (no verification — implied by
    /// allowSelfSignedPeers when no pin is set).
    std::string caFile = "SYSTEM";
    /// Accept self-signed peer certificates (spec: expected of MOS
    /// devices/NCSes; set false to require CA-trusted or pinned peers —
    /// the spec-recommended disable option).
    bool allowSelfSignedPeers = true;
    /// Optional pinning: accept exactly the certificate in this file
    /// (overrides caFile/allowSelfSignedPeers).
    std::string pinnedPeerCertFile;
    bool disableHostnameValidation = false;
    /// Server side only: demand and verify client certificates (mutual
    /// TLS) against caFile/pin. Off by default — the spec's model is a
    /// server certificate plus Basic auth on the upgrade, not mTLS.
    bool requireClientCertificates = false;
};

/// Pluggable server-side credential check (plan §5.1). Implementations must
/// be timing-safe; FixedCredentialVerifier (constant-time) covers the
/// common single-account device case.
class IMOS_TRANSPORTS_API ICredentialVerifier {
public:
    virtual ~ICredentialVerifier() = default;
    [[nodiscard]] virtual bool verify(std::string_view username,
                                      std::string_view password) noexcept = 0;
};

class IMOS_TRANSPORTS_API FixedCredentialVerifier : public ICredentialVerifier {
public:
    explicit FixedCredentialVerifier(imos::ws40::BasicCredentials expected)
        : expected_(std::move(expected)) {}
    [[nodiscard]] bool verify(std::string_view username,
                              std::string_view password) noexcept override;

private:
    imos::ws40::BasicCredentials expected_;
};

/// Diagnostic sink for transport-level protocol errors (frame-type
/// mismatches, rejected upgrades): (channel, diagnostic).
using TransportErrorHandler = std::function<void(Channel, const std::string&)>;

/// Where a transport's pump delivers connectivity changes and inbound
/// frames. PeerSession is the usual destination (the pump(PeerSession&)
/// overloads); Ws40Endpoint interposes one to merge two transports.
struct ChannelEventSink {
    std::function<void(Channel)> connected;
    std::function<void(Channel)> disconnected;
    std::function<void(Channel, std::span<const std::uint8_t>)> frame;
};

// ---------------------------------------------------------------- client --

/// Shared by Ws40Client (IXWebSocket) and Ws40EmscriptenClient (browser,
/// <imos/transports/ws40_emscripten.h>). Browser limitation: the WebSocket
/// API cannot set request headers, so `auth` is rejected there, and TLS
/// trust is the browser's decision (`tls` ignored).
struct ClientConfig {
    /// Peer endpoint, e.g. "wss://ncs.example.com/mos/Communication".
    /// May already carry vendor query parameters; ws:// is permitted for
    /// lab/local use (plan §12).
    std::string endpointUrl;
    std::string mosID;
    std::string ncsID;
    /// Vendor-extra query parameters, appended verbatim (plan §5.1).
    std::vector<std::pair<std::string, std::string>> extraParams;
    /// Channels this client opens. Standard mode: the channels this side
    /// originates traffic on (the peer dials its own for the traffic it
    /// originates — see Ws40Endpoint). Passive mode: every channel the
    /// relationship uses (the external party originates back down these
    /// connections).
    std::vector<Channel> channels{Channel::Mom, Channel::Ro};
    /// Adds passive=true to the URL: this side is the party inside the
    /// firewall, and invites the listening party to originate its own
    /// messages on these connections. Without it the connections carry
    /// only this side's requests and the peer's responses to them.
    bool passive = false;

    /// §3.4 wire encoding — selects binary (UCS-2) vs text (UTF-8) frames.
    /// Must match the PeerSession's wireEncode/wireDecode configuration.
    encoding::WireEncoding wireEncoding = encoding::WireEncoding::Ucs2Be;
    bool enableDeflate = true;                         ///< permessage-deflate (zlib)
    std::optional<imos::ws40::BasicCredentials> auth;  ///< Authorization: Basic
    TlsOptions tls;
    ReconnectConfig reconnect;
    int handshakeTimeoutSecs = 10;
};

#if !defined(__EMSCRIPTEN__)

class IMOS_TRANSPORTS_API Ws40Client : public ITransport {
public:
    Ws40Client(ClientConfig config, IClock* clock, IRandom* random = nullptr,
               ILogger* logger = nullptr);
    ~Ws40Client() override;

    Ws40Client(const Ws40Client&) = delete;
    Ws40Client& operator=(const Ws40Client&) = delete;

    /// Begin connecting every configured channel (asynchronous).
    Result<void> start();
    void stop();

    // ITransport
    Result<void> send(Channel channel, std::span<const std::uint8_t> frame) override;
    [[nodiscard]] bool isConnected(Channel channel) const override;

    /// Drain queued connectivity changes and inbound frames into the
    /// session and drive reconnection. Call from the host's poll loop,
    /// before session.poll().
    void pump(PeerSession& session);
    /// The same drain, delivered to an arbitrary sink (Ws40Endpoint).
    void pump(const ChannelEventSink& sink);

    void setErrorHandler(TransportErrorHandler handler);
    /// Wake-up hook: called on a transport thread whenever pump() has
    /// something new to drain. Install before start(); see
    /// <imos/transports/activity.h> for the contract.
    void setActivityHandler(ActivityHandler handler);

    /// Earliest instant pump() has scheduled work (reconnect attempts),
    /// for host scheduling alongside PeerSession::nextDeadline().
    [[nodiscard]] std::optional<MonotonicTime> nextDeadline() const;
    /// Whether this client dials with passive=true.
    [[nodiscard]] bool passive() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------- server --

/// A peer relationship the server accepts connections for. Connections
/// claiming an unknown mosID/ncsID pairing are rejected at the upgrade.
struct ServerPeerConfig {
    std::string mosID;
    std::string ncsID;
    /// §3.4 expectation for this peer's frames (binary UCS-2 by default).
    encoding::WireEncoding wireEncoding = encoding::WireEncoding::Ucs2Be;
    /// This peer's own credential check, used instead of
    /// ServerConfig::credentials for connections claiming this mosID/ncsID
    /// pairing — so one device's password cannot claim another device's
    /// identity on a shared listener. Must outlive the registration. When
    /// any peer has one, a connection claiming an unknown or malformed
    /// identity is refused as unauthorized (4401), not as unknown (4404),
    /// so the refusal reveals nothing about which identities exist.
    ICredentialVerifier* credentials = nullptr;
};

struct ServerConfig {
    std::string host = "0.0.0.0";
    /// 0 → the first start() picks a free ephemeral port (probe + bind,
    /// retried against the probe race) and port() reports the choice, which
    /// then sticks for the server's lifetime; production deployments
    /// typically pin 80/443.
    int port = 0;
    TlsOptions tls;
    bool enableDeflate = true;
    /// When set, the upgrade must carry valid Basic credentials; verified
    /// timing-safely. Unauthenticated connections are closed before any
    /// frame is processed.
    ICredentialVerifier* credentials = nullptr;
    int handshakeTimeoutSecs = 10;
    std::size_t maxConnections = 32;
};

/// Server-side transport events, drained by the host loop.
struct ServerEvent {
    enum class Kind : std::uint8_t {
        ChannelConnected,     ///< a validated connection claimed a channel
        ChannelDisconnected,  ///< a claimed connection dropped
        Frame,                ///< one inbound WebSocket message
        ProtocolError,        ///< frame-type/encoding policy violation
        Rejected,             ///< upgrade rejected (auth/params/duplicate)
    };
    Kind kind = Kind::Rejected;
    std::string mosID;  ///< empty for Rejected before identification
    std::string ncsID;
    Channel channel = Channel::Mom;
    /// ChannelConnected: the connection's URL carried passive=true, i.e.
    /// the peer invited this side to originate on it.
    bool passive = false;
    std::vector<std::uint8_t> frame;
    std::string diagnostic;
};

class IMOS_TRANSPORTS_API Ws40Server {
public:
    explicit Ws40Server(ServerConfig config, ILogger* logger = nullptr);
    ~Ws40Server();

    Ws40Server(const Ws40Server&) = delete;
    Ws40Server& operator=(const Ws40Server&) = delete;

    /// Register a peer relationship; must precede connections claiming it.
    /// Allowed before or after start().
    Result<void> addPeer(const ServerPeerConfig& peer);
    /// Unregister a peer and close the connections it holds, leaving every
    /// other peer's connections alone (a shared listener can lose one device
    /// without dropping the rest). Its transport — the reference
    /// transportFor() returned — is destroyed: whatever used it, such as the
    /// peer's PeerSession or Ws40Endpoint, must be gone first. Events of the
    /// peer still queued are delivered by nextEvent() as before; hosts drop
    /// them for identities they no longer know. NotFound if not registered.
    Result<void> removePeer(std::string_view mosID, std::string_view ncsID);

    /// Bind and listen. With config.port == 0 a free ephemeral port is
    /// probed and the bind retried a few times — the probe socket must
    /// close before the listener binds, so another process can steal the
    /// port in between. A restart re-binds the port the first start()
    /// resolved, so clients pointed at it can reconnect.
    Result<void> start();
    void stop();
    /// The bound listen port (after start() resolves an ephemeral request);
    /// stays reported across stop().
    [[nodiscard]] int port() const;

    /// The per-peer ITransport over the connections that peer opened to us.
    /// sendReply() answers on the connection currently claiming the channel
    /// — the one the peer's request arrived on. send() originates, and is
    /// accepted only on a connection opened with passive=true (the inner
    /// party's held connections); on a standard-mode connection it fails,
    /// because this side must open its own connection for the messages it
    /// originates. Hand it straight to the PeerSession for a passive-mode
    /// peer; wrap it in a Ws40Endpoint with a Ws40Client for standard mode.
    /// Valid after addPeer; stable for the server's lifetime.
    [[nodiscard]] ITransport& transportFor(std::string_view mosID, std::string_view ncsID);

    /// Drain one queued event, or nullopt. The host routes Frame /
    /// ChannelConnected / ChannelDisconnected into the matching peer's
    /// PeerSession (onFrameReceived / onChannelConnected / …).
    [[nodiscard]] std::optional<ServerEvent> nextEvent();

    /// Wake-up hook: called on a server thread whenever nextEvent() has
    /// something new. Install before start(); see
    /// <imos/transports/activity.h> for the contract.
    void setActivityHandler(ActivityHandler handler);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// -------------------------------------------------------------- endpoint --

/// One peer relationship's transport in standard mode: the connections this
/// side dialed (a Ws40Client) together with the connections the peer dialed
/// to us (that peer's Ws40Server transport), presented to the PeerSession as
/// a single ITransport.
///
/// Routing follows the spec literally — a connection carries its opener's
/// requests and the responses to them:
///  - send() (a message this side originates) goes out on the connection
///    this side dialed. If there is none, it goes to the accepted side,
///    which takes it only when the peer connected with passive=true.
///  - sendReply() (a response) returns on the connection the request came
///    in on: the peer's accepted connection — or this side's own dialed
///    connection when it was opened with passive=true, since that is then
///    where the peer originates.
/// A channel is up for the session while either connection is.
///
/// Either side may be null: dialed-only is a plain client, accepted-only a
/// plain server peer. The endpoint owns neither.
class IMOS_TRANSPORTS_API Ws40Endpoint : public ITransport {
public:
    Ws40Endpoint(Ws40Client* dialed, ITransport* accepted);

    // ITransport
    Result<void> send(Channel channel, std::span<const std::uint8_t> frame) override;
    Result<void> sendReply(Channel channel, std::span<const std::uint8_t> frame) override;
    [[nodiscard]] bool isConnected(Channel channel) const override;

    /// Drain the dialed side into the session and drive its reconnection.
    /// Call from the host's poll loop, before session.poll().
    void pump(PeerSession& session);
    /// Route one of this peer's Ws40Server events (ChannelConnected /
    /// ChannelDisconnected / Frame) into the session. Returns false for the
    /// kinds it does not consume (ProtocolError, Rejected).
    bool onServerEvent(const ServerEvent& event, PeerSession& session);

    /// The same two entry points delivering to a sink instead — for hosts
    /// that feed a role facade (imos::Device / incs::Ncs), whose
    /// onChannelConnected / onFrameReceived run facade-level hooks the bare
    /// session does not. The sink sees the merged channel view: connected
    /// when the first of the two connections comes up, disconnected when
    /// the last goes down.
    void pump(const ChannelEventSink& sink);
    bool onServerEvent(const ServerEvent& event, const ChannelEventSink& sink);

    [[nodiscard]] std::optional<MonotonicTime> nextDeadline() const;

private:
    void linkChanged(Channel channel, bool dialedSide, bool up, const ChannelEventSink& sink);

    Ws40Client* dialed_;
    ITransport* accepted_;
    std::array<bool, kChannelCount> dialedUp_{};
    std::array<bool, kChannelCount> acceptedUp_{};
};

#endif  // !defined(__EMSCRIPTEN__)

}  // namespace imos::transports::ws40
