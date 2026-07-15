// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// MOS 4.0 WebSocket transport over IXWebSocket (plan §5.1, milestone M2).
// Client and server roles for standard and passive topologies. Threading
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
    /// Channels this client opens. Standard mode: the channels this role
    /// originates traffic on. Passive mode: every channel the relationship
    /// uses (the external party originates back down these connections).
    std::vector<Channel> channels{Channel::Mom, Channel::Ro};
    bool passive = false;  ///< adds passive=true to the URL

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

    void setErrorHandler(TransportErrorHandler handler);

    /// Earliest instant pump() has scheduled work (reconnect attempts),
    /// for host scheduling alongside PeerSession::nextDeadline().
    [[nodiscard]] std::optional<MonotonicTime> nextDeadline() const;

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
};

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 0;  ///< 0 → ephemeral (tests); production typically 80/443
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
    Result<void> addPeer(const ServerPeerConfig& peer);

    Result<void> start();
    void stop();
    /// The bound listen port (resolves an ephemeral request).
    [[nodiscard]] int port() const;

    /// The per-peer ITransport to hand to that peer's PeerSession. send()
    /// uses the connection currently claiming the channel — in passive mode
    /// these are the inner party's held connections; in standard mode,
    /// replies ride the inbound connection the request arrived on. Valid
    /// after addPeer; stable for the server's lifetime.
    [[nodiscard]] ITransport& transportFor(std::string_view mosID, std::string_view ncsID);

    /// Drain one queued event, or nullopt. The host routes Frame /
    /// ChannelConnected / ChannelDisconnected into the matching peer's
    /// PeerSession (onFrameReceived / onChannelConnected / …).
    [[nodiscard]] std::optional<ServerEvent> nextEvent();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // !defined(__EMSCRIPTEN__)

}  // namespace imos::transports::ws40
