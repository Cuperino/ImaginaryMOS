// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/transports/ws40.h>

#include <array>
#include <deque>
#include <map>
#include <mutex>

#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>

#include <imos/ws40/url.h>

#include "ws40_common.h"

namespace imos::transports::ws40 {

namespace {

std::string peerKeyOf(std::string_view mosID, std::string_view ncsID) {
    return std::string{mosID} + "|" + std::string{ncsID};
}

}  // namespace

bool FixedCredentialVerifier::verify(std::string_view username,
                                     std::string_view password) noexcept {
    return imos::ws40::verifyCredentials(
        imos::ws40::BasicCredentials{std::string{username}, std::string{password}}, expected_);
}

struct Ws40Server::Impl {
    /// The per-peer send path: routes each channel to whichever validated
    /// connection currently claims it (plan §5.1 "hands the connection to
    /// the matching PeerSession's channel slot").
    class PeerTransport : public ITransport {
    public:
        PeerTransport(Impl* owner, std::string peerKey)
            : owner_(owner), peerKey_(std::move(peerKey)) {}

        Result<void> send(Channel channel, std::span<const std::uint8_t> frame) override {
            if (owner_ == nullptr) {
                return makeError(ErrorCode::NotFound, "peer is not registered");
            }
            std::shared_ptr<ix::WebSocket> socket;
            bool text = false;
            {
                const std::scoped_lock lock{owner_->mutex};
                auto it = owner_->peers.find(peerKey_);
                if (it == owner_->peers.end()) {
                    return makeError(ErrorCode::NotFound, "peer is not registered");
                }
                socket = it->second.claims[static_cast<std::size_t>(channel)].lock();
                text = detail::wantsTextFrames(it->second.config.wireEncoding);
            }
            if (socket == nullptr || socket->getReadyState() != ix::ReadyState::Open) {
                return makeError(ErrorCode::IoError, "no live connection claims the " +
                                                         std::string{channelName(channel)} +
                                                         " channel for this peer");
            }
            const std::string data{reinterpret_cast<const char*>(frame.data()), frame.size()};
            const ix::WebSocketSendInfo sent =
                text ? socket->sendText(data) : socket->sendBinary(data);
            if (!sent.success) {
                return makeError(ErrorCode::IoError, "WebSocket send failed");
            }
            return {};
        }

        [[nodiscard]] bool isConnected(Channel channel) const override {
            if (owner_ == nullptr) {
                return false;
            }
            const std::scoped_lock lock{owner_->mutex};
            const auto it = owner_->peers.find(peerKey_);
            if (it == owner_->peers.end()) {
                return false;
            }
            const std::shared_ptr<ix::WebSocket> socket =
                it->second.claims[static_cast<std::size_t>(channel)].lock();
            return socket != nullptr && socket->getReadyState() == ix::ReadyState::Open;
        }

    private:
        Impl* owner_;
        std::string peerKey_;
    };

    struct Peer {
        ServerPeerConfig config;
        std::unique_ptr<PeerTransport> transport;
        std::array<std::weak_ptr<ix::WebSocket>, kChannelCount> claims;
    };

    /// What a validated connection is bound to; carried by the per-socket
    /// message callback after a successful Open.
    struct Claim {
        std::string peerKey;
        std::string mosID;
        std::string ncsID;
        Channel channel = Channel::Mom;
        encoding::WireEncoding wireEncoding = encoding::WireEncoding::Ucs2Be;
    };

    ServerConfig config;
    ILogger* logger = nullptr;
    std::unique_ptr<ix::WebSocketServer> server;
    bool started = false;

    mutable std::mutex mutex;  // guards peers map, claims, and the queue
    std::map<std::string, Peer> peers;
    std::deque<ServerEvent> queue;

    void log(LogLevel level, const std::string& text) {
        if (logger != nullptr) {
            logger->log(level, text);
        }
    }

    void enqueue(ServerEvent event) {
        const std::scoped_lock lock{mutex};
        queue.push_back(std::move(event));
    }

    void reject(ix::WebSocket& socket, std::uint16_t code, const std::string& reason) {
        ServerEvent event;
        event.kind = ServerEvent::Kind::Rejected;
        event.diagnostic = reason;
        enqueue(std::move(event));
        log(LogLevel::Warning, "ws40 upgrade rejected: " + reason);
        socket.close(code, reason);
    }

    /// Validate an opened connection: credentials first (never reveal
    /// whether the peer/channel would have matched), then parameters, then
    /// the channel-slot claim. Returns the claim on success.
    std::optional<Claim> validateOpen(const std::shared_ptr<ix::WebSocket>& socket,
                                      const ix::WebSocketOpenInfo& openInfo) {
        if (config.credentials != nullptr) {
            const auto header = openInfo.headers.find("Authorization");
            if (header == openInfo.headers.end()) {
                reject(*socket, detail::kCloseUnauthorized,
                       "missing Authorization header on the upgrade");
                return std::nullopt;
            }
            Result<imos::ws40::BasicCredentials> presented =
                imos::ws40::parseBasicAuthorization(header->second);
            if (!presented.ok()) {
                reject(*socket, detail::kCloseUnauthorized,
                       "unparseable Authorization header: " + presented.error().message);
                return std::nullopt;
            }
            if (!config.credentials->verify(presented.value().username,
                                            presented.value().password)) {
                reject(*socket, detail::kCloseUnauthorized, "invalid credentials");
                return std::nullopt;
            }
        }

        Result<imos::ws40::ConnectionParams> params =
            imos::ws40::parseConnectionParams(openInfo.uri);
        if (!params.ok()) {
            reject(*socket, detail::kCloseBadParameters, params.error().message);
            return std::nullopt;
        }

        Claim claim;
        claim.mosID = params.value().mosID;
        claim.ncsID = params.value().ncsID;
        claim.channel = params.value().channel;
        claim.peerKey = peerKeyOf(claim.mosID, claim.ncsID);

        std::uint16_t rejectCode = 0;
        std::string rejectReason;
        {
            const std::scoped_lock lock{mutex};
            const auto it = peers.find(claim.peerKey);
            if (it == peers.end()) {
                // Deliberately terse: don't help a prober enumerate the
                // configured identities (threat model: spoofed peer).
                rejectCode = detail::kCloseUnknownPeer;
                rejectReason = "unknown mosID/ncsID pairing for channel " +
                               std::string{channelName(claim.channel)};
            } else {
                claim.wireEncoding = it->second.config.wireEncoding;
                std::weak_ptr<ix::WebSocket>& slot =
                    it->second.claims[static_cast<std::size_t>(claim.channel)];
                const std::shared_ptr<ix::WebSocket> existing = slot.lock();
                if (existing != nullptr && existing != socket &&
                    existing->getReadyState() == ix::ReadyState::Open) {
                    rejectCode = detail::kCloseDuplicateChannel;
                    rejectReason = "the " + std::string{channelName(claim.channel)} +
                                   " channel is already claimed for this peer";
                } else {
                    slot = socket;
                    ServerEvent event;
                    event.kind = ServerEvent::Kind::ChannelConnected;
                    event.mosID = claim.mosID;
                    event.ncsID = claim.ncsID;
                    event.channel = claim.channel;
                    queue.push_back(std::move(event));  // mutex already held
                }
            }
        }
        if (rejectCode != 0) {
            reject(*socket, rejectCode, rejectReason);
            return std::nullopt;
        }
        return claim;
    }

    void releaseClaim(const Claim& claim, const std::shared_ptr<ix::WebSocket>& socket) {
        const std::scoped_lock lock{mutex};
        const auto it = peers.find(claim.peerKey);
        if (it == peers.end()) {
            return;
        }
        std::weak_ptr<ix::WebSocket>& slot =
            it->second.claims[static_cast<std::size_t>(claim.channel)];
        if (slot.lock() != socket) {
            return;  // another connection took the slot over; not ours to drop
        }
        slot.reset();
        ServerEvent event;
        event.kind = ServerEvent::Kind::ChannelDisconnected;
        event.mosID = claim.mosID;
        event.ncsID = claim.ncsID;
        event.channel = claim.channel;
        queue.push_back(std::move(event));
    }

    void onFrame(const Claim& claim, const std::string& payload, bool binary) {
        ServerEvent event;
        event.mosID = claim.mosID;
        event.ncsID = claim.ncsID;
        event.channel = claim.channel;
        const bool expectBinary = !detail::wantsTextFrames(claim.wireEncoding);
        if (binary != expectBinary) {
            event.kind = ServerEvent::Kind::ProtocolError;
            event.diagnostic = detail::frameTypeMismatchDiagnostic(binary, claim.wireEncoding);
            log(LogLevel::Error, event.diagnostic);
        } else {
            event.kind = ServerEvent::Kind::Frame;
            event.frame.assign(payload.begin(), payload.end());
        }
        enqueue(std::move(event));
    }
};

Ws40Server::Ws40Server(ServerConfig config, ILogger* logger) : impl_(std::make_unique<Impl>()) {
    detail::ensureNetSystem();
    impl_->config = std::move(config);
    impl_->logger = logger;
}

Ws40Server::~Ws40Server() {
    stop();
}

Result<void> Ws40Server::addPeer(const ServerPeerConfig& peer) {
    if (peer.mosID.empty() || peer.ncsID.empty()) {
        return makeError(ErrorCode::InvalidArgument, "peer needs both mosID and ncsID");
    }
    const std::string key = peerKeyOf(peer.mosID, peer.ncsID);
    const std::scoped_lock lock{impl_->mutex};
    auto [it, inserted] = impl_->peers.try_emplace(key);
    if (!inserted) {
        return makeError(ErrorCode::InvalidArgument, "peer is already registered");
    }
    it->second.config = peer;
    it->second.transport = std::make_unique<Impl::PeerTransport>(impl_.get(), key);
    return {};
}

Result<void> Ws40Server::start() {
    if (impl_->started) {
        return makeError(ErrorCode::WrongState, "server already started");
    }
    // config.port == 0 asks for a kernel-assigned ephemeral port. IXWebSocket
    // can bind port 0 but keeps the kernel's choice private, so the port is
    // probed here (bind 0 → getsockname → close) and the listen retried:
    // another process can claim the probed port between the probe socket
    // closing and the listener binding it.
    const bool ephemeral = impl_->config.port == 0;
    const int maxAttempts = ephemeral ? 8 : 1;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        const int port = ephemeral ? ix::getFreePort() : impl_->config.port;
        if (port <= 0) {
            continue;  // probe failed outright; re-probe
        }
        if (listenOn(port)) {
            impl_->config.port = port;
            impl_->started = true;
            return {};
        }
    }
    return makeError(
        ErrorCode::IoError,
        "ws40 server failed to listen on " + impl_->config.host + ":" +
            (ephemeral ? std::string{"<ephemeral>"} : std::to_string(impl_->config.port)));
}

bool Ws40Server::listenOn(int port) {
    impl_->server = std::make_unique<ix::WebSocketServer>(
        port, impl_->config.host, ix::SocketServer::kDefaultTcpBacklog,
        impl_->config.maxConnections, impl_->config.handshakeTimeoutSecs);
    if (!impl_->config.enableDeflate) {
        impl_->server->disablePerMessageDeflate();
    }
    if (impl_->config.tls.enabled) {
        impl_->server->setTLSOptions(
            detail::makeSocketTlsOptions(impl_->config.tls, /*serverSide=*/true));
    }

    Impl* impl = impl_.get();
    impl_->server->setOnConnectionCallback([impl](std::weak_ptr<ix::WebSocket> weakSocket,
                                                  std::shared_ptr<ix::ConnectionState> /*state*/) {
        const std::shared_ptr<ix::WebSocket> socket = weakSocket.lock();
        if (socket == nullptr) {
            return;
        }
        // One shared Claim per connection, filled on Open. The callback
        // for a given socket runs on that socket's own thread, so no
        // lock is needed around the claim itself.
        auto claim = std::make_shared<std::optional<Impl::Claim>>();
        socket->setOnMessageCallback([impl, weakSocket, claim](const ix::WebSocketMessagePtr& msg) {
            const std::shared_ptr<ix::WebSocket> live = weakSocket.lock();
            if (live == nullptr) {
                return;
            }
            switch (msg->type) {
                case ix::WebSocketMessageType::Open:
                    *claim = impl->validateOpen(live, msg->openInfo);
                    break;
                case ix::WebSocketMessageType::Message:
                    if (claim->has_value()) {
                        impl->onFrame(**claim, msg->str, msg->binary);
                    }
                    break;
                case ix::WebSocketMessageType::Close:
                case ix::WebSocketMessageType::Error:
                    if (claim->has_value()) {
                        impl->releaseClaim(**claim, live);
                        claim->reset();
                    }
                    break;
                default:
                    break;
            }
        });
    });

    if (const auto listening = impl_->server->listenAndStart(); !listening) {
        impl_->server.reset();
        return false;
    }
    return true;
}

void Ws40Server::stop() {
    if (impl_->server != nullptr) {
        impl_->server->stop();
        impl_->server.reset();
    }
    impl_->started = false;
}

int Ws40Server::port() const {
    return impl_->server != nullptr ? impl_->server->getPort() : impl_->config.port;
}

ITransport& Ws40Server::transportFor(std::string_view mosID, std::string_view ncsID) {
    const std::scoped_lock lock{impl_->mutex};
    const auto it = impl_->peers.find(peerKeyOf(mosID, ncsID));
    // addPeer precedes transportFor by contract; a miss is a programming
    // error surfaced loudly rather than UB on a dangling reference.
    if (it == impl_->peers.end()) {
        static Impl::PeerTransport unregistered{nullptr, {}};
        impl_->log(LogLevel::Error, "transportFor() called for an unregistered peer");
        return unregistered;
    }
    return *it->second.transport;
}

std::optional<ServerEvent> Ws40Server::nextEvent() {
    const std::scoped_lock lock{impl_->mutex};
    if (impl_->queue.empty()) {
        return std::nullopt;
    }
    ServerEvent event = std::move(impl_->queue.front());
    impl_->queue.pop_front();
    return event;
}

}  // namespace imos::transports::ws40
