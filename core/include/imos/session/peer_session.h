// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// PeerSession skeleton (plan §4.4, milestone M1). One PeerSession owns the
// relationship with one remote party across up to three channels. Per
// channel it enforces the spec's reliability rules (§3.3): strictly
// sequential in-flight-of-one delivery with mutually independent channels,
// retry on ack timeout with capped retries then link-fault escalation,
// durable-before-send buffering through IMessageStore, and inbound
// messageID duplicate suppression so peer retries stay idempotent. It also
// runs the Profile 0 machinery: heartbeat echo with loop avoidance and
// keepAlive emission on idle (never faster than the spec's 30-second
// floor). Reconnect scheduling lives in ReconnectPolicy (session/reconnect.h),
// driven by the transport's pump (M2). The resync heuristic lands in M3;
// the ResyncNeeded event type is the seam.
//
// The session is sans-I/O: hosts push inbound frames and connectivity
// changes in, poll() drives timers off the injected clock, and outbound
// frames leave through the injected ITransport.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <imos/codec/message.h>
#include <imos/core_export.h>
#include <imos/encoding/encoding.h>
#include <imos/error.h>
#include <imos/protocol.h>
#include <imos/session/message_store.h>
#include <imos/session/runtime.h>
#include <imos/session/transport.h>

namespace imos {

struct SessionConfig {
    std::string mosID;
    std::string ncsID;
    /// Store/dedupe key for this relationship; defaults to "mosID|ncsID".
    PeerId peerKey;

    std::int64_t retryTimeoutMillis = 10'000;  ///< well under the 60 s ack ceiling
    int maxRetries = 3;
    /// Idle threshold for keepAlive emission; clamped to the spec's 30 s
    /// floor ("no faster than every thirty seconds"). 0 disables.
    std::int64_t keepAliveIntervalMillis = 30'000;
    /// Heartbeat origination cadence; 0 = answer only, never originate.
    std::int64_t heartbeatIntervalMillis = 0;

    encoding::EncodeOptions wireEncode;  ///< per-peer wire encoding (plan §3.4)
    encoding::DecodeOptions wireDecode;
    codec::XmlParseLimits parseLimits;
};

enum class SessionEventType : std::uint8_t {
    MessageReceived,      ///< peer-initiated message for the application
    AckReceived,          ///< an in-flight exchange completed (carries the reply)
    DeliveryFailed,       ///< retries exhausted for the identified message
    ChannelFault,         ///< link-fault escalation after DeliveryFailed
    DuplicateSuppressed,  ///< peer retry absorbed; not re-delivered
    ProtocolError,        ///< undecodable/unparseable/mismatched inbound
    StoreError,           ///< IMessageStore failure (session continues volatile)
    ResyncNeeded,         ///< placeholder seam for the §3.3 resync heuristic
};

struct SessionEvent {
    SessionEventType type = SessionEventType::ProtocolError;
    Channel channel = Channel::Mom;
    codec::Message message;  ///< MessageReceived / AckReceived payload
    Seq seq = 0;             ///< AckReceived / DeliveryFailed
    std::string messageId;
    std::string diagnostic;  ///< human-readable detail (normalization, errors)
};

class IMOS_CORE_API PeerSession {
public:
    struct Dependencies {
        IClock* clock = nullptr;          ///< required
        ITransport* transport = nullptr;  ///< required
        IMessageStore* store = nullptr;   ///< required
        ILogger* logger = nullptr;        ///< optional
        IRandom* random = nullptr;        ///< optional: retry jitter
        ITimerQueue* timers = nullptr;    ///< optional: push-mode wakeups
    };

    PeerSession(SessionConfig config, Dependencies deps);

    /// Queue a message for sequenced delivery; the channel comes from the
    /// message type's Port annotation. Messages valid on any channel
    /// (Profile 0) need the explicit-channel overload. Legacy-removed types
    /// are rejected (they have no send path in 4.0 mode); assigns the
    /// messageID when the envelope wants one and none is set.
    Result<Seq> enqueue(codec::Message message);
    Result<Seq> enqueue(Channel channel, codec::Message message);

    /// Queue a response that completes the peer's in-flight exchange: sent in
    /// sequence on `channel` but, unlike enqueue(), it never awaits an ack of
    /// its own — the requester correlates it by the echoed messageID (e.g. a
    /// listMachInfo answering reqMachInfo, or a mosObj answering mosReqObj).
    /// Echo the request's messageID onto `message` before calling. This is the
    /// terminal-reply seam every profile responder uses (plan §3.3).
    Result<Seq> enqueueReply(Channel channel, codec::Message message);

    void onChannelConnected(Channel channel);
    void onChannelDisconnected(Channel channel);
    void onFrameReceived(Channel channel, std::span<const std::uint8_t> frame);

    /// Drive timers (retries, keepAlive, heartbeats) and pending sends.
    /// Pull-mode hosts call this from their own loop; no threads exist here.
    void poll();

    [[nodiscard]] std::optional<SessionEvent> nextEvent();
    /// Earliest instant poll() has timed work due, for host scheduling.
    [[nodiscard]] std::optional<MonotonicTime> nextDeadline() const;

    [[nodiscard]] bool channelIdle(Channel channel) const;
    [[nodiscard]] std::size_t queueDepth(Channel channel) const;

private:
    struct PendingSend {
        Seq seq = 0;
        std::string messageId;
        std::string payloadUtf8;
        std::vector<std::uint8_t> wire;
        codec::MessageType type = codec::MessageType::Unknown;
        bool awaitReply = false;
        bool sentOnce = false;
    };
    struct ChannelState {
        bool connected = false;
        bool faulted = false;
        bool storeOpen = false;
        StreamHandle stream;
        std::deque<PendingSend> queue;
        bool awaitingAck = false;
        int retriesLeft = 0;
        MonotonicTime retryDeadline{};
        MonotonicTime lastSendActivity{};
        MonotonicTime nextHeartbeat{};
    };

    ChannelState& state(Channel channel);
    const ChannelState& state(Channel channel) const;
    void logMessage(LogLevel level, std::string_view text);
    void pushEvent(SessionEvent event);
    bool ensureStoreOpen(Channel channel);
    void loadPersistedQueue(Channel channel);
    Result<Seq> enqueueInternal(Channel channel, codec::Message message, bool isReply);
    void trySend(Channel channel);
    bool transmit(Channel channel, const PendingSend& send);
    void completeInFlight(Channel channel, codec::Message reply);
    void handleInbound(Channel channel, codec::Message message);
    /// Split a frame carrying several <mos> documents (tolerated with a
    /// diagnostic, plan §5.1); false if it is not cleanly coalesced.
    bool handleCoalescedFrame(Channel channel, std::string_view utf8);
    void sendKeepAlive(Channel channel);
    void enqueueHeartbeatReply(Channel channel, const codec::Message& request);
    void applyEnvelopeDefaults(codec::Message& message);
    std::int64_t retryJitterMillis();
    void announceDeadline();

    SessionConfig config_;
    Dependencies deps_;
    std::deque<SessionEvent> events_;
    std::array<ChannelState, kChannelCount> channels_;
    std::uint64_t nextMessageId_ = 1;
};

}  // namespace imos
