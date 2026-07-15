// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>

#include <imos/transports_export.h>

/// Transport backend availability probes (plan §5). The 4.0 WebSocket
/// ITransport implementations live in <imos/transports/ws40.h> (IXWebSocket)
/// and <imos/transports/ws40_emscripten.h> (WASM).
namespace imos::transports {

/// Whether the IXWebSocket-based 4.0 WebSocket backend is compiled in.
[[nodiscard]] IMOS_TRANSPORTS_API bool ixwebsocketAvailable() noexcept;

/// Whether this build negotiates TLS (wss) — IMOS_ENABLE_TLS (the
/// linux-tls preset; TLS-off builds still accept TlsOptions but fail
/// wss connects at runtime).
[[nodiscard]] IMOS_TRANSPORTS_API bool tlsEnabled() noexcept;

}  // namespace imos::transports
