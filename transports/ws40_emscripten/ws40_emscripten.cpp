// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/transports/ws40_emscripten.h>

#if defined(__EMSCRIPTEN__)

#include <array>
#include <cstring>
#include <deque>
#include <string>

#include <emscripten/websocket.h>
#include <pthread.h>

#include <imos/ws40/url.h>

namespace imos::transports::ws40 {

namespace {

bool wantsTextFrames(encoding::WireEncoding encoding) {
    return encoding == encoding::WireEncoding::Utf8;
}

struct QueuedEvent {
    enum class Kind : std::uint8_t { Connected, Disconnected, Frame };
    Kind kind = Kind::Frame;
    Channel channel = Channel::Mom;
    std::vector<std::uint8_t> payload;
    bool text = false;
};

}  // namespace

struct Ws40EmscriptenClient::Impl {
    ClientConfig config;
    IClock* clock = nullptr;
    ILogger* logger = nullptr;
    TransportErrorHandler errorHandler;

    struct ChannelSlot {
        Impl* owner = nullptr;  // for the C callback trampolines
        Channel channel = Channel::Mom;
        bool wanted = false;
        EMSCRIPTEN_WEBSOCKET_T socket = 0;  // 0 = none
        std::unique_ptr<ReconnectPolicy> policy;
        bool sessionConnected = false;
        bool open = false;
        bool downReported = false;
    };
    std::array<ChannelSlot, kChannelCount> slots;
    // Single-strand by design (see the header's owning-thread contract):
    // callbacks are registered with Emscripten's calling-thread context, so
    // they are delivered on the thread that called start()/pump() — never
    // concurrently with pump(). No locking is needed.
    std::deque<QueuedEvent> queue;
    bool started = false;
    pthread_t owningThread{};  ///< the thread that called start()
    bool crossThreadReported = false;

    ChannelSlot& slot(Channel channel) { return slots[static_cast<std::size_t>(channel)]; }

    /// Owning-thread misuse guard: diagnose (once) calls from a thread other
    /// than the one that started the transport — the callbacks would be
    /// delivered to the wrong thread and the single-strand model broken.
    void checkOwningThread(const char* operation) {
        if (!started || pthread_equal(owningThread, pthread_self()) != 0) {
            return;
        }
        if (crossThreadReported) {
            return;
        }
        crossThreadReported = true;
        const std::string diagnostic =
            std::string{"ws40_emscripten: "} + operation +
            " called from a thread other than the one that called start(); the transport "
            "is single-strand — one thread must own start/pump/send/stop";
        log(LogLevel::Error, diagnostic);
        if (errorHandler) {
            errorHandler(Channel::Mom, diagnostic);
        }
    }

    void log(LogLevel level, const std::string& text) {
        if (logger != nullptr) {
            logger->log(level, text);
        }
    }

    std::string channelUrl(Channel channel) const {
        imos::ws40::ConnectionParams params;
        params.mosID = config.mosID;
        params.ncsID = config.ncsID;
        params.channel = channel;
        params.passive = config.passive;
        params.extras = config.extraParams;
        return imos::ws40::composeUrl(config.endpointUrl, params);
    }

    void reportDown(ChannelSlot& slot) {
        if (slot.downReported) {
            return;
        }
        slot.downReported = true;
        slot.open = false;
        queue.push_back({QueuedEvent::Kind::Disconnected, slot.channel, {}, false});
    }

    static EM_BOOL onOpen(int /*eventType*/, const EmscriptenWebSocketOpenEvent* /*event*/,
                          void* userData) {
        auto* slot = static_cast<ChannelSlot*>(userData);
        slot->open = true;
        slot->downReported = false;
        slot->owner->queue.push_back({QueuedEvent::Kind::Connected, slot->channel, {}, false});
        return EM_TRUE;
    }

    static EM_BOOL onError(int /*eventType*/, const EmscriptenWebSocketErrorEvent* /*event*/,
                           void* userData) {
        auto* slot = static_cast<ChannelSlot*>(userData);
        slot->owner->reportDown(*slot);
        return EM_TRUE;
    }

    static EM_BOOL onClose(int /*eventType*/, const EmscriptenWebSocketCloseEvent* /*event*/,
                           void* userData) {
        auto* slot = static_cast<ChannelSlot*>(userData);
        slot->owner->reportDown(*slot);
        return EM_TRUE;
    }

    static EM_BOOL onMessage(int /*eventType*/, const EmscriptenWebSocketMessageEvent* event,
                             void* userData) {
        auto* slot = static_cast<ChannelSlot*>(userData);
        QueuedEvent queued;
        queued.kind = QueuedEvent::Kind::Frame;
        queued.channel = slot->channel;
        queued.text = event->isText == EM_TRUE;
        std::size_t length = event->numBytes;
        // For text messages numBytes counts the NUL terminator appended by
        // the Emscripten runtime; strip it.
        if (queued.text && length > 0 && event->data[length - 1] == 0) {
            --length;
        }
        queued.payload.assign(event->data, event->data + length);
        slot->owner->queue.push_back(std::move(queued));
        return EM_TRUE;
    }

    void closeSocket(ChannelSlot& slot) {
        if (slot.socket != 0) {
            emscripten_websocket_close(slot.socket, 1000, "shutdown");
            emscripten_websocket_delete(slot.socket);
            slot.socket = 0;
        }
        slot.open = false;
    }

    Result<void> connect(ChannelSlot& slot) {
        closeSocket(slot);
        const std::string url = channelUrl(slot.channel);
        EmscriptenWebSocketCreateAttributes attributes = {url.c_str(), nullptr, EM_TRUE};
        const EMSCRIPTEN_WEBSOCKET_T socket = emscripten_websocket_new(&attributes);
        if (socket <= 0) {
            return makeError(ErrorCode::IoError,
                             "emscripten_websocket_new failed with " + std::to_string(socket));
        }
        slot.socket = socket;
        slot.downReported = false;
        emscripten_websocket_set_onopen_callback(socket, &slot, &Impl::onOpen);
        emscripten_websocket_set_onerror_callback(socket, &slot, &Impl::onError);
        emscripten_websocket_set_onclose_callback(socket, &slot, &Impl::onClose);
        emscripten_websocket_set_onmessage_callback(socket, &slot, &Impl::onMessage);
        return {};
    }
};

Ws40EmscriptenClient::Ws40EmscriptenClient(ClientConfig config, IClock* clock, IRandom* random,
                                           ILogger* logger)
    : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    impl_->clock = clock;
    impl_->logger = logger;
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        impl_->slots[i].owner = impl_.get();
        impl_->slots[i].channel = static_cast<Channel>(i);
    }
    for (const Channel channel : impl_->config.channels) {
        Impl::ChannelSlot& slot = impl_->slot(channel);
        slot.wanted = true;
        slot.policy = std::make_unique<ReconnectPolicy>(impl_->config.reconnect, clock, random);
    }
}

Ws40EmscriptenClient::~Ws40EmscriptenClient() {
    stop();
}

Result<void> Ws40EmscriptenClient::start() {
    if (impl_->started) {
        return makeError(ErrorCode::WrongState, "client already started");
    }
    if (emscripten_websocket_is_supported() != EM_TRUE) {
        return makeError(ErrorCode::IoError, "WebSocket is not supported in this environment");
    }
    if (impl_->config.auth.has_value()) {
        return makeError(ErrorCode::InvalidArgument,
                         "the browser WebSocket API cannot set an Authorization header; "
                         "authenticate via vendor URL parameters or cookies instead");
    }
    for (Impl::ChannelSlot& slot : impl_->slots) {
        if (!slot.wanted) {
            continue;
        }
        Result<void> connected = impl_->connect(slot);
        if (!connected.ok()) {
            return connected;
        }
    }
    // The calling thread becomes the owning thread: callbacks were just
    // registered with the calling-thread context, so every later call must
    // come from this thread (see the header's owning-thread contract).
    impl_->owningThread = pthread_self();
    impl_->crossThreadReported = false;
    impl_->started = true;
    return {};
}

void Ws40EmscriptenClient::stop() {
    for (Impl::ChannelSlot& slot : impl_->slots) {
        impl_->closeSocket(slot);
    }
    impl_->started = false;
}

Result<void> Ws40EmscriptenClient::send(Channel channel, std::span<const std::uint8_t> frame) {
    impl_->checkOwningThread("send()");
    Impl::ChannelSlot& slot = impl_->slot(channel);
    if (slot.socket == 0 || !slot.open) {
        return makeError(
            ErrorCode::IoError,
            std::string{"ws40 "} + std::string{channelName(channel)} + " channel is not connected");
    }
    EMSCRIPTEN_RESULT result = EMSCRIPTEN_RESULT_SUCCESS;
    if (wantsTextFrames(impl_->config.wireEncoding)) {
        const std::string text{reinterpret_cast<const char*>(frame.data()), frame.size()};
        result = emscripten_websocket_send_utf8_text(slot.socket, text.c_str());
    } else {
        result = emscripten_websocket_send_binary(
            slot.socket, const_cast<std::uint8_t*>(frame.data()), frame.size());
    }
    if (result != EMSCRIPTEN_RESULT_SUCCESS) {
        return makeError(ErrorCode::IoError,
                         "WebSocket send failed with " + std::to_string(result));
    }
    return {};
}

bool Ws40EmscriptenClient::isConnected(Channel channel) const {
    const Impl::ChannelSlot& slot = impl_->slots[static_cast<std::size_t>(channel)];
    return slot.socket != 0 && slot.open;
}

void Ws40EmscriptenClient::pump(PeerSession& session) {
    impl_->checkOwningThread("pump()");
    std::deque<QueuedEvent> local;
    local.swap(impl_->queue);
    for (QueuedEvent& event : local) {
        Impl::ChannelSlot& slot = impl_->slot(event.channel);
        switch (event.kind) {
            case QueuedEvent::Kind::Connected:
                slot.policy->onConnected();
                slot.sessionConnected = true;
                session.onChannelConnected(event.channel);
                break;
            case QueuedEvent::Kind::Disconnected:
                slot.policy->onDisconnected();
                if (slot.sessionConnected) {
                    slot.sessionConnected = false;
                    session.onChannelDisconnected(event.channel);
                }
                break;
            case QueuedEvent::Kind::Frame: {
                const bool expectText = wantsTextFrames(impl_->config.wireEncoding);
                if (event.text != expectText) {
                    const std::string diagnostic =
                        event.text ? "peer sent a text (UTF-8) WebSocket frame but this peer is "
                                     "configured for UCS-2 binary frames — align the per-peer "
                                     "encoding configuration (§3.4)"
                                   : "peer sent a binary WebSocket frame but this peer is "
                                     "configured for UTF-8 text frames — align the per-peer "
                                     "encoding configuration (§3.4)";
                    impl_->log(LogLevel::Error, diagnostic);
                    if (impl_->errorHandler) {
                        impl_->errorHandler(event.channel, diagnostic);
                    }
                    break;
                }
                session.onFrameReceived(event.channel, event.payload);
                break;
            }
        }
    }

    if (!impl_->started) {
        return;
    }
    for (Impl::ChannelSlot& slot : impl_->slots) {
        if (!slot.wanted || slot.sessionConnected) {
            continue;
        }
        if (slot.policy->shouldAttempt()) {
            const Result<void> connected = impl_->connect(slot);
            if (!connected.ok()) {
                slot.policy->onDisconnected();  // count it and back off
            }
        }
    }
}

void Ws40EmscriptenClient::setErrorHandler(TransportErrorHandler handler) {
    impl_->errorHandler = std::move(handler);
}

std::optional<MonotonicTime> Ws40EmscriptenClient::nextDeadline() const {
    std::optional<MonotonicTime> earliest;
    for (const Impl::ChannelSlot& slot : impl_->slots) {
        if (!slot.wanted || slot.policy == nullptr) {
            continue;
        }
        const std::optional<MonotonicTime> at = slot.policy->nextAttemptAt();
        if (at.has_value() && (!earliest.has_value() || *at < *earliest)) {
            earliest = at;
        }
    }
    return earliest;
}

}  // namespace imos::transports::ws40

#endif  // defined(__EMSCRIPTEN__)
