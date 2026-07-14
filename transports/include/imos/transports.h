// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>

#include <imos/transports_export.h>

/// Transport backends (plan §5). At M0 this only reports backend availability;
/// the ITransport implementations land from M2.
namespace imos::transports {

/// Whether the IXWebSocket-based 4.0 WebSocket backend is compiled in.
[[nodiscard]] IMOS_TRANSPORTS_API bool ixwebsocketAvailable() noexcept;

/// Whether this build negotiates TLS (wss). Off until M2 enables it.
[[nodiscard]] IMOS_TRANSPORTS_API bool tlsEnabled() noexcept;

}  // namespace imos::transports
