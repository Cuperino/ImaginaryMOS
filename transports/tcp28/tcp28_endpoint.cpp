// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/transports/tcp28.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <imos/framing/deframer.h>

#include "tcp28_socket.h"

namespace imos::transports::tcp28 {

namespace {

struct QueuedEvent {
    enum class Kind : std::uint8_t { Connected, Disconnected, Frame, Diagnostic };
    Kind kind = Kind::Frame;
    Channel channel = Channel::Mom;
    std::vector<std::uint8_t> frame;
    std::string diagnostic;
};

struct Command {
    enum class Kind : std::uint8_t { Send, Dial, Stop };
    Kind kind = Kind::Send;
    Channel channel = Channel::Mom;
    std::vector<std::uint8_t> bytes;
};

/// One live TCP connection: its deframer state and any bytes the socket
/// has not yet accepted for writing.
struct Connection {
    detail::SocketHandle fd = detail::kInvalidSocket;
    std::uint64_t id = 0;
    framing::StreamDeframer deframer;
    std::size_t overflowsSeen = 0;
    std::vector<std::uint8_t> writeBuffer;

    explicit Connection(detail::SocketHandle handle, std::uint64_t connId,
                        const framing::DeframerOptions& options)
        : fd(handle), id(connId), deframer(options) {}
};

constexpr int kPollTimeoutMillis = 250;
constexpr int kStopDrainMillis = 250;
constexpr std::size_t kReadChunk = 16 * 1024;

}  // namespace

struct Tcp28Endpoint::Impl {
    EndpointConfig config;
    IClock* clock = nullptr;
    IRandom* random = nullptr;
    ILogger* logger = nullptr;
    Tcp28ErrorHandler errorHandler;

    /// Everything the I/O thread owns for one channel. Only the atomics are
    /// read from other threads.
    struct ChannelState {
        bool enabled = false;
        bool dials = false;
        int boundPort = 0;  ///< listening channels; resolved before the thread starts

        detail::SocketHandle listenFd = detail::kInvalidSocket;
        std::vector<std::unique_ptr<Connection>> connections;  ///< accepted, or the one dialed
        std::uint64_t replyConnId = 0;  ///< the connection the last document arrived on

        detail::SocketHandle dialFd = detail::kInvalidSocket;  ///< connect in progress
        bool dialInProgress = false;

        std::atomic<bool> connected{false};

        // Pump-thread state (never touched by the I/O thread).
        std::unique_ptr<ReconnectPolicy> policy;
        bool sessionConnected = false;
    };
    std::array<ChannelState, kChannelCount> channels;

    std::thread ioThread;
    std::atomic<bool> running{false};
    bool started = false;

    detail::WakePipe wake;
    std::mutex commandMutex;
    std::deque<Command> commands;
    std::mutex eventMutex;
    std::deque<QueuedEvent> events;

    std::uint64_t nextConnId = 1;

    ChannelState& channel(Channel c) { return channels[static_cast<std::size_t>(c)]; }
    const ChannelState& channel(Channel c) const { return channels[static_cast<std::size_t>(c)]; }

    void log(LogLevel level, const std::string& text) {
        if (logger != nullptr) {
            logger->log(level, text);
        }
    }

    void postEvent(QueuedEvent event) {
        const std::scoped_lock lock{eventMutex};
        events.push_back(std::move(event));
    }

    void postCommand(Command command) {
        {
            const std::scoped_lock lock{commandMutex};
            commands.push_back(std::move(command));
        }
        wake.wake();
    }

    framing::DeframerOptions deframerOptions() const {
        framing::DeframerOptions options;
        options.encoding = config.wireEncoding;
        options.maxMessageBytes = config.maxMessageBytes;
        return options;
    }

    // ---- I/O thread -----------------------------------------------------

    void ioLoop() {
        bool stopping = false;
        std::chrono::steady_clock::time_point drainDeadline{};
        while (true) {
            if (stopping &&
                (allConnectionsGone() || std::chrono::steady_clock::now() >= drainDeadline)) {
                break;
            }
            pollOnce(stopping);
            if (!stopping) {
                std::deque<Command> local;
                {
                    const std::scoped_lock lock{commandMutex};
                    local.swap(commands);
                }
                for (Command& command : local) {
                    if (command.kind == Command::Kind::Stop) {
                        stopping = true;
                        beginShutdown();
                        drainDeadline = std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds{kStopDrainMillis};
                    } else if (command.kind == Command::Kind::Dial) {
                        startDial(command.channel);
                    } else {
                        queueSend(command.channel, std::move(command.bytes));
                    }
                }
            }
        }
        closeEverything();
    }

    bool allConnectionsGone() const {
        for (const ChannelState& state : channels) {
            if (!state.connections.empty()) {
                return false;
            }
        }
        return true;
    }

    /// Four-way close, step one: half-close every connection and stop
    /// accepting/dialing; the loop then drains until EOF or the grace
    /// deadline (2.8.5's FIN_WAIT_2/CLOSE_WAIT admonition).
    void beginShutdown() {
        for (ChannelState& state : channels) {
            if (state.listenFd != detail::kInvalidSocket) {
                detail::closeSocket(state.listenFd);
                state.listenFd = detail::kInvalidSocket;
            }
            if (state.dialFd != detail::kInvalidSocket) {
                detail::closeSocket(state.dialFd);
                state.dialFd = detail::kInvalidSocket;
                state.dialInProgress = false;
            }
            for (const std::unique_ptr<Connection>& connection : state.connections) {
                detail::shutdownWrite(connection->fd);
            }
        }
    }

    void closeEverything() {
        for (ChannelState& state : channels) {
            if (state.listenFd != detail::kInvalidSocket) {
                detail::closeSocket(state.listenFd);
                state.listenFd = detail::kInvalidSocket;
            }
            if (state.dialFd != detail::kInvalidSocket) {
                detail::closeSocket(state.dialFd);
                state.dialFd = detail::kInvalidSocket;
            }
            for (const std::unique_ptr<Connection>& connection : state.connections) {
                detail::closeSocket(connection->fd);
            }
            state.connections.clear();
            state.connected = false;
        }
    }

    void pollOnce(bool stopping) {
        struct Slot {
            Channel channel;
            enum class What : std::uint8_t { Wake, Listen, Dial, Conn } what;
            std::size_t connIndex = 0;
        };
        std::vector<detail::PollFd> fds;
        std::vector<Slot> slots;
        fds.push_back({wake.readHandle(), POLLIN, 0});
        slots.push_back({Channel::Mom, Slot::What::Wake, 0});
        for (std::size_t i = 0; i < kChannelCount; ++i) {
            const auto channelId = static_cast<Channel>(i);
            ChannelState& state = channels[i];
            if (!state.enabled) {
                continue;
            }
            if (state.listenFd != detail::kInvalidSocket) {
                fds.push_back({state.listenFd, POLLIN, 0});
                slots.push_back({channelId, Slot::What::Listen, 0});
            }
            if (state.dialInProgress) {
                fds.push_back({state.dialFd, POLLOUT, 0});
                slots.push_back({channelId, Slot::What::Dial, 0});
            }
            for (std::size_t c = 0; c < state.connections.size(); ++c) {
                short interest = POLLIN;
                if (!state.connections[c]->writeBuffer.empty()) {
                    interest |= POLLOUT;
                }
                fds.push_back({state.connections[c]->fd, interest, 0});
                slots.push_back({channelId, Slot::What::Conn, c});
            }
        }
        const int ready = detail::pollSockets(fds, stopping ? 20 : kPollTimeoutMillis);
        if (ready <= 0) {
            return;
        }
        // Indices shift when connections close mid-sweep; act on collected
        // handles instead of re-walking state, and tolerate stale entries.
        for (std::size_t i = 0; i < fds.size(); ++i) {
            if (fds[i].revents == 0) {
                continue;
            }
            const Slot& slot = slots[i];
            switch (slot.what) {
                case Slot::What::Wake:
                    wake.drain();
                    break;
                case Slot::What::Listen:
                    acceptPending(slot.channel);
                    break;
                case Slot::What::Dial:
                    finishDial(slot.channel);
                    break;
                case Slot::What::Conn:
                    serviceConnection(slot.channel, fds[i].fd, (fds[i].revents & POLLOUT) != 0,
                                      stopping);
                    break;
            }
        }
    }

    void startDial(Channel channelId) {
        ChannelState& state = channel(channelId);
        if (state.dialInProgress || !state.connections.empty()) {
            return;
        }
        const int port = portFor(channelId);
        Result<detail::SocketHandle> attempt = detail::beginConnect(config.peerHost, port);
        if (!attempt.ok()) {
            postEvent({QueuedEvent::Kind::Disconnected, channelId, {}, attempt.error().message});
            return;
        }
        state.dialFd = attempt.value();
        state.dialInProgress = true;
    }

    void finishDial(Channel channelId) {
        ChannelState& state = channel(channelId);
        if (!state.dialInProgress) {
            return;
        }
        std::string error;
        const int outcome = detail::connectFinished(state.dialFd, error);
        if (outcome == 0) {
            return;
        }
        state.dialInProgress = false;
        if (outcome < 0) {
            detail::closeSocket(state.dialFd);
            state.dialFd = detail::kInvalidSocket;
            postEvent({QueuedEvent::Kind::Disconnected,
                       channelId,
                       {},
                       "connect to " + config.peerHost + ":" + std::to_string(portFor(channelId)) +
                           " failed: " + error});
            return;
        }
        state.connections.push_back(
            std::make_unique<Connection>(state.dialFd, nextConnId++, deframerOptions()));
        state.dialFd = detail::kInvalidSocket;
        state.replyConnId = state.connections.back()->id;
        state.connected = true;
        postEvent({QueuedEvent::Kind::Connected, channelId, {}, {}});
    }

    void acceptPending(Channel channelId) {
        ChannelState& state = channel(channelId);
        while (state.listenFd != detail::kInvalidSocket) {
            const detail::SocketHandle accepted = detail::acceptOne(state.listenFd);
            if (accepted == detail::kInvalidSocket) {
                return;
            }
            if (state.connections.size() >= config.maxConnectionsPerListener) {
                // Bound resource use (plan §12); the spec only requires
                // tolerating a handful of sockets per port.
                detail::closeSocket(accepted);
                postEvent({QueuedEvent::Kind::Diagnostic,
                           channelId,
                           {},
                           "listener connection cap reached; rejecting extra connection"});
                continue;
            }
            const bool first = state.connections.empty();
            state.connections.push_back(
                std::make_unique<Connection>(accepted, nextConnId++, deframerOptions()));
            if (first) {
                state.connected = true;
                postEvent({QueuedEvent::Kind::Connected, channelId, {}, {}});
            }
        }
    }

    void serviceConnection(Channel channelId, detail::SocketHandle fd, bool writable,
                           bool stopping) {
        ChannelState& state = channel(channelId);
        const auto it = std::find_if(
            state.connections.begin(), state.connections.end(),
            [fd](const std::unique_ptr<Connection>& connection) { return connection->fd == fd; });
        if (it == state.connections.end()) {
            return;  // closed earlier in this sweep
        }
        Connection& connection = **it;

        if (writable && !connection.writeBuffer.empty()) {
            const std::int64_t written = detail::writeSome(connection.fd, connection.writeBuffer);
            if (written > 0) {
                connection.writeBuffer.erase(
                    connection.writeBuffer.begin(),
                    connection.writeBuffer.begin() + static_cast<std::ptrdiff_t>(written));
            } else if (written == -2) {
                dropConnection(channelId, connection.id,
                               "write failed: " + detail::lastErrorText());
                return;
            }
        }

        std::array<std::uint8_t, kReadChunk> buffer;
        while (true) {
            const std::int64_t got = detail::readSome(connection.fd, buffer);
            if (got > 0) {
                connection.deframer.feed(
                    std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(got)});
                drainDeframer(channelId, connection, stopping);
                continue;
            }
            if (got == -1) {
                return;  // would block; stay subscribed
            }
            // Orderly EOF or hard error: either way this connection is done.
            dropConnection(channelId, connection.id,
                           got == 0 ? std::string{} : "read failed: " + detail::lastErrorText());
            return;
        }
    }

    void drainDeframer(Channel channelId, Connection& connection, bool stopping) {
        while (true) {
            std::optional<framing::Frame> frame = connection.deframer.next();
            if (!frame.has_value()) {
                break;
            }
            if (frame->discardedBefore > 0) {
                postEvent({QueuedEvent::Kind::Diagnostic,
                           channelId,
                           {},
                           "deframer resynchronized past " +
                               std::to_string(frame->discardedBefore) + " garbage bytes"});
            }
            channel(channelId).replyConnId = connection.id;
            if (!stopping) {
                postEvent({QueuedEvent::Kind::Frame, channelId, std::move(frame->document), {}});
            }
        }
        if (connection.deframer.overflowCount() > connection.overflowsSeen) {
            connection.overflowsSeen = connection.deframer.overflowCount();
            postEvent({QueuedEvent::Kind::Diagnostic,
                       channelId,
                       {},
                       "message over the size cap discarded (plan §12)"});
        }
    }

    void dropConnection(Channel channelId, std::uint64_t connId, const std::string& reason) {
        ChannelState& state = channel(channelId);
        const auto it = std::find_if(state.connections.begin(), state.connections.end(),
                                     [connId](const std::unique_ptr<Connection>& connection) {
                                         return connection->id == connId;
                                     });
        if (it == state.connections.end()) {
            return;
        }
        detail::closeSocket((*it)->fd);
        state.connections.erase(it);
        if (state.connections.empty()) {
            state.connected = false;
            postEvent({QueuedEvent::Kind::Disconnected, channelId, {}, reason});
        }
    }

    void queueSend(Channel channelId, std::vector<std::uint8_t> bytes) {
        ChannelState& state = channel(channelId);
        Connection* target = nullptr;
        // Reply on the socket the last message arrived on (2.8.5 rule);
        // otherwise the newest live connection (a dialed channel has one).
        for (const std::unique_ptr<Connection>& connection : state.connections) {
            if (connection->id == state.replyConnId) {
                target = connection.get();
                break;
            }
        }
        if (target == nullptr && !state.connections.empty()) {
            target = state.connections.back().get();
        }
        if (target == nullptr) {
            postEvent({QueuedEvent::Kind::Diagnostic,
                       channelId,
                       {},
                       "send dropped: channel has no live connection"});
            return;
        }
        const bool wasEmpty = target->writeBuffer.empty();
        target->writeBuffer.insert(target->writeBuffer.end(), bytes.begin(), bytes.end());
        if (wasEmpty) {
            // Try immediately; whatever the socket refuses stays buffered
            // and rides the POLLOUT subscription.
            const std::int64_t written = detail::writeSome(target->fd, target->writeBuffer);
            if (written > 0) {
                target->writeBuffer.erase(
                    target->writeBuffer.begin(),
                    target->writeBuffer.begin() + static_cast<std::ptrdiff_t>(written));
            } else if (written == -2) {
                dropConnection(channelId, target->id, "write failed: " + detail::lastErrorText());
            }
        }
    }

    int portFor(Channel channelId) const {
        switch (channelId) {
            case Channel::Mom:
                return config.ports.mom;
            case Channel::Ro:
                return config.ports.ro;
            case Channel::Aux:
                return config.ports.aux;
        }
        return 0;
    }
};

bool Tcp28Endpoint::dialsChannel(Role role, Channel channel) noexcept {
    // 2.8.5 "Message Transport": the NCS accepts connections on the lower
    // port (mom), the MOS accepts on the upper port (ro); the query port
    // (aux) carries NCS-initiated searches, so the MOS listens there too.
    const bool deviceDials = channel == Channel::Mom;
    return role == Role::Device ? deviceDials : !deviceDials;
}

Tcp28Endpoint::Tcp28Endpoint(EndpointConfig config, IClock* clock, IRandom* random, ILogger* logger)
    : impl_(std::make_unique<Impl>()) {
    detail::ensureNetSystem();
    impl_->config = std::move(config);
    impl_->clock = clock;
    impl_->random = random;
    impl_->logger = logger;
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        const auto channelId = static_cast<Channel>(i);
        Impl::ChannelState& state = impl_->channels[i];
        state.enabled = channelId != Channel::Aux || impl_->config.enableAux;
        state.dials = dialsChannel(impl_->config.role, channelId);
        if (state.enabled && state.dials) {
            state.policy =
                std::make_unique<ReconnectPolicy>(impl_->config.reconnect, clock, random);
        }
    }
}

Tcp28Endpoint::~Tcp28Endpoint() {
    stop();
}

Result<void> Tcp28Endpoint::start() {
    if (impl_->started) {
        return makeError(ErrorCode::WrongState, "endpoint already started");
    }
    if (!impl_->wake.ok()) {
        return makeError(ErrorCode::IoError, "cannot create I/O wake pipe");
    }
    // Bind listeners synchronously so configuration errors surface here,
    // before any thread exists.
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        Impl::ChannelState& state = impl_->channels[i];
        if (!state.enabled || state.dials) {
            continue;
        }
        int boundPort =
            state.boundPort != 0 ? state.boundPort : impl_->portFor(static_cast<Channel>(i));
        Result<detail::SocketHandle> listener =
            detail::listenOn(impl_->config.bindAddress, boundPort, boundPort);
        if (!listener.ok()) {
            for (Impl::ChannelState& opened : impl_->channels) {
                if (opened.listenFd != detail::kInvalidSocket) {
                    detail::closeSocket(opened.listenFd);
                    opened.listenFd = detail::kInvalidSocket;
                }
            }
            return std::move(listener).error();
        }
        state.listenFd = listener.value();
        state.boundPort = boundPort;  // sticks across stop()/start()
    }
    impl_->running = true;
    impl_->ioThread = std::thread{[impl = impl_.get()] { impl->ioLoop(); }};
    impl_->started = true;
    // Dial channels start connecting immediately; failures feed the
    // reconnect policy through pump().
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        const Impl::ChannelState& state = impl_->channels[i];
        if (state.enabled && state.dials) {
            impl_->postCommand({Command::Kind::Dial, static_cast<Channel>(i), {}});
        }
    }
    return {};
}

void Tcp28Endpoint::stop() {
    if (!impl_->started) {
        return;
    }
    impl_->postCommand({Command::Kind::Stop, Channel::Mom, {}});
    if (impl_->ioThread.joinable()) {
        impl_->ioThread.join();
    }
    impl_->running = false;
    impl_->started = false;
}

Result<void> Tcp28Endpoint::send(Channel channel, std::span<const std::uint8_t> frame) {
    const Impl::ChannelState& state = impl_->channel(channel);
    if (!state.enabled) {
        return makeError(
            ErrorCode::InvalidArgument,
            std::string{"tcp28 "} + std::string{channelName(channel)} + " channel is not enabled");
    }
    if (!state.connected.load()) {
        return makeError(ErrorCode::IoError, std::string{"tcp28 "} +
                                                 std::string{channelName(channel)} +
                                                 " channel is not connected");
    }
    impl_->postCommand(
        {Command::Kind::Send, channel, std::vector<std::uint8_t>{frame.begin(), frame.end()}});
    return {};
}

bool Tcp28Endpoint::isConnected(Channel channel) const {
    return impl_->channel(channel).connected.load();
}

void Tcp28Endpoint::pump(PeerSession& session) {
    std::deque<QueuedEvent> local;
    {
        const std::scoped_lock lock{impl_->eventMutex};
        local.swap(impl_->events);
    }
    for (QueuedEvent& event : local) {
        Impl::ChannelState& state = impl_->channel(event.channel);
        switch (event.kind) {
            case QueuedEvent::Kind::Connected:
                if (state.policy != nullptr) {
                    state.policy->onConnected();
                }
                state.sessionConnected = true;
                session.onChannelConnected(event.channel);
                break;
            case QueuedEvent::Kind::Disconnected:
                if (state.policy != nullptr) {
                    state.policy->onDisconnected();
                }
                if (!event.diagnostic.empty()) {
                    impl_->log(LogLevel::Debug, std::string{channelName(event.channel)} +
                                                    " channel down: " + event.diagnostic);
                }
                if (state.sessionConnected) {
                    state.sessionConnected = false;
                    session.onChannelDisconnected(event.channel);
                }
                break;
            case QueuedEvent::Kind::Frame:
                session.onFrameReceived(event.channel, event.frame);
                break;
            case QueuedEvent::Kind::Diagnostic:
                impl_->log(LogLevel::Warning, event.diagnostic);
                if (impl_->errorHandler) {
                    impl_->errorHandler(event.channel, event.diagnostic);
                }
                break;
        }
    }

    if (!impl_->started) {
        return;
    }
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        Impl::ChannelState& state = impl_->channels[i];
        if (!state.enabled || !state.dials || state.sessionConnected || state.policy == nullptr) {
            continue;
        }
        if (state.policy->shouldAttempt()) {
            impl_->postCommand({Command::Kind::Dial, static_cast<Channel>(i), {}});
        }
    }
}

void Tcp28Endpoint::setErrorHandler(Tcp28ErrorHandler handler) {
    impl_->errorHandler = std::move(handler);
}

std::optional<int> Tcp28Endpoint::listenPort(Channel channel) const {
    const Impl::ChannelState& state = impl_->channel(channel);
    if (!state.enabled || state.dials || state.boundPort == 0) {
        return std::nullopt;
    }
    return state.boundPort;
}

std::optional<MonotonicTime> Tcp28Endpoint::nextDeadline() const {
    std::optional<MonotonicTime> earliest;
    for (const Impl::ChannelState& state : impl_->channels) {
        if (state.policy == nullptr) {
            continue;
        }
        const std::optional<MonotonicTime> at = state.policy->nextAttemptAt();
        if (at.has_value() && (!earliest.has_value() || *at < *earliest)) {
            earliest = at;
        }
    }
    return earliest;
}

}  // namespace imos::transports::tcp28
