// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// The transport seam between the sans-I/O session and the outside world.
// Concrete implementations live under transports/ (ws40_ixws, tcp28, …);
// tests inject fakes. Outbound frames flow through this interface; inbound
// frames and connectivity changes are pushed INTO the session by the host
// glue (PeerSession::onFrameReceived / onChannelConnected), keeping the core
// free of callbacks-from-below.

#pragma once

#include <cstdint>
#include <span>

#include <imos/error.h>
#include <imos/protocol.h>

namespace imos {

class ITransport {
public:
    virtual ~ITransport() = default;

    /// Send one complete MOS document as one frame (one message per
    /// WebSocket frame in 4.0; back-to-back bytes on a 2.8 stream). The
    /// bytes are already in the peer's configured wire encoding.
    virtual Result<void> send(Channel channel, std::span<const std::uint8_t> frame) = 0;

    /// Send a frame that answers a message the peer originated (an ack, a
    /// list reply, a heartbeat echo). Transports with one connection per
    /// channel treat it exactly like send() — the default. The 2.8 TCP
    /// transport holds up to two sockets per port (one each side dialed) and
    /// must return a response "down the same socket on which the original
    /// message was received", so it routes these differently.
    virtual Result<void> sendReply(Channel channel, std::span<const std::uint8_t> frame) {
        return send(channel, frame);
    }

    [[nodiscard]] virtual bool isConnected(Channel channel) const = 0;
};

}  // namespace imos
