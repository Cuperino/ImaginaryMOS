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
#include <imos/profiles/object_query.h>
#include <imos/profiles/profile2.h>
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

std::deque<std::vector<std::uint8_t>>& queueOf(LoopbackEndpoint& endpoint, Channel channel) {
    return endpoint.outbound[static_cast<std::size_t>(channel)];
}

/// Parse a subtree out of a wrapper document.
XmlNode parseNode(const std::string& xml) {
    auto parsed = imos::codec::parseMessage("<mos>" + xml + "</mos>");
    EXPECT_TRUE(parsed.ok());
    return parsed.value().document().root.children.front();
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

// ===========================================================================
// SpecReview — findings from the same review that were first identified by
// reading the code against the specification, then confirmed by running the
// scenario. Each test names the spec text it asserts and the code that used
// to deviate from it.
// ===========================================================================

// A. Spec §2.3 note 5: "Message transmissions which do not receive a
//    response will be retried at intervals until a response is received."
//    After maxRetries the channel is marked faulted and — while the socket
//    stays up — never transmits again: no further retry, no heartbeat, no
//    keepAlive. Only a transport reconnect clears it, and nothing in the
//    library forces one. (core/session/peer_session.cpp, poll().)
TEST(SpecReview, UnansweredMessageKeepsBeingRetriedWhileConnected) {
    TwoRoleFixture fx;
    fx.connectAll();
    Message stat = imos::codec::makeMessage(MessageType::RoElementStat);
    stat.body()->setAttribute("element", "RO");
    stat.body()->appendElement("roID").setText("ROX");
    stat.body()->appendElement("status").setText("READY");
    stat.body()->appendElement("time").setText("2026-01-01T00:00:00");
    ASSERT_TRUE(fx.device->session().enqueue(std::move(stat)).ok());

    // The NCS is up but silent: let the capped retries lapse.
    for (int i = 0; i < 6; ++i) {
        fx.clock.advanceSeconds(11);
        fx.device->poll();
    }
    const std::size_t sentAtFault = roQueue(fx.deviceTransport).size();
    ASSERT_GE(sentAtFault, 2U);

    // The connection never dropped. Ten more minutes of polling must show at
    // least one further attempt.
    for (int i = 0; i < 60; ++i) {
        fx.clock.advanceSeconds(10);
        fx.device->poll();
    }
    EXPECT_GT(roQueue(fx.deviceTransport).size(), sentAtFault)
        << "the channel went permanently silent after the retry cap";
}

// B. Spec §2.2: for a mosObj with status UPDATED, "if metadata tags exist in
//    the target MOS Object and are not present in the mosObj UPDATED message,
//    the metadata tags ... should be left intact". The NCS directory replaces
//    the stored object wholesale. (core/profiles/profile1.cpp,
//    ObjectDirectory::ingest.)
TEST(SpecReview, MosObjUpdatedLeavesAbsentTagsIntact) {
    TwoRoleFixture fx;
    fx.connectAll();
    const auto announce = [&](const std::string& bodyXml) {
        Message object = imos::codec::makeMessage(MessageType::MosObj);
        *object.body() = parseNode(bodyXml);
        ASSERT_TRUE(fx.device->announceObject(std::move(object)).ok());
        fx.settle();
    };
    announce(
        "<mosObj><objID>M1</objID><objSlug>First slug</objSlug>"
        "<mosAbstract>An abstract</mosAbstract><objType>VIDEO</objType>"
        "<objTB>50</objTB><objRev>1</objRev><objDur>250</objDur><status>NEW</status>"
        "<objAir>READY</objAir><description>Searchable description</description>"
        "</mosObj>");
    ASSERT_NE(fx.ncs->object("M1"), nullptr);

    announce(
        "<mosObj><objID>M1</objID><objSlug>Second slug</objSlug>"
        "<objRev>2</objRev><status>UPDATED</status></mosObj>");
    const Message* stored = fx.ncs->object("M1");
    ASSERT_NE(stored, nullptr);
    const XmlNode* body = stored->body();
    ASSERT_NE(body, nullptr);
    EXPECT_EQ(body->childText("objSlug"), "Second slug");
    EXPECT_EQ(body->childText("mosAbstract"), "An abstract");
    EXPECT_EQ(body->childText("objDur"), "250");
    EXPECT_EQ(body->childText("description"), "Searchable description");
}

// E. Spec §2.1: every message names its mosID/ncsID pair and "the MOS and
//    NCS character names are predefined in each system". A message addressed
//    to a different pairing is not this relationship's traffic, yet the
//    session applies it. (No identity check exists anywhere in core/.)
TEST(SpecReview, MessageForADifferentPairingIsNotApplied) {
    TwoRoleFixture fx;
    fx.connectAll();
    fx.device->onFrameReceived(Channel::Ro,
                               encode("<mos><mosID>some.other.device.mos</mosID>"
                                      "<ncsID>some.other.ncs</ncsID><messageID>41</messageID>"
                                      "<roCreate><roID>FOREIGN</roID><roSlug>Not ours</roSlug>"
                                      "</roCreate></mos>"));
    EXPECT_FALSE(fx.device->runningOrderModel().has("FOREIGN"))
        << "a roCreate addressed to another mosID/ncsID pair was applied";
}

// F1. Spec §3.9.1: for NEW, element_source holds "one or more stories to be
//     inserted". Only the last one is created; the roAck still reports
//     success. (incs/profile7_ncs.cpp, the NEW payload loop.)
TEST(SpecReview, StoryActionNewInsertsEveryStory) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");
    fx.ncs->onFrameReceived(
        Channel::Ro, encode("<mos>" + envelope("610") +
                            "<roReqStoryAction operation=\"NEW\"><roStorySend><roID>RO1</roID>"
                            "<element_target><storyID>s1</storyID></element_target>"
                            "<element_source>"
                            "<story><storyID>n1</storyID><storySlug>one</storySlug></story>"
                            "<story><storyID>n2</storyID><storySlug>two</storySlug></story>"
                            "</element_source></roStorySend></roReqStoryAction></mos>"));
    const std::vector<std::string> stories = fx.ncs->author().desired().storyIDs("RO1");
    EXPECT_EQ(stories, (std::vector<std::string>{"n1", "n2", "s1", "s2", "s3", "s4"}));
}

// F2. Spec §3.9.1: UPDATE "replaces an existing Story". The replacement the
//     NCS builds is header-only, so the story's item references vanish from
//     the running order even when the MOS's own body still embeds them.
//     (incs/profile7_ncs.cpp, applyUpdate: makeStory(id, slug, num).)
TEST(SpecReview, StoryActionUpdateKeepsItemsTheBodyStillReferences) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");
    ASSERT_EQ(fx.ncs->author().desired().itemIDs("RO1", "s1").size(), 2U);

    imos::profiles::StoryActionRequest request;
    request.operation = imos::profiles::StoryAction::Update;
    request.roID = "RO1";
    request.storyID = "s1";
    request.storySlug = "slug";
    request.storyBody = parseNode(std::string{"<storyBody><p>Intro</p>"} +
                                  "<storyItem><itemID>s1-i0</itemID><objID>OBJ0</objID>"
                                  "<mosID>" +
                                  TwoRoleFixture::kMosID +
                                  "</mosID></storyItem><p>Middle</p>"
                                  "<storyItem><itemID>s1-i1</itemID><objID>OBJ1</objID><mosID>" +
                                  TwoRoleFixture::kMosID + "</mosID></storyItem></storyBody>");
    ASSERT_TRUE(fx.device->requestStoryAction(request).ok());
    fx.settle();
    const std::optional<imos::profiles::StoryActionResult> result =
        fx.device->nextStoryActionResult();
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->ok) << result->roStatus;

    EXPECT_EQ(fx.ncs->author().desired().itemIDs("RO1", "s1"),
              (std::vector<std::string>{"s1-i0", "s1-i1"}))
        << "the UPDATE stripped the story's items from the running order";
}

// G. Spec §2.3 "Important Note": once the device's list no longer matches
//    what the NCS sent, it sends roElementStat DISCONNECTED "and ACK[s] all
//    subsequent 'ro' messages with a status of DISCONNECTED". In model mode
//    the acks are generated by the library and always say OK.
TEST(SpecReview, DisconnectedRunningOrderAcksWithDisconnected) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");

    imos::profiles::ElementStatusReport report;
    report.element = imos::profiles::RoElement::RunningOrder;
    report.roID = "RO1";
    report.status = "DISCONNECTED";
    ASSERT_TRUE(fx.device->reportElementStatus(report).ok());
    fx.settle();

    ASSERT_TRUE(fx.ncs->author().moveStory("RO1", "s4", "s1").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();
    const std::vector<incs::CommitEvent> commits = drainCommits(*fx.ncs);
    ASSERT_EQ(commits.size(), 1U);
    EXPECT_EQ(commits[0].roStatus, "DISCONNECTED");
}

// H1. Store sequence numbers are per channel, so a mom send and a ro send
//     can share one. Profile5NcsResponder keys its pending roCtrl sends by
//     seq alone, so a delivery failure on mom resolves an unrelated roCtrl
//     as failed — and the roCtrl's real ack then matches nothing.
//     (core/profiles/profile5.cpp, Profile5NcsResponder::handle.)
TEST(SpecReview, MomDeliveryFailureDoesNotResolveARoCtrl) {
    TwoRoleFixture fx;
    fx.connectAll();

    // mom seq 1: a mosReqObj the device never answers. Three of its four
    // transmissions go by (poll() releases one retry per timeout).
    ASSERT_TRUE(fx.ncs->requestObject("M1").ok());
    for (int i = 0; i < 3; ++i) {
        fx.clock.advanceSeconds(11);
        fx.ncs->poll();
    }

    // ro seq 1: a roCtrl, sent late enough to still have retries in hand
    // when the mom exchange gives up on the next timeout.
    ASSERT_TRUE(fx.ncs
                    ->sendCtrl(imos::profiles::CtrlScope::RunningOrder, "RO1", "", "",
                               imos::profiles::CtrlCommand::Ready)
                    .ok());
    fx.clock.advanceSeconds(11);
    fx.ncs->poll();
    ASSERT_FALSE(fx.ncs->session().channelIdle(Channel::Ro));  // roCtrl still in flight
    const std::optional<imos::profiles::CtrlResult> early = fx.ncs->nextCtrlResult();
    EXPECT_FALSE(early.has_value())
        << "a roCtrl still in flight was resolved as '" << (early ? early->roStatus : "") << "'";
}

// H2. A story deleted from the running order (or a deleted RO) leaves its
//     roStorySend body in the device's story store forever.
//     (core/profiles/profile4.cpp: eraseStory/eraseStories are never called.)
TEST(SpecReview, DeletedStoryIsDroppedFromTheStoryStore) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");
    ASSERT_TRUE(fx.ncs->author()
                    .setStoryBody("RO1", "s2", parseNode("<storyBody><p>Script</p></storyBody>"))
                    .ok());
    ASSERT_TRUE(fx.ncs->sendStory("RO1", "s2").ok());
    fx.settle();
    ASSERT_NE(fx.device->story("RO1", "s2"), nullptr);

    ASSERT_TRUE(fx.ncs->author().removeStory("RO1", "s2").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();
    ASSERT_EQ(fx.device->runningOrderModel().story("RO1", "s2"), nullptr);
    EXPECT_EQ(fx.device->story("RO1", "s2"), nullptr)
        << "the body of a story no longer in the running order is still held";
}

// H3. peer_session.h documents listMachInfo as a terminal reply that never
//     waits behind this side's own in-flight request. Profile0Responder
//     queues it with enqueue(), so it does wait.
//     (core/profiles/profile0.cpp, handle().)
TEST(SpecReview, ListMachInfoIsNotHeldBehindAnInFlightRequest) {
    TwoRoleFixture fx;
    fx.connectAll();
    Message stat = imos::codec::makeMessage(MessageType::RoElementStat);
    stat.body()->setAttribute("element", "RO");
    stat.body()->appendElement("roID").setText("ROX");
    stat.body()->appendElement("status").setText("READY");
    stat.body()->appendElement("time").setText("2026-01-01T00:00:00");
    ASSERT_TRUE(fx.device->session().enqueue(std::move(stat)).ok());
    roQueue(fx.deviceTransport).clear();  // lost: the device stays in flight

    ASSERT_TRUE(fx.ncs->requestMachineInfo(Channel::Ro).ok());
    ASSERT_TRUE(fx.pump());  // reqMachInfo reaches the device
    ASSERT_EQ(roQueue(fx.deviceTransport).size(), 1U)
        << "listMachInfo is queued behind the device's unacknowledged message";
    EXPECT_EQ(parseFrame(roQueue(fx.deviceTransport).front()).type(), MessageType::ListMachInfo);
}

// H4. Spec §3.2.5: a null listReturnTotal "indicates generally more than one
//     object has been found" — it is not zero. The directory's paged resync
//     reads it as 0 and stops after the first page.
//     (core/profiles/profile3.cpp, ObjectDirectory::completeQuery.)
TEST(SpecReview, NullListReturnTotalDoesNotEndDirectoryResync) {
    TwoRoleFixture fx;
    fx.connectAll();

    // Teach the NCS that its peer supports Profile 3.
    ASSERT_TRUE(fx.ncs->requestMachineInfo(Channel::Mom).ok());
    ASSERT_EQ(queueOf(fx.ncsTransport, Channel::Mom).size(), 1U);
    const Message req = parseFrame(queueOf(fx.ncsTransport, Channel::Mom).front());
    queueOf(fx.ncsTransport, Channel::Mom).clear();
    std::string profiles;
    for (int n = 0; n < 8; ++n) {
        profiles += "<mosProfile number=\"" + std::to_string(n) + "\">YES</mosProfile>";
    }
    fx.ncs->onFrameReceived(
        Channel::Mom, encode("<mos>" + envelope(std::string{*req.messageID()}) +
                             "<listMachInfo><manufacturer>X</manufacturer><model>Y</model><hwRev/>"
                             "<swRev>1</swRev><DOM/><SN/><ID>device.example.mos</ID>"
                             "<time>2026-01-01T00:00:00</time><mosRev>4.0</mosRev>"
                             "<supportedProfiles deviceType=\"MOS\">" +
                             profiles + "</supportedProfiles></listMachInfo></mos>"));

    ASSERT_TRUE(fx.ncs->resyncDirectory(2).ok());
    ASSERT_EQ(queueOf(fx.ncsTransport, Channel::Aux).size(), 1U);
    const Message query = parseFrame(queueOf(fx.ncsTransport, Channel::Aux).front());
    ASSERT_EQ(query.type(), MessageType::MosReqObjList);
    queueOf(fx.ncsTransport, Channel::Aux).clear();

    // First page: two objects, total unknown (null).
    fx.ncs->onFrameReceived(Channel::Aux,
                            encode("<mos>" + envelope(std::string{*query.messageID()}) +
                                   "<mosObjList><queryID>" + query.body()->childText("queryID") +
                                   "</queryID><listReturnStart>1</listReturnStart>"
                                   "<listReturnEnd>2</listReturnEnd><listReturnTotal/>"
                                   "<list><mosObj><objID>A</objID><objSlug>a</objSlug></mosObj>"
                                   "<mosObj><objID>B</objID><objSlug>b</objSlug></mosObj></list>"
                                   "</mosObjList></mos>"));
    ASSERT_EQ(queueOf(fx.ncsTransport, Channel::Aux).size(), 1U)
        << "the resync stopped after one page although the total was unknown";
    const Message next = parseFrame(queueOf(fx.ncsTransport, Channel::Aux).front());
    EXPECT_EQ(next.body()->childText("listReturnStart"), "3");
}

// H5. A numeric literal too large for a double is infinity; XPath's
//     string() of it is "Infinity". The evaluator converts the value through
//     long long first — undefined behaviour for infinity — and produces a
//     garbage integer. (core/profiles/object_query.cpp, operandString.)
TEST(SpecReview, HugeXPathNumberLiteralIsNotCastThroughAnInteger) {
    const auto field = imos::profiles::parseFieldXPath("/f[. = string(1e400)]");
    ASSERT_TRUE(field.ok()) << field.error().message;
    const XmlNode payload = parseNode("<mosPayload><f>Infinity</f></mosPayload>");
    EXPECT_TRUE(imos::profiles::evaluateField(field.value(), payload));
}

// H6. Both sides originate a heartbeat on the same channel at the same
//     moment (spec §2.1: answer a heartbeat with a heartbeat, "avoid an
//     endless looping condition"). The peer's heartbeat arrives while ours
//     is in flight, is taken for a reply with the wrong messageID, and is
//     dropped as a ProtocolError — neither side is answered until a retry.
//     (core/session/peer_session.cpp, handleInbound correlation.)
TEST(SpecReview, CrossingHeartbeatsAreBothAnswered) {
    TwoRoleFixture fx;
    fx.connectAll();
    Message fromDevice = imos::codec::makeMessage(MessageType::Heartbeat);
    fromDevice.setMessageID("101");
    Message fromNcs = imos::codec::makeMessage(MessageType::Heartbeat);
    fromNcs.setMessageID("202");
    ASSERT_TRUE(fx.device->session().enqueue(Channel::Ro, std::move(fromDevice)).ok());
    ASSERT_TRUE(fx.ncs->session().enqueue(Channel::Ro, std::move(fromNcs)).ok());
    fx.settle();  // no clock advance: no retry may be needed

    EXPECT_TRUE(fx.device->session().channelIdle(Channel::Ro));
    EXPECT_TRUE(fx.ncs->session().channelIdle(Channel::Ro));
    int protocolErrors = 0;
    while (std::optional<imos::SessionEvent> event = fx.device->nextEvent()) {
        protocolErrors += event->type == imos::SessionEventType::ProtocolError ? 1 : 0;
    }
    while (std::optional<imos::SessionEvent> event = fx.ncs->nextEvent()) {
        protocolErrors += event->type == imos::SessionEventType::ProtocolError ? 1 : 0;
    }
    EXPECT_EQ(protocolErrors, 0);
}

// I1. Spec §4.1.6: "the last used messageID must be persistent." A restarted
//     session over the same store starts numbering at 1 again, re-using IDs
//     the peer's dedupe window may still hold.
//     (core/session/peer_session.cpp: nextMessageId_ is never stored.)
TEST(SpecReview, MessageIdCounterSurvivesARestart) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");
    const auto sendStatus = [&]() {
        imos::profiles::ElementStatusReport report;
        report.element = imos::profiles::RoElement::RunningOrder;
        report.roID = "RO1";
        report.status = "READY";
        EXPECT_TRUE(fx.device->reportElementStatus(report).ok());
        EXPECT_EQ(roQueue(fx.deviceTransport).size(), 1U);
        const std::string id{parseFrame(roQueue(fx.deviceTransport).front()).messageID().value()};
        fx.settle();
        return std::stoll(id);
    };
    const long long before = sendStatus();

    // Restart the NCS-facing device session over the same store; rebuild its
    // model through the NCS so it can report status again.
    fx.device = std::make_unique<imos::Device>(
        imos::DeviceConfig{TwoRoleFixture::sessionConfig(),
                           TwoRoleFixture::machineInfo(imos::profiles::DeviceType::Mos)},
        imos::PeerSession::Dependencies{&fx.clock, &fx.deviceTransport, &fx.deviceStore, nullptr,
                                        &fx.random, nullptr});
    for (std::size_t i = 0; i < imos::kChannelCount; ++i) {
        fx.device->onChannelConnected(static_cast<Channel>(i));
    }
    // A fresh message from the restarted session.
    Message stat = imos::codec::makeMessage(MessageType::RoElementStat);
    stat.body()->setAttribute("element", "RO");
    stat.body()->appendElement("roID").setText("RO1");
    stat.body()->appendElement("status").setText("READY");
    stat.body()->appendElement("time").setText("2026-01-01T00:00:00");
    ASSERT_TRUE(fx.device->session().enqueue(std::move(stat)).ok());
    ASSERT_EQ(roQueue(fx.deviceTransport).size(), 1U);
    const long long after = std::stoll(
        std::string{parseFrame(roQueue(fx.deviceTransport).front()).messageID().value()});
    EXPECT_GT(after, before) << "the restarted session re-used messageID " << after;
}

// I2. A retried command is answered from the dedupe window with a fabricated
//     "OK" — even when the original was refused. The retry must get the same
//     verdict the first copy got. (core/session/peer_session.cpp, duplicate
//     branch.)
TEST(SpecReview, RetryOfARefusedCommandIsNotAckedOk) {
    TwoRoleFixture fx;
    fx.connectAll();
    const std::string action =
        "<mos>" + envelope("950") +
        "<roElementAction operation=\"DELETE\"><roID>NOPE</roID>"
        "<element_source><storyID>s1</storyID></element_source></roElementAction></mos>";
    const auto ackOf = [&]() {
        for (const auto& frame : roQueue(fx.deviceTransport)) {
            const Message message = parseFrame(frame);
            if (message.type() == MessageType::RoAck) {
                return message;
            }
        }
        ADD_FAILURE() << "no roAck";
        return imos::codec::makeMessage(MessageType::RoAck);
    };
    fx.device->onFrameReceived(Channel::Ro, encode(action));
    ASSERT_TRUE(imos::profiles::roAckIsNack(ackOf()));  // unknown roID → refused
    roQueue(fx.deviceTransport).clear();

    fx.device->onFrameReceived(Channel::Ro, encode(action));  // the NCS retries
    EXPECT_TRUE(imos::profiles::roAckIsNack(ackOf()))
        << "the retry of a refused command was acknowledged as OK";
}
