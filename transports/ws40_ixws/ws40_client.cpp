// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/transports/ws40.h>

#include <array>
#include <atomic>
#include <deque>
#include <mutex>

#include <ixwebsocket/IXWebSocket.h>

#include <imos/ws40/url.h>

#include "ws40_common.h"

namespace imos::transports::ws40 {

namespace {

struct QueuedEvent {
    enum class Kind : std::uint8_t { Connected, Disconnected, Frame };
    Kind kind = Kind::Frame;
    Channel channel = Channel::Mom;
    std::string payload;
    bool binary = false;
};

}  // namespace

struct Ws40Client::Impl {
    ClientConfig config;
    IClock* clock = nullptr;
    ILogger* logger = nullptr;
    TransportErrorHandler errorHandler;

    struct ChannelSlot {
        bool wanted = false;
        std::unique_ptr<ix::WebSocket> socket;
        std::unique_ptr<ReconnectPolicy> policy;
        /// Pump-thread view: has the session been told this channel is up?
        bool sessionConnected = false;
        /// Callback-thread view, so a failed attempt reports down exactly
        /// once even when IXWebSocket emits Error and Close back to back.
        std::atomic<bool> openSeen{false};
        std::atomic<bool> downReported{false};
    };
    std::array<ChannelSlot, kChannelCount> slots;

    std::mutex queueMutex;
    std::deque<QueuedEvent> queue;
    bool started = false;

    ChannelSlot& slot(Channel channel) { return slots[static_cast<std::size_t>(channel)]; }
    const ChannelSlot& slot(Channel channel) const {
        return slots[static_cast<std::size_t>(channel)];
    }

    void log(LogLevel level, const std::string& text) {
        if (logger != nullptr) {
            logger->log(level, text);
        }
    }

    void enqueue(QueuedEvent event) {
        const std::scoped_lock lock{queueMutex};
        queue.push_back(std::move(event));
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

    Result<void> configureSocket(Channel channel) {
        ChannelSlot& s = slot(channel);
        s.socket = std::make_unique<ix::WebSocket>();
        s.socket->setUrl(channelUrl(channel));
        s.socket->disableAutomaticReconnection();  // ReconnectPolicy drives retries
        s.socket->setHandshakeTimeout(config.handshakeTimeoutSecs);
        s.socket->setPingInterval(0);  // liveness belongs to keepAlive/heartbeat
        if (config.enableDeflate) {
            s.socket->enablePerMessageDeflate();
        } else {
            s.socket->disablePerMessageDeflate();
        }
        s.socket->setTLSOptions(detail::makeSocketTlsOptions(config.tls, /*serverSide=*/false));
        if (config.auth.has_value()) {
            Result<std::string> header = imos::ws40::composeBasicAuthorization(*config.auth);
            if (!header.ok()) {
                return std::move(header).error();
            }
            ix::WebSocketHttpHeaders headers;
            headers["Authorization"] = header.value();
            s.socket->setExtraHeaders(headers);
        }

        ChannelSlot* slotPtr = &s;
        s.socket->setOnMessageCallback(
            [this, channel, slotPtr](const ix::WebSocketMessagePtr& msg) {
                switch (msg->type) {
                    case ix::WebSocketMessageType::Open:
                        slotPtr->openSeen = true;
                        slotPtr->downReported = false;
                        enqueue({QueuedEvent::Kind::Connected, channel, {}, false});
                        break;
                    case ix::WebSocketMessageType::Close:
                    case ix::WebSocketMessageType::Error:
                        // A failed attempt can emit Error then Close; a live
                        // drop can emit Error then Close too. Report once.
                        if (!slotPtr->downReported.exchange(true)) {
                            slotPtr->openSeen = false;
                            enqueue({QueuedEvent::Kind::Disconnected, channel,
                                     msg->type == ix::WebSocketMessageType::Error
                                         ? msg->errorInfo.reason
                                         : std::string{},
                                     false});
                        }
                        break;
                    case ix::WebSocketMessageType::Message:
                        enqueue({QueuedEvent::Kind::Frame, channel, msg->str, msg->binary});
                        break;
                    default:
                        break;
                }
            });
        return {};
    }
};

Ws40Client::Ws40Client(ClientConfig config, IClock* clock, IRandom* random, ILogger* logger)
    : impl_(std::make_unique<Impl>()) {
    detail::ensureNetSystem();
    impl_->config = std::move(config);
    impl_->clock = clock;
    impl_->logger = logger;
    for (const Channel channel : impl_->config.channels) {
        Impl::ChannelSlot& slot = impl_->slot(channel);
        slot.wanted = true;
        slot.policy = std::make_unique<ReconnectPolicy>(impl_->config.reconnect, clock, random);
    }
}

Ws40Client::~Ws40Client() {
    stop();
}

Result<void> Ws40Client::start() {
    if (impl_->started) {
        return makeError(ErrorCode::WrongState, "client already started");
    }
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        const auto channel = static_cast<Channel>(i);
        if (!impl_->slot(channel).wanted) {
            continue;
        }
        Result<void> configured = impl_->configureSocket(channel);
        if (!configured.ok()) {
            return configured;
        }
        impl_->slot(channel).socket->start();
    }
    impl_->started = true;
    return {};
}

void Ws40Client::stop() {
    for (Impl::ChannelSlot& slot : impl_->slots) {
        if (slot.socket != nullptr) {
            slot.socket->stop();
        }
    }
    impl_->started = false;
}

Result<void> Ws40Client::send(Channel channel, std::span<const std::uint8_t> frame) {
    Impl::ChannelSlot& slot = impl_->slot(channel);
    if (slot.socket == nullptr || slot.socket->getReadyState() != ix::ReadyState::Open) {
        return makeError(
            ErrorCode::IoError,
            std::string{"ws40 "} + std::string{channelName(channel)} + " channel is not connected");
    }
    // One MOS message per WebSocket message (plan §5.1): the session hands
    // us exactly one serialized document per call, sent as one message.
    const std::string data{reinterpret_cast<const char*>(frame.data()), frame.size()};
    const ix::WebSocketSendInfo sent = detail::wantsTextFrames(impl_->config.wireEncoding)
                                           ? slot.socket->sendText(data)
                                           : slot.socket->sendBinary(data);
    if (!sent.success) {
        return makeError(ErrorCode::IoError, "WebSocket send failed");
    }
    return {};
}

bool Ws40Client::isConnected(Channel channel) const {
    const Impl::ChannelSlot& slot = impl_->slot(channel);
    return slot.socket != nullptr && slot.socket->getReadyState() == ix::ReadyState::Open;
}

void Ws40Client::pump(PeerSession& session) {
    std::deque<QueuedEvent> local;
    {
        const std::scoped_lock lock{impl_->queueMutex};
        local.swap(impl_->queue);
    }
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
                if (!event.payload.empty()) {
                    impl_->log(LogLevel::Debug, std::string{channelName(event.channel)} +
                                                    " channel down: " + event.payload);
                }
                if (slot.sessionConnected) {
                    slot.sessionConnected = false;
                    session.onChannelDisconnected(event.channel);
                }
                break;
            case QueuedEvent::Kind::Frame: {
                const bool expectBinary = !detail::wantsTextFrames(impl_->config.wireEncoding);
                if (event.binary != expectBinary) {
                    const std::string diagnostic = detail::frameTypeMismatchDiagnostic(
                        event.binary, impl_->config.wireEncoding);
                    impl_->log(LogLevel::Error, diagnostic);
                    if (impl_->errorHandler) {
                        impl_->errorHandler(event.channel, diagnostic);
                    }
                    break;  // never delivered: setup problem, not data
                }
                session.onFrameReceived(
                    event.channel, std::span<const std::uint8_t>{
                                       reinterpret_cast<const std::uint8_t*>(event.payload.data()),
                                       event.payload.size()});
                break;
            }
        }
    }

    // Reconnection: the policy says when; stop() joins the finished socket
    // thread, start() launches the next attempt.
    if (!impl_->started) {
        return;
    }
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        const auto channel = static_cast<Channel>(i);
        Impl::ChannelSlot& slot = impl_->slot(channel);
        if (!slot.wanted || slot.sessionConnected || slot.socket == nullptr) {
            continue;
        }
        if (slot.policy->shouldAttempt()) {
            slot.socket->stop();
            slot.downReported = false;
            slot.openSeen = false;
            slot.socket->start();
        }
    }
}

void Ws40Client::setErrorHandler(TransportErrorHandler handler) {
    impl_->errorHandler = std::move(handler);
}

std::optional<MonotonicTime> Ws40Client::nextDeadline() const {
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
