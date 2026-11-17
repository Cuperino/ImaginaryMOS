// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// MOS 3.x WebService transport (plan §5.3, milestone M6; design of record
// reference/m6-plan.md §2). One endpoint plays both halves of the 3.8.2
// WSDL world: it hosts the MOSWebService (IXWebSocket HttpServer) for the
// peer's calls and calls the peer's service (IXWebSocket HttpClient) for
// its own — SOAP 1.1 document/literal, one TCP port, UTF-8; the channel
// concept collapses onto the single pipe (send()'s channel parameter only
// routes the synthesized replies back to the session).
//
// Request/response IS the ack: an outbound call's HTTP response is
// unwrapped into the classic reply and fed back through pump(), clearing
// the session's in-flight exchange; an inbound call parks its HTTP
// exchange until the session's reply arrives via send() (correlated by
// messageID), which becomes the SOAP response — a parked exchange that
// outlives its timeout answers with a SOAP Fault.
//
// Threading contract mirrors the other transports: IXWebSocket's server
// threads and this endpoint's one outbound worker only touch queues; the
// host drains everything into the sans-I/O PeerSession via pump().
// Sessions speaking ws3 must run WireEncoding::Utf8.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include <imos/error.h>
#include <imos/protocol.h>
#include <imos/session/peer_session.h>
#include <imos/session/transport.h>
#include <imos/transports/activity.h>
#include <imos/transports/channel_events.h>
#include <imos/transports_export.h>
#include <imos/ws40/basic_auth.h>

namespace imos::transports::ws3 {

struct EndpointConfig {
    /// The peer's service URL, e.g. "http://ncs.example.com:8080/mos". The
    /// 3.8.2 WSDL's placeholder is an .asmx path; real deployments vary, so
    /// the URL is taken verbatim.
    std::string peerServiceUrl;
    std::string bindHost = "0.0.0.0";
    /// 0 → an ephemeral port is probed at start() and port() reports it.
    int bindPort = 0;
    /// Session identities, used to rebuild the classic envelope of replies
    /// synthesized from SOAP responses.
    std::string mosID;
    std::string ncsID;
    /// Outbound Basic auth (the 3.x service is plain HTTP infrastructure;
    /// credentialed deployments exist).
    std::optional<imos::ws40::BasicCredentials> auth;
    /// Advertise gzip on outbound calls (zlib via IXWebSocket).
    bool acceptGzipResponses = true;
    int requestTimeoutSecs = 10;           ///< outbound HTTP round trip
    int inboundReplyTimeoutMillis = 5000;  ///< parked-exchange wait
};

/// Diagnostic sink: HTTP failures, transform rejections, parked-exchange
/// timeouts. (channel, diagnostic).
using Ws3ErrorHandler = std::function<void(Channel, const std::string&)>;

class IMOS_TRANSPORTS_API Ws3Endpoint : public ITransport {
public:
    Ws3Endpoint(EndpointConfig config, ILogger* logger = nullptr);
    ~Ws3Endpoint() override;

    Ws3Endpoint(const Ws3Endpoint&) = delete;
    Ws3Endpoint& operator=(const Ws3Endpoint&) = delete;

    /// Bind and start the service and the outbound worker. All channels
    /// report connected once started — HTTP is connectionless, so per-call
    /// failures surface as diagnostics and ride the session's retry timer.
    Result<void> start();
    void stop();

    // ITransport. `frame` is a classic UTF-8 <mos> document: a reply whose
    // messageID matches a parked inbound exchange becomes that exchange's
    // SOAP response; anything else becomes an outbound SOAP call.
    Result<void> send(Channel channel, std::span<const std::uint8_t> frame) override;
    [[nodiscard]] bool isConnected(Channel channel) const override;

    /// Drain synthesized replies, inbound calls, and diagnostics into the
    /// session. Call from the host's poll loop, before session.poll().
    void pump(PeerSession& session);
    /// The same, into a sink of the host's: connectivity changes and
    /// inbound frames, for a host that sees the traffic on its way to its
    /// session (a facade such as incs::Ncs, or a traffic log).
    void pump(const ChannelEventSink& sink);

    void setErrorHandler(Ws3ErrorHandler handler);
    /// Wake-up hook: called on a transport thread whenever pump() has
    /// something new to drain. Install before start(); see
    /// <imos/transports/activity.h> for the contract.
    void setActivityHandler(ActivityHandler handler);

    /// Re-point the peer service URL (config.peerServiceUrl). Two endpoints
    /// binding ephemeral ports cannot know each other's URLs up front —
    /// start both, then exchange; also covers failover-style re-pointing.
    void setPeerServiceUrl(std::string url);

    /// The bound service port (resolves an ephemeral request after start()).
    [[nodiscard]] int port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace imos::transports::ws3
