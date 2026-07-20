// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// MOS 3.x SOAP body transform (plan §5.3, design of record
// reference/m6-plan.md §2). The 3.8.2 WSDL (vendored:
// fixtures/wsdl/MOSWebService-3.8.2.wsdl) does not tunnel classic <mos>
// documents — every message is its own document/literal operation in the
// http://mosprotocol.com/webservices/ namespace, with the envelope IDs
// re-homed into a mosHeader complex type, several fields hoisted to the
// wrapper level per operation, repeated elements regrouped into .NET-style
// ArrayOfX containers of <x_type> members, and the HTTP response carrying
// the ack (plus any data reply) synchronously.
//
// This module is the pure transform between those two shapes — no I/O, so
// the ws3 endpoint stays thin and the mapping is unit-testable against the
// WSDL fixture. Sessions speaking ws3 run WireEncoding::Utf8; everything
// here is UTF-8 text.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <imos/codec/message.h>
#include <imos/error.h>
#include <imos/transports_export.h>

namespace imos::transports::ws3 {

inline constexpr std::string_view kSoapEnvelopeNs = "http://schemas.xmlsoap.org/soap/envelope/";
inline constexpr std::string_view kMosWebServicesNs = "http://mosprotocol.com/webservices/";

/// One outbound SOAP call, ready for HTTP POST.
struct SoapRequest {
    std::string operation;   ///< WSDL operation, e.g. "mosObj"
    std::string soapAction;  ///< SOAPAction header value
    std::string envelope;    ///< SOAP 1.1 envelope, UTF-8
    std::string messageId;   ///< classic <messageID>, for reply correlation
};

/// Classic <mos> document → SOAP request. Fails with Unsupported for
/// message types that are not WSDL operations (pure replies and 4.0-only
/// vocabulary) — the caller surfaces the diagnostic.
[[nodiscard]] IMOS_TRANSPORTS_API Result<SoapRequest> buildSoapRequest(
    const codec::Message& message);

/// SOAP response for `operation` → the classic reply document(s). A
/// data-carrying response yields the data reply (the session treats it as
/// the terminal reply, clearing the in-flight exchange); the redundant ack
/// result next to it is dropped (m6-plan §2.2). The reply envelope is
/// rebuilt from the session's IDs plus the request's messageID.
[[nodiscard]] IMOS_TRANSPORTS_API Result<std::vector<codec::Message>> parseSoapResponse(
    std::string_view envelope, std::string_view operation, std::string_view mosID,
    std::string_view ncsID, std::string_view messageId);

/// Inbound SOAP request → the classic <mos> document to feed the session.
struct InboundCall {
    codec::Message message;
    std::string operation;
    std::string messageId;
};
[[nodiscard]] IMOS_TRANSPORTS_API Result<InboundCall> parseSoapRequest(std::string_view envelope);

/// Classic reply message (the session's ack or terminal reply to
/// `operation`) → the SOAP response envelope, synthesizing the ack result
/// element beside a data result where the WSDL wants both.
[[nodiscard]] IMOS_TRANSPORTS_API Result<std::string> buildSoapResponse(const codec::Message& reply,
                                                                        std::string_view operation);

/// A SOAP 1.1 Fault envelope (server-side parked-exchange timeouts and
/// transform failures).
[[nodiscard]] IMOS_TRANSPORTS_API std::string buildSoapFault(std::string_view faultCode,
                                                             std::string_view faultString);

/// True when `type` is one of the 24 WSDL operations (senders gate on this).
[[nodiscard]] IMOS_TRANSPORTS_API bool isWs3Operation(codec::MessageType type) noexcept;

}  // namespace imos::transports::ws3
