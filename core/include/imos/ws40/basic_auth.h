// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// HTTP Basic authorization for the MOS 4.0 WebSocket upgrade (plan §5.1,
// spec "Authentication"): the spec recommends credentials in an
// Authorization header over URL parameters. Pure string work, kept in the
// sans-I/O core so both the IXWebSocket and Emscripten transports (and
// tests) share one implementation. Server-side comparison is constant-time.

#pragma once

#include <string>
#include <string_view>

#include <imos/core_export.h>
#include <imos/error.h>

namespace imos::ws40 {

struct BasicCredentials {
    std::string username;
    std::string password;
};

/// "Basic <base64(user:pass)>" — the Authorization header value. The
/// username must not contain ':' (RFC 7617); returns an error if it does.
IMOS_CORE_API Result<std::string> composeBasicAuthorization(const BasicCredentials& credentials);

/// Parse an Authorization header value; accepts only the Basic scheme
/// (case-insensitive scheme token per RFC 7235).
IMOS_CORE_API Result<BasicCredentials> parseBasicAuthorization(std::string_view headerValue);

/// Timing-safe equality: examines every byte regardless of where the first
/// mismatch is, so comparison time leaks nothing about the expected value.
[[nodiscard]] IMOS_CORE_API bool constantTimeEquals(std::string_view a,
                                                    std::string_view b) noexcept;

/// Constant-time credential check (username and password both timing-safe).
[[nodiscard]] IMOS_CORE_API bool verifyCredentials(const BasicCredentials& presented,
                                                   const BasicCredentials& expected) noexcept;

}  // namespace imos::ws40
