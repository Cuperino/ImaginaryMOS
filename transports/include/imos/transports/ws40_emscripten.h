// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// MOS 4.0 WebSocket client for WebAssembly (plan §5.4), over
// emscripten/websocket.h. Client-only by construction — the browser sandbox
// permits no listeners — which is exactly what MOS 4.0 passive mode needs:
// the inner party opens outbound connections per channel and the external
// party originates back down them, so a browser-hosted imos device is fully
// functional in passive mode.
//
// Threading contract — the *owning thread* model: the thread that calls
// start() owns the transport. That same thread must make every subsequent
// call (pump()/send()/stop()) and is where the WebSocket callbacks are
// delivered (they are registered with Emscripten's calling-thread context).
// In a single-threaded build the owning thread is the main browser thread —
// callbacks arrive between turns of the event loop. In a multithreaded
// (-pthread) build a dedicated worker pthread may own the transport instead:
// the underlying JS WebSocket object lives on the main thread
// (createOnMainThread), so socket operations proxy to it — the main thread
// must stay responsive or those calls stall — and callbacks proxy back to
// the owning pthread, which must service Emscripten's queued calls (any
// event loop or futex-based wait does; a busy-spinning thread does not).
// The sans-I/O single-strand model absorbs all of this; no locking is used
// or needed. Cross-thread misuse is reported once through the error handler.
//
// Browser realities (documented limitations, not bugs): request headers
// cannot be set, so HTTP Basic auth is unavailable — start() fails if
// ClientConfig.auth is set; TLS trust decisions belong to the browser
// (ClientConfig.tls is ignored), and wss:// is mandatory from secure pages.

#pragma once

#if defined(__EMSCRIPTEN__)

#include <memory>
#include <optional>
#include <span>

#include <imos/transports/ws40.h>

namespace imos::transports::ws40 {

class IMOS_TRANSPORTS_API Ws40EmscriptenClient : public ITransport {
public:
    Ws40EmscriptenClient(ClientConfig config, IClock* clock, IRandom* random = nullptr,
                         ILogger* logger = nullptr);
    ~Ws40EmscriptenClient() override;

    Ws40EmscriptenClient(const Ws40EmscriptenClient&) = delete;
    Ws40EmscriptenClient& operator=(const Ws40EmscriptenClient&) = delete;

    /// Begin connecting every configured channel. Fails when the embedding
    /// browser lacks WebSocket support or ClientConfig.auth is set.
    Result<void> start();
    void stop();

    // ITransport
    Result<void> send(Channel channel, std::span<const std::uint8_t> frame) override;
    [[nodiscard]] bool isConnected(Channel channel) const override;

    /// Drain queued connectivity changes and inbound frames into the
    /// session and drive reconnection; call from the host's poll loop
    /// (e.g. an emscripten_set_main_loop tick).
    void pump(PeerSession& session);

    void setErrorHandler(TransportErrorHandler handler);
    [[nodiscard]] std::optional<MonotonicTime> nextDeadline() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace imos::transports::ws40

#endif  // defined(__EMSCRIPTEN__)
