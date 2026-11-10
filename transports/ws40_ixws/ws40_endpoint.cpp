// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/transports/ws40.h>

#include <string>

namespace imos::transports::ws40 {

namespace {

Error notConnected(Channel channel) {
    return makeError(ErrorCode::IoError, std::string{"ws40 "} + std::string{channelName(channel)} +
                                             " channel is not connected");
}

}  // namespace

Ws40Endpoint::Ws40Endpoint(Ws40Client* dialed, ITransport* accepted)
    : dialed_(dialed), accepted_(accepted) {}

Result<void> Ws40Endpoint::send(Channel channel, std::span<const std::uint8_t> frame) {
    // A message this side originates rides the connection this side opened.
    if (dialed_ != nullptr && dialed_->isConnected(channel)) {
        return dialed_->send(channel, frame);
    }
    // No dialed connection: the accepted side takes it only when the peer
    // connected with passive=true, and says why when it did not.
    if (accepted_ != nullptr && accepted_->isConnected(channel)) {
        return accepted_->send(channel, frame);
    }
    return notConnected(channel);
}

Result<void> Ws40Endpoint::sendReply(Channel channel, std::span<const std::uint8_t> frame) {
    // A response returns on the connection the request arrived on. The peer
    // originates on the connections it opened to us — unless we are the
    // passive-mode dialer, in which case it originates on ours.
    if (dialed_ != nullptr && (dialed_->passive() || accepted_ == nullptr)) {
        return dialed_->sendReply(channel, frame);
    }
    if (accepted_ != nullptr) {
        return accepted_->sendReply(channel, frame);
    }
    return notConnected(channel);
}

bool Ws40Endpoint::isConnected(Channel channel) const {
    return (dialed_ != nullptr && dialed_->isConnected(channel)) ||
           (accepted_ != nullptr && accepted_->isConnected(channel));
}

void Ws40Endpoint::linkChanged(Channel channel, bool dialedSide, bool up, PeerSession& session) {
    const auto index = static_cast<std::size_t>(channel);
    const bool before = dialedUp_[index] || acceptedUp_[index];
    (dialedSide ? dialedUp_ : acceptedUp_)[index] = up;
    const bool after = dialedUp_[index] || acceptedUp_[index];
    // The session sees one channel: up while either connection is.
    if (!before && after) {
        session.onChannelConnected(channel);
    } else if (before && !after) {
        session.onChannelDisconnected(channel);
    }
}

void Ws40Endpoint::pump(PeerSession& session) {
    if (dialed_ == nullptr) {
        return;
    }
    ChannelEventSink sink;
    sink.connected = [this, &session](Channel channel) {
        linkChanged(channel, /*dialedSide=*/true, /*up=*/true, session);
    };
    sink.disconnected = [this, &session](Channel channel) {
        linkChanged(channel, /*dialedSide=*/true, /*up=*/false, session);
    };
    sink.frame = [&session](Channel channel, std::span<const std::uint8_t> frame) {
        session.onFrameReceived(channel, frame);
    };
    dialed_->pump(sink);
}

bool Ws40Endpoint::onServerEvent(const ServerEvent& event, PeerSession& session) {
    switch (event.kind) {
        case ServerEvent::Kind::ChannelConnected:
            linkChanged(event.channel, /*dialedSide=*/false, /*up=*/true, session);
            return true;
        case ServerEvent::Kind::ChannelDisconnected:
            linkChanged(event.channel, /*dialedSide=*/false, /*up=*/false, session);
            return true;
        case ServerEvent::Kind::Frame:
            session.onFrameReceived(event.channel, event.frame);
            return true;
        case ServerEvent::Kind::ProtocolError:
        case ServerEvent::Kind::Rejected:
            break;
    }
    return false;
}

std::optional<MonotonicTime> Ws40Endpoint::nextDeadline() const {
    return dialed_ != nullptr ? dialed_->nextDeadline() : std::nullopt;
}

}  // namespace imos::transports::ws40
