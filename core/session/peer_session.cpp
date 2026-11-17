// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/session/peer_session.h>

#include <algorithm>
#include <charconv>
#include <string>
#include <utility>

#include <imos/codec/legacy.h>
#include <imos/framing/deframer.h>
#include <imos/time_format.h>

namespace imos {

namespace {

constexpr std::int64_t kKeepAliveFloorMillis = 30'000;  // spec §4.1.1

bool typeIsAmong(codec::MessageType type, std::span<const codec::MessageType> set) {
    return std::find(set.begin(), set.end(), type) != set.end();
}

/// Inbound duplicate suppression (spec §4.1.6) applies to exactly the
/// peer-initiated requests whose answer is a bare acknowledgement: those are
/// the state-changing commands a blind retry must not apply twice. Two
/// families stay out of the window:
///  - responses (mosAck/roAck, list replies): they echo *our* messageIDs,
///    which share no number space with the peer's requests — recording one
///    would later suppress an unrelated peer request carrying that number;
///  - information requests (roReq, mosReqObj, reqMachInfo, heartbeat, …):
///    the spec calls dedupe "not relevant" for them, and their rich reply
///    cannot be regenerated here, so a retry is simply answered again.
bool participatesInDedupe(const codec::MessageInfo& info) {
    if (info.responses.empty()) {
        return false;
    }
    const codec::MessageType primary = info.responses.front();
    return primary == codec::MessageType::MosAck || primary == codec::MessageType::RoAck;
}

constexpr std::size_t kLateHeartbeatReplyCap = 64;
constexpr std::size_t kSentAckCap = 256;
/// messageIDs are reserved in the store a block at a time, so the counter
/// costs one durable write per block rather than one per message; a restart
/// resumes above the block (skipping its unused tail keeps IDs unique).
constexpr std::uint64_t kMessageIdBlock = 32;
/// Spec §4.1.6: a 32-bit signed integer >= 1 that "wraps to 1 when the limit
/// of the data type is reached".
constexpr std::uint64_t kMessageIdMax = 0x7FFFFFFF;

}  // namespace

PeerSession::PeerSession(SessionConfig config, Dependencies deps)
    : config_(std::move(config)), deps_(deps) {
    if (config_.peerKey.empty()) {
        config_.peerKey = config_.mosID + "|" + config_.ncsID;
    }
    if (config_.keepAliveIntervalMillis != 0 &&
        config_.keepAliveIntervalMillis < kKeepAliveFloorMillis) {
        config_.keepAliveIntervalMillis = kKeepAliveFloorMillis;
    }
    // Resume numbering above everything a previous run may have used.
    if (Result<std::uint64_t> last = deps_.store->lastMessageId(config_.peerKey); last.ok()) {
        reservedMessageIds_ = last.value();
        if (reservedMessageIds_ >= nextMessageId_) {
            nextMessageId_ = reservedMessageIds_ + 1;
        }
    }
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        const auto channel = static_cast<Channel>(i);
        if (ensureStoreOpen(channel)) {
            loadPersistedQueue(channel);  // crash resume (plan §3.3 durability)
        }
    }
}

std::uint64_t PeerSession::takeMessageId() {
    if (nextMessageId_ > kMessageIdMax) {
        nextMessageId_ = 1;
        reservedMessageIds_ = 0;
    }
    if (nextMessageId_ > reservedMessageIds_) {
        reservedMessageIds_ = std::min(nextMessageId_ + kMessageIdBlock - 1, kMessageIdMax);
        if (Result<void> noted = deps_.store->noteMessageId(config_.peerKey, reservedMessageIds_);
            !noted.ok()) {
            logMessage(LogLevel::Warning,
                       "messageID counter not persisted: " + noted.error().message);
        }
    }
    return nextMessageId_++;
}

PeerSession::ChannelState& PeerSession::state(Channel channel) {
    return channels_[static_cast<std::size_t>(channel)];
}

const PeerSession::ChannelState& PeerSession::state(Channel channel) const {
    return channels_[static_cast<std::size_t>(channel)];
}

void PeerSession::logMessage(LogLevel level, std::string_view text) {
    if (deps_.logger != nullptr) {
        deps_.logger->log(level, text);
    }
}

void PeerSession::pushEvent(SessionEvent event) {
    events_.push_back(std::move(event));
}

bool PeerSession::ensureStoreOpen(Channel channel) {
    ChannelState& ch = state(channel);
    if (ch.storeOpen) {
        return true;
    }
    Result<StreamHandle> opened = deps_.store->open(config_.peerKey, channel);
    if (!opened.ok()) {
        SessionEvent event;
        event.type = SessionEventType::StoreError;
        event.channel = channel;
        event.diagnostic = opened.error().message;
        pushEvent(std::move(event));
        logMessage(LogLevel::Error, "store open failed: " + opened.error().message);
        return false;
    }
    ch.stream = opened.value();
    ch.storeOpen = true;
    return true;
}

void PeerSession::loadPersistedQueue(Channel channel) {
    ChannelState& ch = state(channel);
    Result<PendingCursor> cursor = deps_.store->pending(ch.stream);
    if (!cursor.ok()) {
        SessionEvent event;
        event.type = SessionEventType::StoreError;
        event.channel = channel;
        event.diagnostic = cursor.error().message;
        pushEvent(std::move(event));
        return;
    }
    while (std::optional<PendingRecord> record = cursor.value().next()) {
        if ((record->flags & kOutboundReplyFlag) != 0) {
            // It answered a request of an earlier session's: that exchange
            // is gone, and the peer asks again. Sent now it would be a reply
            // to nothing - and a transport that carries replies only as the
            // answer to a call (MOS 3.x) could not send it at all.
            logMessage(LogLevel::Debug,
                       "dropping a persisted reply to messageID " + record->messageId);
            (void)deps_.store->markAcked(ch.stream, record->seq);
            continue;
        }
        PendingSend send;
        send.seq = record->seq;
        send.messageId = std::move(record->messageId);
        send.payloadUtf8.assign(record->payload.begin(), record->payload.end());

        Result<codec::Message> parsed = codec::parseMessage(send.payloadUtf8, config_.parseLimits);
        if (!parsed.ok()) {
            logMessage(LogLevel::Warning,
                       "dropping unreadable persisted record: " + parsed.error().message);
            (void)deps_.store->markAcked(ch.stream, send.seq);
            continue;
        }
        send.type = parsed.value().type();
        send.awaitReply = !parsed.value().info().responses.empty();

        Result<std::vector<std::uint8_t>> wire =
            encoding::encodeFromUtf8(send.payloadUtf8, config_.wireEncode);
        if (!wire.ok()) {
            logMessage(LogLevel::Warning,
                       "dropping unencodable persisted record: " + wire.error().message);
            (void)deps_.store->markAcked(ch.stream, send.seq);
            continue;
        }
        send.wire = std::move(wire).value();
        ch.queue.push_back(std::move(send));
    }
    // Replayed records may outrun the fresh messageID counter; stay above
    // any numeric ID we just reloaded so retries remain distinguishable.
    for (const PendingSend& send : ch.queue) {
        const char* const first = send.messageId.data();
        const char* const last = first + send.messageId.size();
        std::uint64_t numeric = 0;
        const auto [ptr, ec] = std::from_chars(first, last, numeric);
        if (ec == std::errc{} && ptr == last && numeric >= nextMessageId_) {
            nextMessageId_ = numeric + 1;
        }
    }
}

void PeerSession::applyEnvelopeDefaults(codec::Message& message) {
    const codec::MessageInfo& info = message.info();
    if (info.envelope == EnvelopeKind::None) {
        return;
    }
    if (!message.mosID().has_value() || message.mosID()->empty()) {
        message.setMosID(config_.mosID);
    }
    if (!message.ncsID().has_value() || message.ncsID()->empty()) {
        message.setNcsID(config_.ncsID);
    }
    if (info.envelope == EnvelopeKind::Full &&
        (!message.messageID().has_value() || message.messageID()->empty())) {
        message.setMessageID(std::to_string(takeMessageId()));
    }
}

Result<Seq> PeerSession::enqueue(codec::Message message) {
    const codec::MessageInfo& info = message.info();
    const std::optional<Channel> channel = channelFor(info.channel);
    if (!channel.has_value()) {
        if (info.channel == ChannelAffinity::AnyChannel) {
            return makeError(ErrorCode::InvalidArgument,
                             std::string{info.wireName} +
                                 " is valid on any channel; use the explicit-channel "
                                 "enqueue overload");
        }
        return makeError(ErrorCode::InvalidArgument, "message type is not network traffic");
    }
    return enqueueInternal(*channel, std::move(message), SendKind::Request);
}

Result<Seq> PeerSession::enqueue(Channel channel, codec::Message message) {
    const ChannelAffinity affinity = message.info().channel;
    if (affinity != ChannelAffinity::AnyChannel && channelFor(affinity) != channel) {
        return makeError(ErrorCode::InvalidArgument,
                         std::string{message.info().wireName} + " does not belong on the " +
                             std::string{channelName(channel)} + " channel");
    }
    return enqueueInternal(channel, std::move(message), SendKind::Request);
}

Result<Seq> PeerSession::enqueueReply(Channel channel, codec::Message message) {
    return enqueueInternal(channel, std::move(message), SendKind::Reply);
}

Result<Seq> PeerSession::enqueueOrderedReply(Channel channel, codec::Message message) {
    return enqueueInternal(channel, std::move(message), SendKind::OrderedReply);
}

void PeerSession::rememberAck(const std::string& messageId, const std::string& payloadUtf8) {
    for (auto& [id, payload] : sentAcks_) {
        if (id == messageId) {
            payload = payloadUtf8;
            return;
        }
    }
    sentAcks_.emplace_back(messageId, payloadUtf8);
    if (sentAcks_.size() > kSentAckCap) {
        sentAcks_.pop_front();
    }
}

bool PeerSession::expectingReply(const ChannelState& ch) const {
    if (ch.queue.empty()) {
        return false;
    }
    const PendingSend& front = ch.queue.front();
    return ch.awaitingAck || (ch.faulted && front.awaitReply && front.sentOnce);
}

bool PeerSession::identityMatches(const codec::Message& message) const {
    if (!config_.verifyPeerIdentity || message.info().envelope == EnvelopeKind::None) {
        return true;
    }
    // An absent or empty ID is tolerated (nothing contradicts the pairing);
    // a different one names somebody else's relationship.
    const std::optional<std::string_view> mosID = message.mosID();
    if (mosID.has_value() && !mosID->empty() && *mosID != config_.mosID) {
        return false;
    }
    const std::optional<std::string_view> ncsID = message.ncsID();
    return !ncsID.has_value() || ncsID->empty() || *ncsID == config_.ncsID ||
           message.type() == codec::MessageType::RoItemCue;
}

Result<Seq> PeerSession::enqueueInternal(Channel channel, codec::Message message, SendKind kind) {
    const bool isReply = kind != SendKind::Request;
    const codec::MessageInfo& info = message.info();
    if (info.legacyRemoved) {
        // Plan §3.2: the thirteen removed messages have no send path in 4.0
        // mode; they are receive-and-normalize only.
        return makeError(ErrorCode::ProtocolViolation,
                         std::string{info.wireName} + " was removed in MOS 4.0 and cannot be sent");
    }
    if (info.plugin) {
        return makeError(ErrorCode::InvalidArgument, std::string{info.wireName} +
                                                         " is host↔plug-in traffic, not "
                                                         "network traffic");
    }
    applyEnvelopeDefaults(message);

    PendingSend send;
    send.type = message.type();
    send.awaitReply = !isReply && !info.responses.empty();
    send.isReply = isReply;
    send.bypass = kind == SendKind::Reply;
    if (const std::optional<std::string_view> id = message.messageID()) {
        send.messageId = *id;
    }
    send.payloadUtf8 = codec::serializeMessage(message);
    if (isReply && info.isAck && !send.messageId.empty()) {
        rememberAck(send.messageId, send.payloadUtf8);
    }

    Result<std::vector<std::uint8_t>> wire =
        encoding::encodeFromUtf8(send.payloadUtf8, config_.wireEncode);
    if (!wire.ok()) {
        return std::move(wire).error();
    }
    send.wire = std::move(wire).value();

    ChannelState& ch = state(channel);
    if (ensureStoreOpen(channel)) {
        OutboundRecord record;
        record.messageId = send.messageId;
        record.payload = std::span<const std::uint8_t>{
            reinterpret_cast<const std::uint8_t*>(send.payloadUtf8.data()),
            send.payloadUtf8.size()};
        record.enqueuedAt = Timestamp{deps_.clock->wallClock().millisSinceEpoch};
        record.flags = send.isReply ? kOutboundReplyFlag : 0U;
        Result<Seq> appended = deps_.store->append(ch.stream, record);
        if (appended.ok()) {
            send.seq = appended.value();
            (void)deps_.store->sync(ch.stream);  // durable before send
        } else {
            SessionEvent event;
            event.type = SessionEventType::StoreError;
            event.channel = channel;
            event.diagnostic = appended.error().message;
            pushEvent(std::move(event));
        }
    }

    const Seq seq = send.seq;
    ch.queue.push_back(std::move(send));
    trySend(channel);
    announceDeadline();
    return seq;
}

bool PeerSession::transmit(Channel channel, const PendingSend& send) {
    ChannelState& ch = state(channel);
    const std::span<const std::uint8_t> frame{send.wire.data(), send.wire.size()};
    Result<void> sent = send.isReply ? deps_.transport->sendReply(channel, frame)
                                     : deps_.transport->send(channel, frame);
    if (!sent.ok()) {
        logMessage(LogLevel::Debug, "transport send failed: " + sent.error().message);
        return false;
    }
    ch.lastSendActivity = deps_.clock->now();
    return true;
}

void PeerSession::expectLateHeartbeatReplies(ChannelState& ch, const PendingSend& send, int count) {
    // Every transmission of a heartbeat can draw its own reply. A reply that
    // is not the one completing the exchange looks exactly like a
    // peer-initiated heartbeat; remember how many to swallow so they are
    // neither answered (the spec's "endless looping condition") nor
    // mistaken for peer traffic.
    if (send.type != codec::MessageType::Heartbeat || send.messageId.empty() || count <= 0) {
        return;
    }
    if (ch.lateHeartbeatReplies.size() >= kLateHeartbeatReplyCap &&
        !ch.lateHeartbeatReplies.contains(send.messageId)) {
        ch.lateHeartbeatReplies.erase(ch.lateHeartbeatReplies.begin());
    }
    ch.lateHeartbeatReplies[send.messageId] += count;
}

void PeerSession::trySend(Channel channel) {
    ChannelState& ch = state(channel);
    bool headBlocked = false;
    while (ch.connected && !ch.faulted && !ch.awaitingAck && !ch.queue.empty()) {
        PendingSend& front = ch.queue.front();
        if (!transmit(channel, front)) {
            // poll() keeps retrying while the queue is non-empty. A request
            // the transport cannot take right now (its own connection is
            // down while the peer's is up) must not hold up the answers the
            // peer is owed — they travel on the peer's connection.
            headBlocked = !front.isReply;
            break;
        }
        front.sentOnce = true;
        ++front.transmissions;
        if (front.awaitReply) {
            // In-flight-of-one (plan §3.3): hold the channel until a reply
            // from this message's response set arrives or retries lapse.
            ch.awaitingAck = true;
            ch.retriesLeft = config_.maxRetries;
            ch.retryDeadline =
                deps_.clock->now() + config_.retryTimeoutMillis + retryJitterMillis();
            break;
        }
        // Fire-and-forget traffic (acks, list replies): durable until sent.
        if (ch.storeOpen && front.seq != 0) {
            (void)deps_.store->markAcked(ch.stream, front.seq);
        }
        ch.queue.pop_front();
    }

    // Replies never wait behind our own in-flight request: the in-flight-of-
    // one rule governs messages we initiate, but the peer's exchange must
    // still complete — otherwise two crossing requests on one channel (e.g.
    // an NCS mid-commit while the device issues a resync roReq) deadlock,
    // each side holding the other's reply behind its blocked queue. The same
    // holds while the head message is faulted: the peer is still owed its
    // answers.
    if (!ch.connected || (!ch.awaitingAck && !ch.faulted && !headBlocked)) {
        return;
    }
    for (std::size_t i = 1; i < ch.queue.size();) {
        PendingSend& send = ch.queue[i];
        if (!send.bypass) {
            ++i;
            continue;
        }
        if (!transmit(channel, send)) {
            return;
        }
        if (ch.storeOpen && send.seq != 0) {
            (void)deps_.store->markAcked(ch.stream, send.seq);
        }
        ch.queue.erase(ch.queue.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

void PeerSession::completeInFlight(Channel channel, codec::Message reply) {
    ChannelState& ch = state(channel);
    PendingSend front = std::move(ch.queue.front());
    ch.queue.pop_front();
    ch.awaitingAck = false;
    ch.faulted = false;  // a late answer to a faulted head clears the fault
    ch.faultAnnounced = false;
    expectLateHeartbeatReplies(ch, front, front.transmissions - 1);
    if (ch.storeOpen && front.seq != 0) {
        (void)deps_.store->markAcked(ch.stream, front.seq);
    }
    SessionEvent event;
    event.type = SessionEventType::AckReceived;
    event.channel = channel;
    event.message = std::move(reply);
    event.seq = front.seq;
    event.messageId = std::move(front.messageId);
    pushEvent(std::move(event));
    trySend(channel);
}

void PeerSession::enqueueHeartbeatReply(Channel channel, const codec::Message& request) {
    codec::Message reply = codec::makeMessage(codec::MessageType::Heartbeat);
    reply.setMosID(config_.mosID);
    reply.setNcsID(config_.ncsID);
    codec::echoMessageID(request, reply);
    if (codec::XmlNode* body = reply.body()) {
        body->appendElement("time").setText(formatSpecTime(deps_.clock->wallClock()));
    }
    Result<Seq> queued = enqueueInternal(channel, std::move(reply), SendKind::Reply);
    if (!queued.ok()) {
        logMessage(LogLevel::Warning, "failed to queue heartbeat reply: " + queued.error().message);
    }
}

void PeerSession::handleInbound(Channel channel, codec::Message message) {
    ChannelState& ch = state(channel);
    const codec::MessageInfo& info = message.info();

    // keepAlive: no messageID, no reply; discard on receipt (spec §4.1.1).
    if (message.type() == codec::MessageType::KeepAlive) {
        logMessage(LogLevel::Trace, "keepAlive received");
        return;
    }

    if (!identityMatches(message)) {
        SessionEvent event;
        event.type = SessionEventType::ProtocolError;
        event.channel = channel;
        event.diagnostic = "message for mosID '" + std::string{message.mosID().value_or("")} +
                           "' / ncsID '" + std::string{message.ncsID().value_or("")} +
                           "' is not addressed to this relationship ('" + config_.mosID + "' / '" +
                           config_.ncsID + "'); dropped";
        pushEvent(std::move(event));
        return;
    }

    // Does this complete the in-flight exchange? Correlate by response set
    // first, then by echoed messageID when both sides carry one.
    if (expectingReply(ch) &&
        typeIsAmong(message.type(), codec::messageInfo(ch.queue.front().type).responses)) {
        const PendingSend& front = ch.queue.front();
        const std::optional<std::string_view> replyId = message.messageID();
        const bool idMismatch = replyId.has_value() && !replyId->empty() &&
                                !front.messageId.empty() && *replyId != front.messageId;
        if (!idMismatch) {
            completeInFlight(channel, std::move(message));
            return;
        }
        // A heartbeat is its own reply type: one carrying a different
        // messageID while ours is in flight is the peer's own heartbeat
        // crossing ours, not a bad reply — fall through and answer it.
        if (message.type() != codec::MessageType::Heartbeat) {
            SessionEvent event;
            event.type = SessionEventType::ProtocolError;
            event.channel = channel;
            event.diagnostic = "response messageID '" + std::string{*replyId} +
                               "' does not match in-flight '" + front.messageId + "'";
            pushEvent(std::move(event));
            return;
        }
    }

    std::string messageId;
    if (const std::optional<std::string_view> id = message.messageID()) {
        messageId = *id;
    }

    // A heartbeat reply to an earlier transmission of one of our own
    // heartbeats (a retry drew two replies, or the reply outlived a fault):
    // swallow it rather than answer it.
    if (message.type() == codec::MessageType::Heartbeat) {
        if (const auto late = ch.lateHeartbeatReplies.find(messageId);
            late != ch.lateHeartbeatReplies.end()) {
            if (--late->second <= 0) {
                ch.lateHeartbeatReplies.erase(late);
            }
            logMessage(LogLevel::Debug, "late heartbeat reply for messageID " + messageId);
            return;
        }
    }

    // Peer-initiated commands: duplicate suppression via the store window
    // (spec §3.3 "intelligent retry" made idempotent).
    if (!messageId.empty() && participatesInDedupe(info)) {
        Result<bool> seen = deps_.store->seenInbound(config_.peerKey, messageId);
        if (seen.ok() && seen.value()) {
            // Re-ack the blind retry; participatesInDedupe() guarantees a
            // bare ACK is the whole answer. The retry gets the verdict the
            // first copy got: the ack we sent then is replayed when we
            // still hold it (a refusal must not turn into "OK"). Only when
            // it is gone — the window outlives a restart, this cache does
            // not — is a plain acknowledgement synthesized.
            const codec::MessageType ackType = info.responses.front();
            const auto remembered =
                std::find_if(sentAcks_.begin(), sentAcks_.end(),
                             [&](const auto& entry) { return entry.first == messageId; });
            Result<codec::Message> replay =
                remembered != sentAcks_.end()
                    ? codec::parseMessage(remembered->second, config_.parseLimits)
                    : Result<codec::Message>{makeError(ErrorCode::NotFound, "no remembered ack")};
            codec::Message ack =
                replay.ok() ? std::move(replay).value() : codec::makeMessage(ackType);
            ack.setMessageID(messageId);
            if (!replay.ok() && ackType == codec::MessageType::RoAck) {
                if (const codec::XmlNode* body = message.body()) {
                    if (const codec::XmlNode* roId = body->firstChildElement("roID")) {
                        if (codec::XmlNode* ackBody = ack.body()) {
                            ackBody->appendElement("roID").setText(roId->text());
                            ackBody->appendElement("roStatus").setText("OK");
                        }
                    }
                }
            }
            (void)enqueueInternal(channel, std::move(ack), SendKind::Reply);
            SessionEvent event;
            event.type = SessionEventType::DuplicateSuppressed;
            event.channel = channel;
            event.messageId = messageId;
            pushEvent(std::move(event));
            return;
        }
        (void)deps_.store->noteInbound(config_.peerKey, messageId,
                                       Timestamp{deps_.clock->wallClock().millisSinceEpoch});
    }

    // heartbeat: echo with a timestamp. Loop avoidance: replies complete the
    // in-flight branch above, so only peer-initiated heartbeats get answers.
    if (message.type() == codec::MessageType::Heartbeat) {
        enqueueHeartbeatReply(channel, message);
        return;
    }

    // Legacy-removed messages: accept-and-normalize with a diagnostic
    // (plan §3.2 policy); the application only ever sees 4.0 semantics.
    if (info.legacyRemoved) {
        Result<codec::Message> normalized = codec::normalizeLegacy(message);
        SessionEvent event;
        event.channel = channel;
        event.messageId = messageId;
        if (normalized.ok()) {
            event.type = SessionEventType::MessageReceived;
            event.message = std::move(normalized).value();
            event.diagnostic = "normalized from legacy " + std::string{info.wireName};
        } else {
            event.type = SessionEventType::ProtocolError;
            event.message = std::move(message);
            event.diagnostic = normalized.error().message;
        }
        pushEvent(std::move(event));
        return;
    }

    if (message.type() == codec::MessageType::Unknown) {
        logMessage(LogLevel::Warning, "unknown message element received (delivered as-is)");
    }
    SessionEvent event;
    event.type = SessionEventType::MessageReceived;
    event.channel = channel;
    event.messageId = std::move(messageId);
    event.message = std::move(message);
    pushEvent(std::move(event));
}

void PeerSession::onFrameReceived(Channel channel, std::span<const std::uint8_t> frame) {
    Result<std::string> utf8 = encoding::decodeToUtf8(frame, config_.wireDecode);
    if (!utf8.ok()) {
        SessionEvent event;
        event.type = SessionEventType::ProtocolError;
        event.channel = channel;
        event.diagnostic = utf8.error().message;
        pushEvent(std::move(event));
        return;
    }
    Result<codec::Message> parsed = codec::parseMessage(utf8.value(), config_.parseLimits);
    if (!parsed.ok()) {
        // One MOS message per frame is the 4.0 rule, but a coalescing peer is
        // tolerated with a diagnostic (plan §5.1): re-scan the frame for
        // multiple <mos> documents before declaring it undecodable.
        if (handleCoalescedFrame(channel, utf8.value())) {
            announceDeadline();
            return;
        }
        SessionEvent event;
        event.type = SessionEventType::ProtocolError;
        event.channel = channel;
        event.diagnostic = parsed.error().message;
        pushEvent(std::move(event));
        return;
    }
    handleInbound(channel, std::move(parsed).value());
    announceDeadline();
}

bool PeerSession::handleCoalescedFrame(Channel channel, std::string_view utf8) {
    framing::DeframerOptions options;
    options.encoding = encoding::WireEncoding::Utf8;
    options.maxMessageBytes = config_.parseLimits.maxDocumentBytes;
    framing::StreamDeframer deframer{options};
    deframer.feed(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(utf8.data()),
                                                utf8.size()});

    std::vector<codec::Message> messages;
    while (std::optional<framing::Frame> doc = deframer.next()) {
        const std::string_view text{reinterpret_cast<const char*>(doc->document.data()),
                                    doc->document.size()};
        Result<codec::Message> parsed = codec::parseMessage(text, config_.parseLimits);
        if (!parsed.ok()) {
            return false;  // not a clean coalesced frame; report the original error
        }
        messages.push_back(std::move(parsed).value());
    }
    if (messages.size() < 2 || deframer.bufferedBytes() != 0 || deframer.totalDiscarded() != 0) {
        return false;
    }
    logMessage(LogLevel::Warning,
               "peer coalesced " + std::to_string(messages.size()) +
                   " MOS messages into one frame (one message per frame is the 4.0 rule)");
    for (codec::Message& message : messages) {
        handleInbound(channel, std::move(message));
    }
    return true;
}

void PeerSession::onChannelConnected(Channel channel) {
    ChannelState& ch = state(channel);
    ch.connected = true;
    ch.faulted = false;
    ch.faultAnnounced = false;
    ch.lastSendActivity = deps_.clock->now();
    if (config_.heartbeatIntervalMillis > 0 && ch.nextHeartbeat == MonotonicTime{}) {
        ch.nextHeartbeat = deps_.clock->now() + config_.heartbeatIntervalMillis;
    }
    if (ch.awaitingAck && !ch.queue.empty()) {
        // Mid-flight reconnect: retransmit immediately (same messageID; the
        // peer's dedupe window absorbs it if the first copy landed).
        if (transmit(channel, ch.queue.front())) {
            ++ch.queue.front().transmissions;
            ch.retryDeadline =
                deps_.clock->now() + config_.retryTimeoutMillis + retryJitterMillis();
        }
    }
    trySend(channel);  // resumes the queue; flushes replies past an in-flight
    announceDeadline();
}

void PeerSession::onChannelDisconnected(Channel channel) {
    state(channel).connected = false;
}

void PeerSession::sendKeepAlive(Channel channel) {
    codec::Message keepAlive = codec::makeMessage(codec::MessageType::KeepAlive);
    keepAlive.setMosID(config_.mosID);
    keepAlive.setNcsID(config_.ncsID);
    const std::string payload = codec::serializeMessage(keepAlive);
    Result<std::vector<std::uint8_t>> wire = encoding::encodeFromUtf8(payload, config_.wireEncode);
    if (!wire.ok()) {
        return;
    }
    // keepAlive is unsequenced and unacknowledged: straight to the wire,
    // never stored (discard-on-receipt semantics both ways).
    ChannelState& ch = state(channel);
    if (deps_.transport
            ->send(channel, std::span<const std::uint8_t>{wire.value().data(), wire.value().size()})
            .ok()) {
        ch.lastSendActivity = deps_.clock->now();
    }
}

std::int64_t PeerSession::retryJitterMillis() {
    if (deps_.random == nullptr || config_.retryTimeoutMillis <= 0) {
        return 0;
    }
    // Up to +10% keeps synchronized peers from retrying in lockstep.
    const std::int64_t bound = config_.retryTimeoutMillis / 10;
    return bound == 0 ? 0
                      : static_cast<std::int64_t>(deps_.random->next() %
                                                  static_cast<std::uint64_t>(bound));
}

void PeerSession::poll() {
    const MonotonicTime now = deps_.clock->now();
    for (std::size_t i = 0; i < kChannelCount; ++i) {
        const auto channel = static_cast<Channel>(i);
        ChannelState& ch = state(channel);
        if (!ch.connected) {
            continue;
        }

        if (ch.awaitingAck && now >= ch.retryDeadline && !ch.queue.empty()) {
            if (ch.retriesLeft > 0) {
                --ch.retriesLeft;
                logMessage(LogLevel::Debug, "retrying messageID " + ch.queue.front().messageId);
                if (transmit(channel, ch.queue.front())) {
                    ++ch.queue.front().transmissions;
                }
                ch.retryDeadline = now + config_.retryTimeoutMillis + retryJitterMillis();
            } else {
                // Link-fault escalation (plan §4.4): the message stays in the
                // store; reconnect resumes it.
                PendingSend& front = ch.queue.front();
                ch.faulted = true;
                ch.awaitingAck = false;
                ch.faultRetryDeadline = now + config_.faultRetryIntervalMillis;
                // Replies to the transmissions made so far may still arrive
                // while nothing is in flight; the resend after reconnect
                // starts a fresh count.
                expectLateHeartbeatReplies(ch, front, front.transmissions);
                front.transmissions = 0;
                if (!ch.faultAnnounced) {
                    ch.faultAnnounced = true;
                    SessionEvent failed;
                    failed.type = SessionEventType::DeliveryFailed;
                    failed.channel = channel;
                    failed.seq = front.seq;
                    failed.messageId = front.messageId;
                    pushEvent(std::move(failed));
                    SessionEvent fault;
                    fault.type = SessionEventType::ChannelFault;
                    fault.channel = channel;
                    fault.diagnostic = "retries exhausted";
                    pushEvent(std::move(fault));
                }
            }
        }

        // Spec §2.3: an unanswered message is "retried at intervals until a
        // response is received". The fault stands (nothing else is sent on
        // the channel), but the head keeps going out at the slow cadence.
        if (ch.faulted && !ch.queue.empty() && config_.faultRetryIntervalMillis > 0 &&
            now >= ch.faultRetryDeadline) {
            PendingSend& front = ch.queue.front();
            if (transmit(channel, front)) {
                front.sentOnce = true;
                ++front.transmissions;
            }
            ch.faultRetryDeadline = now + config_.faultRetryIntervalMillis;
        }

        if (config_.heartbeatIntervalMillis > 0 && !ch.faulted &&
            ch.nextHeartbeat != MonotonicTime{} && now >= ch.nextHeartbeat) {
            ch.nextHeartbeat = now + config_.heartbeatIntervalMillis;
            codec::Message heartbeat = codec::makeMessage(codec::MessageType::Heartbeat);
            if (codec::XmlNode* body = heartbeat.body()) {
                body->appendElement("time").setText(formatSpecTime(deps_.clock->wallClock()));
            }
            Result<Seq> queued = enqueueInternal(channel, std::move(heartbeat), SendKind::Request);
            if (!queued.ok()) {
                logMessage(LogLevel::Warning,
                           "failed to queue heartbeat: " + queued.error().message);
            }
        }

        if (config_.keepAliveIntervalMillis > 0 && !ch.faulted &&
            now.millis - ch.lastSendActivity.millis >= config_.keepAliveIntervalMillis) {
            sendKeepAlive(channel);
        }

        trySend(channel);
    }
    announceDeadline();
}

std::optional<MonotonicTime> PeerSession::nextDeadline() const {
    std::optional<MonotonicTime> earliest;
    const auto consider = [&earliest](MonotonicTime candidate) {
        if (!earliest.has_value() || candidate < *earliest) {
            earliest = candidate;
        }
    };
    for (const ChannelState& ch : channels_) {
        if (!ch.connected) {
            continue;
        }
        if (ch.faulted) {
            if (!ch.queue.empty() && config_.faultRetryIntervalMillis > 0) {
                consider(ch.faultRetryDeadline);
            }
            continue;
        }
        if (ch.awaitingAck) {
            consider(ch.retryDeadline);
        }
        if (config_.keepAliveIntervalMillis > 0) {
            consider(ch.lastSendActivity + config_.keepAliveIntervalMillis);
        }
        if (config_.heartbeatIntervalMillis > 0 && ch.nextHeartbeat != MonotonicTime{}) {
            consider(ch.nextHeartbeat);
        }
    }
    return earliest;
}

void PeerSession::announceDeadline() {
    if (deps_.timers == nullptr) {
        return;
    }
    if (const std::optional<MonotonicTime> deadline = nextDeadline()) {
        deps_.timers->requestWakeup(*deadline);
    }
}

std::optional<SessionEvent> PeerSession::nextEvent() {
    if (events_.empty()) {
        return std::nullopt;
    }
    SessionEvent event = std::move(events_.front());
    events_.pop_front();
    return event;
}

bool PeerSession::channelIdle(Channel channel) const {
    const ChannelState& ch = state(channel);
    return !ch.awaitingAck && ch.queue.empty();
}

std::size_t PeerSession::queueDepth(Channel channel) const {
    return state(channel).queue.size();
}

}  // namespace imos
