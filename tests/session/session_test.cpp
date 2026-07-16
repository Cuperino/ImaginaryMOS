// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// PeerSession reliability rules (plan §3.3), pinned deterministically with a
// fake clock, fake transport, and the in-memory store: in-flight-of-one per
// channel with cross-channel independence, retry-on-timeout schedules with
// link-fault escalation, durable-before-send ordering, duplicate-messageID
// suppression with ack re-emission, heartbeat echo with loop avoidance,
// keepAlive cadence floors, and store-driven resume after restart.

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/message.h>
#include <imos/encoding/encoding.h>
#include <imos/session/in_memory_store.h>
#include <imos/session/peer_session.h>

using imos::Channel;
using imos::MonotonicTime;
using imos::PeerSession;
using imos::SessionConfig;
using imos::SessionEvent;
using imos::SessionEventType;
using imos::codec::makeMessage;
using imos::codec::Message;
using imos::codec::MessageType;

namespace {

class FakeClock : public imos::IClock {
public:
    MonotonicTime now() override { return now_; }
    imos::WallClockTime wallClock() override {
        return imos::WallClockTime{1'750'000'000'000 + now_.millis};
    }
    void advance(std::int64_t millis) { now_.millis += millis; }

private:
    MonotonicTime now_{1000};
};

class FakeTransport : public imos::ITransport {
public:
    imos::Result<void> send(Channel channel, std::span<const std::uint8_t> frame) override {
        ++sendCalls;
        if (failSends) {
            return imos::makeError(imos::ErrorCode::IoError, "injected send failure");
        }
        sent[static_cast<std::size_t>(channel)].emplace_back(frame.begin(), frame.end());
        return {};
    }
    bool isConnected(Channel channel) const override {
        return connected[static_cast<std::size_t>(channel)];
    }

    std::vector<std::vector<std::uint8_t>> sent[imos::kChannelCount];
    bool connected[imos::kChannelCount] = {false, false, false};
    bool failSends = false;
    int sendCalls = 0;
};

/// Store decorator recording operation order against the transport, to pin
/// the durable-before-send guarantee.
class RecordingStore : public imos::IMessageStore {
public:
    RecordingStore(imos::IMessageStore& inner, std::vector<std::string>& trace)
        : inner_(inner), trace_(trace) {}

    imos::Result<imos::StreamHandle> open(const imos::PeerId& p, Channel c) override {
        return inner_.open(p, c);
    }
    imos::Result<imos::Seq> append(imos::StreamHandle s, const imos::OutboundRecord& r) override {
        trace_.push_back("append:" + std::string{r.messageId});
        return inner_.append(s, r);
    }
    imos::Result<void> markAcked(imos::StreamHandle s, imos::Seq q) override {
        trace_.push_back("ack:" + std::to_string(q));
        return inner_.markAcked(s, q);
    }
    imos::Result<imos::PendingCursor> pending(imos::StreamHandle s) override {
        return inner_.pending(s);
    }
    imos::Result<void> noteInbound(const imos::PeerId& p, imos::MessageIdView m,
                                   imos::Timestamp t) override {
        return inner_.noteInbound(p, m, t);
    }
    imos::Result<bool> seenInbound(const imos::PeerId& p, imos::MessageIdView m) override {
        return inner_.seenInbound(p, m);
    }
    imos::Result<void> compact(imos::StreamHandle s) override { return inner_.compact(s); }
    imos::Result<void> sync(imos::StreamHandle s) override {
        trace_.push_back("sync");
        return inner_.sync(s);
    }

private:
    imos::IMessageStore& inner_;
    std::vector<std::string>& trace_;
};

std::string decodeFrame(const std::vector<std::uint8_t>& frame) {
    auto decoded = imos::encoding::decodeToUtf8(frame, {});
    EXPECT_TRUE(decoded.ok());
    return std::move(decoded).value();
}

Message parseFrame(const std::vector<std::uint8_t>& frame) {
    auto parsed = imos::codec::parseMessage(decodeFrame(frame));
    EXPECT_TRUE(parsed.ok());
    return std::move(parsed).value();
}

std::vector<std::uint8_t> encodeXml(const std::string& xml) {
    auto encoded = imos::encoding::encodeFromUtf8(xml, {});
    EXPECT_TRUE(encoded.ok());
    return std::move(encoded).value();
}

Message makeMosObj(const std::string& objId) {
    Message message = makeMessage(MessageType::MosObj);
    if (imos::codec::XmlNode* body = message.body()) {
        body->appendElement("objID").setText(objId);
    }
    return message;
}

SessionConfig testConfig() {
    SessionConfig config;
    config.mosID = "device.example.mos";
    config.ncsID = "ncs.example.com";
    config.retryTimeoutMillis = 10'000;
    config.maxRetries = 2;
    config.keepAliveIntervalMillis = 30'000;
    return config;
}

struct Harness {
    FakeClock clock;
    FakeTransport transport;
    imos::InMemoryMessageStore store;
    std::unique_ptr<PeerSession> session;

    explicit Harness(SessionConfig config = testConfig()) {
        session = std::make_unique<PeerSession>(
            std::move(config),
            PeerSession::Dependencies{&clock, &transport, &store, nullptr, nullptr, nullptr});
    }

    void connectAll() {
        for (std::size_t i = 0; i < imos::kChannelCount; ++i) {
            transport.connected[i] = true;
            session->onChannelConnected(static_cast<Channel>(i));
        }
    }

    std::vector<SessionEvent> drainEvents() {
        std::vector<SessionEvent> events;
        while (auto event = session->nextEvent()) {
            events.push_back(std::move(*event));
        }
        return events;
    }

    const std::vector<std::vector<std::uint8_t>>& momFrames() {
        return transport.sent[static_cast<std::size_t>(Channel::Mom)];
    }

    void deliverMomAck(const std::string& messageId) {
        session->onFrameReceived(
            Channel::Mom,
            encodeXml("<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
                      "<messageID>" +
                      messageId +
                      "</messageID><mosAck><objID>M</objID><objRev>1</objRev>"
                      "<status>ACK</status><statusDescription/></mosAck></mos>"));
    }
};

}  // namespace

TEST(PeerSession, InFlightOfOnePerChannelWithIndependentChannels) {
    Harness h;
    h.connectAll();

    ASSERT_TRUE(h.session->enqueue(makeMosObj("M1")).ok());
    ASSERT_TRUE(h.session->enqueue(makeMosObj("M2")).ok());
    // mom holds the second message while the first awaits its mosAck.
    EXPECT_EQ(h.momFrames().size(), 1U);
    EXPECT_EQ(h.session->queueDepth(Channel::Mom), 2U);

    // The ro channel is independent: an roCreate goes straight out.
    Message roCreate = makeMessage(MessageType::RoCreate);
    ASSERT_TRUE(h.session->enqueue(std::move(roCreate)).ok());
    EXPECT_EQ(h.transport.sent[static_cast<std::size_t>(Channel::Ro)].size(), 1U);

    // Ack the first mom message (responses echo the request messageID).
    const Message first = parseFrame(h.momFrames()[0]);
    h.deliverMomAck(std::string{first.messageID().value_or("")});
    EXPECT_EQ(h.momFrames().size(), 2U);

    const auto events = h.drainEvents();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events[0].type, SessionEventType::AckReceived);
}

TEST(PeerSession, MessageIdsAreAssignedSequentially) {
    Harness h;
    h.connectAll();
    ASSERT_TRUE(h.session->enqueue(makeMosObj("M1")).ok());
    const Message sent = parseFrame(h.momFrames()[0]);
    EXPECT_EQ(sent.messageID().value_or(""), "1");
    EXPECT_EQ(sent.mosID().value_or(""), "device.example.mos");
    EXPECT_EQ(sent.ncsID().value_or(""), "ncs.example.com");
}

TEST(PeerSession, RetryOnTimeoutThenLinkFault) {
    Harness h;
    h.connectAll();
    ASSERT_TRUE(h.session->enqueue(makeMosObj("M1")).ok());
    EXPECT_EQ(h.momFrames().size(), 1U);

    h.clock.advance(9'999);
    h.session->poll();
    EXPECT_EQ(h.momFrames().size(), 1U);  // not yet due

    h.clock.advance(2);  // past the 10 s retry timeout
    h.session->poll();
    EXPECT_EQ(h.momFrames().size(), 2U);  // first retry, same bytes
    EXPECT_EQ(h.momFrames()[0], h.momFrames()[1]);

    h.clock.advance(10'001);
    h.session->poll();
    EXPECT_EQ(h.momFrames().size(), 3U);  // second (last) retry

    h.clock.advance(10'001);
    h.session->poll();  // retries exhausted -> fault
    const auto events = h.drainEvents();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].type, SessionEventType::DeliveryFailed);
    EXPECT_EQ(events[1].type, SessionEventType::ChannelFault);

    // The message survives in the store for resume-after-reconnect.
    auto handle = h.store.open(h.session ? "device.example.mos|ncs.example.com" : "", Channel::Mom);
    ASSERT_TRUE(handle.ok());
    auto pending = h.store.pending(handle.value());
    ASSERT_TRUE(pending.ok());
    EXPECT_EQ(pending.value().remaining(), 1U);

    // Reconnect resumes delivery.
    h.session->onChannelConnected(Channel::Mom);
    EXPECT_EQ(h.momFrames().size(), 4U);
}

TEST(PeerSession, DurableBeforeSend) {
    FakeClock clock;
    FakeTransport transport;
    imos::InMemoryMessageStore inner;
    std::vector<std::string> trace;
    RecordingStore store{inner, trace};

    PeerSession session{testConfig(), PeerSession::Dependencies{&clock, &transport, &store, nullptr,
                                                                nullptr, nullptr}};
    transport.connected[0] = true;
    session.onChannelConnected(Channel::Mom);
    const int sendsBefore = transport.sendCalls;
    ASSERT_TRUE(session.enqueue(makeMosObj("M1")).ok());
    ASSERT_GE(trace.size(), 2U);
    EXPECT_EQ(trace[0], "append:1");
    EXPECT_EQ(trace[1], "sync");
    EXPECT_EQ(transport.sendCalls, sendsBefore + 1);  // send strictly after append+sync
}

TEST(PeerSession, DuplicateInboundIsSuppressedAndReAcked) {
    Harness h;
    h.connectAll();
    const std::string roCreate =
        "<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
        "<messageID>555</messageID><roCreate><roID>RO1</roID><roSlug>5PM</roSlug>"
        "</roCreate></mos>";

    h.session->onFrameReceived(Channel::Ro, encodeXml(roCreate));
    auto events = h.drainEvents();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].type, SessionEventType::MessageReceived);
    EXPECT_EQ(events[0].message.type(), MessageType::RoCreate);

    // Blind peer retry: absorbed, and a bare roAck goes back out.
    const auto roFramesBefore = h.transport.sent[static_cast<std::size_t>(Channel::Ro)].size();
    h.session->onFrameReceived(Channel::Ro, encodeXml(roCreate));
    events = h.drainEvents();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].type, SessionEventType::DuplicateSuppressed);
    EXPECT_EQ(events[0].messageId, "555");

    const auto& roFrames = h.transport.sent[static_cast<std::size_t>(Channel::Ro)];
    ASSERT_EQ(roFrames.size(), roFramesBefore + 1);
    const Message ack = parseFrame(roFrames.back());
    EXPECT_EQ(ack.type(), MessageType::RoAck);
    EXPECT_EQ(ack.messageID().value_or(""), "555");
}

TEST(PeerSession, HeartbeatEchoWithLoopAvoidance) {
    SessionConfig config = testConfig();
    config.heartbeatIntervalMillis = 60'000;
    Harness h{std::move(config)};
    h.connectAll();

    // Peer-initiated heartbeat gets echoed with a timestamp, no app event.
    h.session->onFrameReceived(
        Channel::Mom,
        encodeXml("<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
                  "<messageID>0042</messageID><heartbeat>"
                  "<time>2026-07-14T12:00:00</time></heartbeat></mos>"));
    EXPECT_TRUE(h.drainEvents().empty());
    ASSERT_EQ(h.momFrames().size(), 1U);
    const Message echo = parseFrame(h.momFrames()[0]);
    EXPECT_EQ(echo.type(), MessageType::Heartbeat);
    EXPECT_EQ(echo.messageID().value_or(""), "0042");
    ASSERT_NE(echo.body()->firstChildElement("time"), nullptr);

    // Originated heartbeat: fires on the interval, and the peer's echo
    // completes it rather than triggering a reply (loop avoidance).
    h.clock.advance(60'001);
    h.session->poll();
    ASSERT_EQ(h.momFrames().size(), 2U);
    const Message mine = parseFrame(h.momFrames()[1]);
    ASSERT_EQ(mine.type(), MessageType::Heartbeat);

    h.session->onFrameReceived(
        Channel::Mom,
        encodeXml("<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
                  "<messageID>" +
                  std::string{mine.messageID().value_or("")} +
                  "</messageID><heartbeat><time>2026-07-14T12:01:00</time>"
                  "</heartbeat></mos>"));
    const auto events = h.drainEvents();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].type, SessionEventType::AckReceived);
    EXPECT_EQ(h.momFrames().size(), 2U);  // no re-echo of the echo
}

TEST(PeerSession, KeepAliveCadenceWithSpecFloor) {
    SessionConfig config = testConfig();
    config.keepAliveIntervalMillis = 5'000;  // below the floor: clamped to 30 s
    Harness h{std::move(config)};
    h.connectAll();

    h.clock.advance(10'000);
    h.session->poll();
    EXPECT_TRUE(h.momFrames().empty());  // 5 s config must NOT emit yet

    h.clock.advance(20'001);  // 30 s idle reached
    h.session->poll();
    ASSERT_EQ(h.momFrames().size(), 1U);
    const Message keepAlive = parseFrame(h.momFrames()[0]);
    EXPECT_EQ(keepAlive.type(), MessageType::KeepAlive);
    EXPECT_FALSE(keepAlive.messageID().has_value());  // spec: no messageID

    // Activity resets the idle timer.
    h.clock.advance(29'000);
    h.session->poll();
    EXPECT_EQ(h.momFrames().size(), 1U);
}

TEST(PeerSession, StoreDrivenResumeAfterRestart) {
    FakeClock clock;
    imos::InMemoryMessageStore store;
    {
        FakeTransport offline;
        PeerSession first{testConfig(), PeerSession::Dependencies{&clock, &offline, &store, nullptr,
                                                                  nullptr, nullptr}};
        ASSERT_TRUE(first.enqueue(makeMosObj("M9")).ok());
        // Never connected: nothing sent, but the record is in the store.
    }

    FakeTransport transport;
    PeerSession second{testConfig(), PeerSession::Dependencies{&clock, &transport, &store, nullptr,
                                                               nullptr, nullptr}};
    transport.connected[0] = true;
    second.onChannelConnected(Channel::Mom);

    const auto& frames = transport.sent[static_cast<std::size_t>(Channel::Mom)];
    ASSERT_EQ(frames.size(), 1U);
    const Message resumed = parseFrame(frames[0]);
    EXPECT_EQ(resumed.type(), MessageType::MosObj);
    ASSERT_NE(resumed.body()->firstChildElement("objID"), nullptr);
    EXPECT_EQ(resumed.body()->firstChildElement("objID")->text(), "M9");
}

TEST(PeerSession, LegacyInboundIsNormalizedWithDiagnostic) {
    Harness h;
    h.connectAll();
    h.session->onFrameReceived(
        Channel::Ro,
        encodeXml("<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
                  "<messageID>88</messageID><roStoryInsert><roID>R</roID>"
                  "<storyID>S2</storyID><story><storyID>S9</storyID></story>"
                  "</roStoryInsert></mos>"));
    const auto events = h.drainEvents();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].type, SessionEventType::MessageReceived);
    EXPECT_EQ(events[0].message.type(), MessageType::RoElementAction);
    EXPECT_NE(events[0].diagnostic.find("roStoryInsert"), std::string::npos);
}

TEST(PeerSession, OutboundLegacyIsRejected) {
    Harness h;
    h.connectAll();
    Message legacy = makeMessage(MessageType::RoStorySwap);
    const auto result = h.session->enqueue(std::move(legacy));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, imos::ErrorCode::ProtocolViolation);
}

TEST(PeerSession, UndecodableFrameSurfacesProtocolError) {
    Harness h;
    h.connectAll();
    const std::vector<std::uint8_t> oddLength = {0x00, '<', 0x00};
    h.session->onFrameReceived(Channel::Mom, oddLength);
    const auto events = h.drainEvents();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].type, SessionEventType::ProtocolError);
    EXPECT_NE(events[0].diagnostic.find("misconfigured"), std::string::npos);
}

TEST(PeerSession, EnqueueReplyIsTerminalAndDoesNotAwaitAck) {
    // A response that completes the peer's exchange (e.g. a mosObj answering a
    // mosReqObj) is sent in sequence but never holds the channel awaiting an
    // ack of its own — the requester correlates it by the echoed messageID.
    Harness h;
    h.connectAll();

    Message reply = makeMosObj("R1");
    reply.setMessageID("654");  // echo the request's messageID
    ASSERT_TRUE(h.session->enqueueReply(Channel::Mom, std::move(reply)).ok());

    ASSERT_EQ(h.momFrames().size(), 1U);
    EXPECT_EQ(parseFrame(h.momFrames()[0]).messageID().value_or(""), "654");
    EXPECT_TRUE(h.session->channelIdle(Channel::Mom));  // no phantom in-flight
    EXPECT_TRUE(h.drainEvents().empty());

    // The channel was left free, so ordinary sequenced traffic flows straight
    // out behind the reply (contrast: this one awaits its own ack).
    ASSERT_TRUE(h.session->enqueue(makeMosObj("N1")).ok());
    EXPECT_EQ(h.momFrames().size(), 2U);
    EXPECT_FALSE(h.session->channelIdle(Channel::Mom));
}

TEST(PeerSession, NextDeadlineTracksRetryTimer) {
    Harness h;
    h.connectAll();
    ASSERT_TRUE(h.session->enqueue(makeMosObj("M1")).ok());
    const auto deadline = h.session->nextDeadline();
    ASSERT_TRUE(deadline.has_value());
    EXPECT_EQ(deadline->millis, h.clock.now().millis + 10'000);
}

TEST(PeerSession, CoalescedFrameIsToleratedWithBothMessagesDelivered) {
    // One MOS message per WS message is the 4.0 rule; a peer that coalesces
    // is tolerated with a diagnostic (plan §5.1).
    Harness h;
    h.connectAll();
    const std::string doc1 =
        "<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
        "<messageID>71</messageID><roReadyToAir><roID>RO1</roID>"
        "<roAir>READY</roAir></roReadyToAir></mos>";
    const std::string doc2 =
        "<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
        "<messageID>72</messageID><roDelete><roID>RO2</roID></roDelete></mos>";
    h.session->onFrameReceived(Channel::Ro, encodeXml(doc1 + doc2));

    const auto events = h.drainEvents();
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].type, SessionEventType::MessageReceived);
    EXPECT_EQ(events[0].message.type(), MessageType::RoReadyToAir);
    EXPECT_EQ(events[1].type, SessionEventType::MessageReceived);
    EXPECT_EQ(events[1].message.type(), MessageType::RoDelete);
}

TEST(PeerSession, CoalescedFrameWithTrailingGarbageStaysAProtocolError) {
    Harness h;
    h.connectAll();
    const std::string doc =
        "<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
        "<messageID>73</messageID><roDelete><roID>RO3</roID></roDelete></mos>";
    h.session->onFrameReceived(Channel::Ro, encodeXml(doc + "<mos><trailing-junk"));
    const auto events = h.drainEvents();
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].type, SessionEventType::ProtocolError);
}
