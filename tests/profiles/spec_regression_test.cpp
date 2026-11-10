// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Spec-review regressions. Each test pins one defect found by reviewing the
// implementation against the MOS 4.0 specification text, reproduced with a
// real imos::Device against a real incs::Ncs over the loopback pair; every
// test names the spec clause it asserts.
//
//   1. A late/duplicate ack must not enter the inbound dedupe window
//      (spec §4.1.6: only peer *requests* with identical messageIDs are
//      repeats).
//   2. Diffs queued before a resync must not be re-applied on top of the
//      roList rebuild (spec §2.3 "Very Important Note": every message is
//      applied exactly once, in order).
//   3. roStatus is "OK" or an error description (spec §6) — an error text
//      without a "NACK" prefix is still a failure.
//   4. A retried information request is answered again (spec §4.1.6: dedupe
//      "is not relevant for messages which say 'give me that piece of
//      information'").
//   5. roMetadataReplace merges into the header (spec §3.4.4); it does not
//      drop the tags the message leaves out.
//   6. Times carry comma-separated fractions (spec §6:
//      YYYY-MM-DD'T'hh:mm:ss[,ddd]['Z']).
//   7. A leaseLock holder can send its follow-up action inside the lease
//      (spec §3.9.1 note 5).

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/message.h>
#include <imos/encoding/encoding.h>
#include <imos/profiles/profile7.h>
#include <imos/time_format.h>

#include "two_role_fixture.h"

using imos::Channel;
using imos::codec::Message;
using imos::codec::MessageType;
using imos::codec::XmlNode;
using imos::test::LoopbackEndpoint;
using imos::test::TwoRoleFixture;
using incs::RunningOrderAuthor;

namespace {

/// Envelope shared by the hand-built frames: the fixture's identities plus
/// the given messageID.
std::string envelope(const std::string& messageID) {
    return std::string{"<mosID>"} + TwoRoleFixture::kMosID + "</mosID><ncsID>" +
           TwoRoleFixture::kNcsID + "</ncsID><messageID>" + messageID + "</messageID>";
}

std::vector<std::uint8_t> encode(const std::string& xml) {
    auto encoded = imos::encoding::encodeFromUtf8(xml, {});
    EXPECT_TRUE(encoded.ok());
    return std::move(encoded).value();
}

Message parseFrame(const std::vector<std::uint8_t>& frame) {
    auto decoded = imos::encoding::decodeToUtf8(frame, {});
    EXPECT_TRUE(decoded.ok());
    auto parsed = imos::codec::parseMessage(decoded.value());
    EXPECT_TRUE(parsed.ok());
    return std::move(parsed).value();
}

std::deque<std::vector<std::uint8_t>>& roQueue(LoopbackEndpoint& endpoint) {
    return endpoint.outbound[static_cast<std::size_t>(Channel::Ro)];
}

XmlNode storyWithItems(const std::string& storyID, int count) {
    XmlNode story = RunningOrderAuthor::makeStory(storyID, "slug");
    for (int i = 0; i < count; ++i) {
        story.children.push_back(RunningOrderAuthor::makeItem(
            storyID + "-i" + std::to_string(i), "OBJ" + std::to_string(i), TwoRoleFixture::kMosID));
    }
    return story;
}

std::vector<incs::CommitEvent> drainCommits(incs::Ncs& ncs) {
    std::vector<incs::CommitEvent> events;
    while (std::optional<incs::CommitEvent> event = ncs.author().nextEvent()) {
        events.push_back(std::move(*event));
    }
    return events;
}

/// Author one RO with stories s1..s4 (2 items each) and converge.
void convergeBaseRo(TwoRoleFixture& fx, const std::string& roID) {
    ASSERT_TRUE(fx.ncs->author().create(roID, "Rundown").ok());
    for (const char* id : {"s1", "s2", "s3", "s4"}) {
        ASSERT_TRUE(fx.ncs->author().insertStory(roID, storyWithItems(id, 2)).ok());
    }
    ASSERT_TRUE(fx.ncs->author().commit(roID).ok());
    fx.settle();
    ASSERT_EQ(fx.device->runningOrderModel().storyIDs(roID),
              (std::vector<std::string>{"s1", "s2", "s3", "s4"}));
    for (const incs::CommitEvent& event : drainCommits(*fx.ncs)) {
        ASSERT_TRUE(event.ok) << event.roStatus;
    }
}

}  // namespace

// 1. The device retries one request (same messageID, per §4.1.6) and so
//    receives two acks for it. The second ack arrives with nothing in flight
//    and is currently recorded in the *inbound* dedupe window — under our own
//    messageID. The peer's unrelated request that happens to carry the same
//    number is then suppressed and auto-ACKed "OK" without being applied.
//    (core/session/peer_session.cpp, handleInbound: the unsolicited-reply
//    path falls through to noteInbound.)
TEST(SpecRegression, LateDuplicateAckMustNotSuppressAPeerRequest) {
    TwoRoleFixture fx;
    fx.connectAll();

    // Device-originated messageID "1" on ro.
    Message stat = imos::codec::makeMessage(MessageType::RoElementStat);
    stat.body()->setAttribute("element", "RO");
    stat.body()->appendElement("roID").setText("ROX");
    stat.body()->appendElement("status").setText("READY");
    stat.body()->appendElement("time").setText("2026-01-01T00:00:00");
    ASSERT_TRUE(fx.device->session().enqueue(std::move(stat)).ok());

    // The ack is slow: the device retries before any ack lands.
    fx.clock.advanceSeconds(11);
    fx.device->poll();
    ASSERT_EQ(roQueue(fx.deviceTransport).size(), 2U);  // original + retry
    fx.settle();                                        // the NCS acks both copies

    // The NCS's own first request also carries messageID "1".
    ASSERT_TRUE(fx.ncs->author().create("RO1", "Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();

    const std::vector<incs::CommitEvent> commits = drainCommits(*fx.ncs);
    ASSERT_EQ(commits.size(), 1U);
    EXPECT_TRUE(commits[0].ok) << commits[0].roStatus;
    // The NCS was told the roCreate succeeded, so the device must hold it.
    EXPECT_TRUE(fx.device->runningOrderModel().has("RO1"))
        << "roCreate was acked OK but suppressed as a duplicate of our own ack's messageID";
}

// 2. The device has lost one story. Three commits are queued: the first is
//    NACKed (unknown storyID) and triggers the §2.3 roReq resync. The roList
//    reply is built from the author's desired state — which already includes
//    the two commits still waiting in the queue — and overtakes them. They
//    are then applied a second time on top of the rebuild: the SWAP swaps
//    back, both commits report OK, and the author's projection is marked
//    valid, so nothing ever repairs it.
//    (incs/running_order_author.cpp, RunningOrderAuthor::runningOrder.)
TEST(SpecRegression, DiffsQueuedBeforeAResyncAreNotAppliedTwice) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");

    // The device silently drops s4 — a frame the author knows nothing about.
    fx.device->onFrameReceived(Channel::Ro,
                               encode("<mos>" + envelope("900") +
                                      "<roElementAction operation=\"DELETE\"><roID>RO1</roID>"
                                      "<element_source><storyID>s4</storyID></element_source>"
                                      "</roElementAction></mos>"));
    roQueue(fx.deviceTransport).clear();  // drop the stray ack
    ASSERT_EQ(fx.device->runningOrderModel().storyIDs("RO1").size(), 3U);

    ASSERT_TRUE(fx.ncs->author().moveStory("RO1", "s4", "s1").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", storyWithItems("s5", 1)).ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    ASSERT_TRUE(fx.ncs->author().swapStories("RO1", "s2", "s3").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();

    // Give the author one more chance to notice and repair the projection.
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();

    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"),
              fx.ncs->author().desired().storyIDs("RO1"))
        << "the device's running order diverged from the NCS after a resync";
}

// 3. Spec §6: roStatus is "OK" or an error description. A device that
//    reports its error without the word "NACK" has still refused the
//    message; the commit must not resolve as successful.
//    (core/profiles/profile2.cpp, roAckIsNack: starts_with("NACK").)
TEST(SpecRegression, RoAckErrorDescriptionIsAFailure) {
    TwoRoleFixture fx;
    fx.connectAll();
    ASSERT_TRUE(fx.ncs->author().create("RO1", "Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());

    // Intercept the roCreate and answer as a spec-literal device would.
    ASSERT_EQ(roQueue(fx.ncsTransport).size(), 1U);
    const Message create = parseFrame(roQueue(fx.ncsTransport).front());
    roQueue(fx.ncsTransport).clear();
    ASSERT_TRUE(create.messageID().has_value());
    fx.ncs->onFrameReceived(Channel::Ro,
                            encode("<mos>" + envelope(std::string{*create.messageID()}) +
                                   "<roAck><roID>RO1</roID>"
                                   "<roStatus>Cannot create running order: disk full</roStatus>"
                                   "</roAck></mos>"));

    const std::vector<incs::CommitEvent> commits = drainCommits(*fx.ncs);
    ASSERT_EQ(commits.size(), 1U);
    EXPECT_FALSE(commits[0].ok) << "roStatus '" << commits[0].roStatus
                                << "' is an error description, not OK";
}

// 4. The roList answering a roReq is lost in transit, so the device retries
//    the roReq with the same messageID (§4.1.6). The retry is currently
//    swallowed as a duplicate with no reply at all — only bare-ack exchanges
//    are re-acked — so the device retries into a link fault. Information
//    requests are idempotent and must simply be answered again.
//    (core/session/peer_session.cpp, handleInbound duplicate branch.)
TEST(SpecRegression, RetriedRoReqIsAnsweredAgain) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");

    const std::string request = "<mos>" + envelope("777") + "<roReq><roID>RO1</roID></roReq></mos>";
    fx.ncs->onFrameReceived(Channel::Ro, encode(request));
    ASSERT_EQ(roQueue(fx.ncsTransport).size(), 1U);
    ASSERT_EQ(parseFrame(roQueue(fx.ncsTransport).front()).type(), MessageType::RoList);
    roQueue(fx.ncsTransport).clear();  // the roList never arrives

    fx.ncs->onFrameReceived(Channel::Ro, encode(request));  // the retry
    ASSERT_EQ(roQueue(fx.ncsTransport).size(), 1U) << "the retried roReq went unanswered";
    EXPECT_EQ(parseFrame(roQueue(fx.ncsTransport).front()).type(), MessageType::RoList);
}

// 5. Spec §3.4.4: tags present in roMetadataReplace replace the RO's values,
//    absent ones are left alone, and a mosExternalMetadata block replaces
//    only a block with a matching mosSchema. A message carrying just the
//    required roID + roSlug currently wipes every other header field.
//    (core/profiles/running_order_model.cpp, applyMetadataReplace.)
TEST(SpecRegression, RoMetadataReplaceMergesInsteadOfDroppingAbsentTags) {
    TwoRoleFixture fx;
    fx.connectAll();
    fx.device->onFrameReceived(
        Channel::Ro, encode("<mos>" + envelope("501") +
                            "<roCreate><roID>R</roID><roSlug>A</roSlug><roChannel>CH1</roChannel>"
                            "<roEdStart>2009-04-17T17:02:00</roEdStart>"
                            "<mosExternalMetadata><mosScope>PLAYLIST</mosScope>"
                            "<mosSchema>http://example.com/schema/a</mosSchema>"
                            "<mosPayload><k>1</k></mosPayload></mosExternalMetadata>"
                            "</roCreate></mos>"));
    fx.device->onFrameReceived(Channel::Ro,
                               encode("<mos>" + envelope("502") +
                                      "<roMetadataReplace><roID>R</roID><roSlug>B</roSlug>"
                                      "</roMetadataReplace></mos>"));

    const XmlNode* ro = fx.device->runningOrderModel().runningOrder("R");
    ASSERT_NE(ro, nullptr);
    EXPECT_EQ(ro->childText("roSlug"), "B");  // the tag that was sent is replaced
    EXPECT_EQ(ro->childText("roChannel"), "CH1");
    EXPECT_EQ(ro->childText("roEdStart"), "2009-04-17T17:02:00");
    EXPECT_NE(ro->firstChildElement("mosExternalMetadata"), nullptr);
}

// 6. Spec §6 time format: YYYY-MM-DD'T'hh:mm:ss[,ddd]['Z'] — the fraction
//    separator is a comma, and both of the field definition's examples must
//    parse. The spec is not consistent, though: its roItemCue examples carry
//    <roEventTime>2000-03-20T10:45:00.00</roEventTime>, so the dot form (at
//    any precision) has to keep parsing too, to the same instant.
TEST(SpecRegression, SpecTimeAcceptsCommaFractions) {
    const auto whole = imos::parseSpecTime("2009-04-11T14:22:07Z");
    ASSERT_TRUE(whole.ok());

    const auto zulu = imos::parseSpecTime("2009-04-11T14:22:07,125Z");
    ASSERT_TRUE(zulu.ok()) << "spec example 2009-04-11T14:22:07,125Z rejected";
    EXPECT_EQ(zulu.value().millisSinceEpoch, whole.value().millisSinceEpoch + 125);

    const auto offset = imos::parseSpecTime("2009-04-11T14:22:07,125-05:00");
    ASSERT_TRUE(offset.ok()) << "spec example 2009-04-11T14:22:07,125-05:00 rejected";
    EXPECT_EQ(offset.value().millisSinceEpoch,
              whole.value().millisSinceEpoch + 125 + 5 * 3'600'000);

    // The dot separator names the same instants.
    for (const char* dotted : {"2009-04-11T14:22:07.125Z", "2009-04-11T14:22:07.125-05:00"}) {
        const auto parsed = imos::parseSpecTime(dotted);
        ASSERT_TRUE(parsed.ok()) << dotted;
    }
    EXPECT_EQ(imos::parseSpecTime("2009-04-11T14:22:07.125Z").value().millisSinceEpoch,
              zulu.value().millisSinceEpoch);
    EXPECT_EQ(imos::parseSpecTime("2009-04-11T14:22:07.125-05:00").value().millisSinceEpoch,
              offset.value().millisSinceEpoch);

    // The roItemCue example's own value, and its comma twin.
    const auto cue = imos::parseSpecTime("2000-03-20T10:45:00.00");
    const auto cueComma = imos::parseSpecTime("2000-03-20T10:45:00,00");
    const auto cueWhole = imos::parseSpecTime("2000-03-20T10:45:00");
    ASSERT_TRUE(cue.ok());
    ASSERT_TRUE(cueComma.ok());
    ASSERT_TRUE(cueWhole.ok());
    EXPECT_EQ(cue.value().millisSinceEpoch, cueWhole.value().millisSinceEpoch);
    EXPECT_EQ(cueComma.value().millisSinceEpoch, cueWhole.value().millisSinceEpoch);

    // A separator with no digits after it is still malformed, either way.
    EXPECT_FALSE(imos::parseSpecTime("2009-04-11T14:22:07,Z").ok());
    EXPECT_FALSE(imos::parseSpecTime("2009-04-11T14:22:07.Z").ok());
}

// 7. Spec §3.9.1 note 5: after a leaseLock request the MOS "must send a
//    subsequent action message ... before the original leaseLock message has
//    expired". The lease holder is currently the request's messageID — new
//    for every request — so the same MOS is locked out of its own lease.
//    (incs/profile7_ncs.cpp: holder = event.messageId.)
TEST(SpecRegression, LeaseHolderCanSendItsFollowUpAction) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");

    const auto leasedMove = [&](const std::string& before) {
        imos::profiles::StoryActionRequest request;
        request.operation = imos::profiles::StoryAction::Move;
        request.roID = "RO1";
        request.storyIDs = {"s2"};
        request.beforeStoryID = before;
        request.leaseSeconds = 30;
        EXPECT_TRUE(fx.device->requestStoryAction(request).ok());
        fx.settle();
        return fx.device->nextStoryActionResult();
    };

    const std::optional<imos::profiles::StoryActionResult> first = leasedMove("s1");
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->ok) << first->roStatus;

    // Same MOS, still inside the 30 s lease.
    const std::optional<imos::profiles::StoryActionResult> second = leasedMove("s4");
    ASSERT_TRUE(second.has_value());
    EXPECT_TRUE(second->ok) << "the lease holder was refused: " << second->roStatus;
}

// 1b. The heartbeat is the one message that is its own reply, so a second
//     reply to a retried heartbeat is indistinguishable from a peer-initiated
//     one. It must be swallowed — not answered (the spec's "endless looping
//     condition", §2.1) and not recorded as peer traffic — while a genuinely
//     retried peer heartbeat is still answered every time.
TEST(SpecRegression, RetriedHeartbeatNeitherLoopsNorGoesUnanswered) {
    TwoRoleFixture fx;
    fx.connectAll();

    ASSERT_TRUE(fx.device->session()
                    .enqueue(Channel::Ro, imos::codec::makeMessage(MessageType::Heartbeat))
                    .ok());
    fx.clock.advanceSeconds(11);
    fx.device->poll();
    ASSERT_EQ(roQueue(fx.deviceTransport).size(), 2U);  // original + retry

    // Both copies reach the NCS; each is answered (a retry means the first
    // reply may have been lost).
    ASSERT_TRUE(fx.pump());
    ASSERT_EQ(roQueue(fx.ncsTransport).size(), 2U);
    // Both replies reach the device: one completes the exchange, the other
    // is swallowed — nothing goes back.
    ASSERT_TRUE(fx.pump());
    EXPECT_TRUE(roQueue(fx.deviceTransport).empty()) << "a late heartbeat reply was answered";
    EXPECT_TRUE(roQueue(fx.ncsTransport).empty());
    EXPECT_TRUE(fx.device->session().channelIdle(Channel::Ro));
}
