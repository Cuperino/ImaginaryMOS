// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// MOS 4.0 connection-URL parameter scheme (plan §3.1/§5.1). Connection URLs
// carry query parameters identifying the parties and the logical channel —
// mosID, ncsID, channel (mom|ro|aux) — plus passive=true for passive-mode
// topologies. Vendor-extra parameters (API keys, configuration hints) are
// preserved verbatim so they survive a round trip through us untouched.
//
// This is one of the four standing fuzz surfaces (plan §10/§12): hard size
// caps are enforced before any parsing work begins, and every rejection
// carries a diagnostic naming what was wrong.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imos/core_export.h>
#include <imos/error.h>
#include <imos/protocol.h>

namespace imos::ws40 {

/// Hard cap on the URI/query text accepted by parseConnectionParams,
/// enforced before any decoding (plan §12).
inline constexpr std::size_t kMaxUriBytes = 8 * 1024;

/// The MOS-defined connection parameters plus everything else on the URL.
struct ConnectionParams {
    std::string mosID;
    std::string ncsID;
    Channel channel = Channel::Mom;
    bool passive = false;

    /// Vendor-extra query parameters in original order, kept verbatim in
    /// their wire (percent-encoded) form: `first` is the raw key, `second`
    /// the raw value ("" for a bare key). Re-emitted unchanged by
    /// composeQuery so vendor round-trips are byte-clean.
    std::vector<std::pair<std::string, std::string>> extras;
};

/// Parse the MOS parameters out of a request URI ("/path?query"), a full URL
/// ("wss://host/path?query"), or a bare query string ("a=1&mosID=…").
/// Requires mosID, ncsID, and channel; rejects duplicated or empty MOS
/// parameters (parameter-smuggling hardening) with a diagnostic naming the
/// offender. passive accepts the spec's "true" (case-insensitive); any other
/// value is an error. Percent-decoding applies to the MOS parameters only;
/// extras stay in wire form. '+' is taken literally, not as space.
IMOS_CORE_API Result<ConnectionParams> parseConnectionParams(std::string_view uri);

/// Compose the query string (no leading '?'): extras first, verbatim and in
/// order (matching the spec's example layout), then the percent-encoded MOS
/// parameters, then passive=true when set.
[[nodiscard]] IMOS_CORE_API std::string composeQuery(const ConnectionParams& params);

/// Append composeQuery(params) to a base endpoint URL that may already carry
/// a query ("wss://host/mos/Communication" or "…?vendor=1").
[[nodiscard]] IMOS_CORE_API std::string composeUrl(std::string_view baseUrl,
                                                   const ConnectionParams& params);

/// Percent-encode one query value per RFC 3986 (unreserved bytes pass).
[[nodiscard]] IMOS_CORE_API std::string percentEncode(std::string_view value);

/// Percent-decode; rejects malformed %-sequences and encoded NUL.
IMOS_CORE_API Result<std::string> percentDecode(std::string_view value);

}  // namespace imos::ws40
