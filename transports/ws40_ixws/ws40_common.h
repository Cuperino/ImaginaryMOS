// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Internal helpers shared by the ws40_ixws client and server. Not installed.

#pragma once

#include <string>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXSocketTLSOptions.h>

#include <imos/encoding/encoding.h>
#include <imos/transports/ws40.h>

namespace imos::transports::ws40::detail {

/// One-time platform network bring-up (Winsock on Windows; no-op elsewhere).
inline void ensureNetSystem() {
    static const bool initialized = [] { return ix::initNetSystem(); }();
    (void)initialized;
}

/// Map the certificate policy onto IXWebSocket's options: pinning trusts
/// exactly one certificate; the self-signed switch disables chain
/// verification (the spec's "accept self-signed" reading); otherwise the
/// configured CA bundle (default: system) applies.
inline ix::SocketTLSOptions makeSocketTlsOptions(const TlsOptions& tls, bool serverSide) {
    ix::SocketTLSOptions out;
    out.tls = serverSide ? tls.enabled : false;  // client infers from the URL scheme
    out.certFile = tls.certFile;
    out.keyFile = tls.keyFile;
    if (serverSide && !tls.requireClientCertificates) {
        // A CA on the server side makes IXWebSocket demand client
        // certificates (mutual TLS); the spec's model is a server
        // certificate plus Basic auth, so peer verification stays off
        // unless mTLS is explicitly requested.
        out.caFile = "NONE";
    } else if (!tls.pinnedPeerCertFile.empty()) {
        out.caFile = tls.pinnedPeerCertFile;
    } else if (tls.allowSelfSignedPeers) {
        out.caFile = "NONE";
    } else {
        out.caFile = tls.caFile;
    }
    out.disable_hostname_validation = tls.disableHostnameValidation || tls.allowSelfSignedPeers;
    return out;
}

/// §3.4 carriage rule: UCS-2 rides binary frames; UTF-8 rides text frames.
inline bool wantsTextFrames(encoding::WireEncoding encoding) {
    return encoding == encoding::WireEncoding::Utf8;
}

/// Diagnostic for a frame whose WebSocket type contradicts the peer's
/// configured wire encoding — name the likely misconfiguration instead of
/// letting mojibake surface later (plan §3.4).
inline std::string frameTypeMismatchDiagnostic(bool receivedBinary,
                                               encoding::WireEncoding expected) {
    const std::string_view expectedName = encoding::wireEncodingName(expected);
    if (receivedBinary) {
        return std::string{
                   "peer sent a binary WebSocket frame but this peer is configured "
                   "for UTF-8 text frames; the peer is likely configured for "} +
               "UCS-2 wire encoding — align the per-peer encoding configuration (§3.4)";
    }
    return std::string{
               "peer sent a text (UTF-8) WebSocket frame but this peer is configured "
               "for "} +
           std::string{expectedName} +
           " binary frames; the peer is likely configured for UTF-8 wire encoding — align "
           "the per-peer encoding configuration (§3.4)";
}

/// RFC 6455 application close codes used by the acceptor.
inline constexpr std::uint16_t kCloseBadParameters = 4400;
inline constexpr std::uint16_t kCloseUnauthorized = 4401;
inline constexpr std::uint16_t kCloseUnknownPeer = 4404;
inline constexpr std::uint16_t kCloseDuplicateChannel = 4409;

}  // namespace imos::transports::ws40::detail
