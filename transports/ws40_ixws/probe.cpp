// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/transports.h>

#include <ixwebsocket/IXWebSocket.h>

namespace imos::transports {

bool ixwebsocketAvailable() noexcept {
    // Touch a real IXWebSocket symbol so the pinned library is compiled and
    // linked. The socket is inert until start()/connect() — no network here.
    ix::WebSocket socket;
    static_cast<void>(socket.getUrl());
    return true;
}

bool tlsEnabled() noexcept {
#ifdef IXWEBSOCKET_USE_TLS
    return true;
#else
    return false;
#endif
}

}  // namespace imos::transports
