// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 2 — Basic Running Order / Content List Workflow conformance
// (plan §3.2 row 2, §10, M3). A real imos::Device against a real incs::Ncs
// over the loopback pair: the authoring/diff walk (author, commit, device
// model converges after every mutation), diff quality (minimal message
// sequences: MOVE not DELETE+INSERT, SWAP for transpositions, header-only →
// roMetadataReplace, threshold → roReplace), roReadyToAir, bidirectional
// roElementStat, the §3.3 resync heuristic (rebuild-from-roList and the
// NACK branch), store-backed resume, legacy-13 receive normalization, and
// the negative branches (wrong-state NACK, oversized RO, per-element NACK
// statuses propagated to the commit result).

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/gen/views.h>
#include <imos/codec/message.h>
#include <imos/encoding/encoding.h>
#include <imos/profiles/profile2.h>
#include <imos/store/file_store.h>

#include "two_role_fixture.h"

using imos::Channel;
using imos::codec::Message;
using imos::codec::MessageType;
using imos::codec::XmlNode;
using imos::profiles::ElementStatusReport;
using imos::profiles::RoElement;
using imos::test::FakeClock;
using imos::test::FakeRandom;
using imos::test::LoopbackEndpoint;
using imos::test::TwoRoleFixture;
using incs::RunningOrderAuthor;

namespace {

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

/// A story subtree with `count` items ("<storyID>-i<n>" itemIDs).
XmlNode storyWithItems(const std::string& storyID, int count, const std::string& slug = "slug") {
    XmlNode story = RunningOrderAuthor::makeStory(storyID, slug);
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

int countResyncEvents(imos::Device& device) {
    int count = 0;
    while (std::optional<imos::SessionEvent> event = device.nextEvent()) {
        count += event->type == imos::SessionEventType::ResyncNeeded ? 1 : 0;
    }
    return count;
}

/// Rebuild the fixture's device from the same dependencies — the "device
/// restarted and lost its (volatile) shadow model" scenario that makes the
/// spec's resync heuristic fire. The session store survives (dedupe window,
/// pending queue); the RunningOrderModel does not.
void restartDevice(TwoRoleFixture& fx) {
    fx.device = std::make_unique<imos::Device>(
        imos::DeviceConfig{TwoRoleFixture::sessionConfig(),
                           TwoRoleFixture::machineInfo(imos::profiles::DeviceType::Mos)},
        imos::PeerSession::Dependencies{&fx.clock, &fx.deviceTransport, &fx.deviceStore, nullptr,
                                        &fx.random, nullptr});
    for (std::size_t i = 0; i < imos::kChannelCount; ++i) {
        fx.device->onChannelConnected(static_cast<Channel>(i));
    }
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

// 1. Authoring walk: create → commit is a single roCreate; the device model
//    converges byte-fully and both ro channels come to rest.
TEST(Profile2, CreateCommitConvergesWithSingleRoCreate) {
    TwoRoleFixture fx;
    fx.connectAll();

    ASSERT_TRUE(fx.ncs->author().create("RO1", "5PM Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", storyWithItems("s1", 2, "Hotel Fire")).ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", storyWithItems("s2", 1, "Weather")).ok());

    imos::Result<std::vector<imos::Seq>> seqs = fx.ncs->author().commit("RO1");
    ASSERT_TRUE(seqs.ok());
    ASSERT_EQ(seqs.value().size(), 1U);
    ASSERT_EQ(roQueue(fx.ncsTransport).size(), 1U);
    EXPECT_EQ(parseFrame(roQueue(fx.ncsTransport)[0]).type(), MessageType::RoCreate);

    fx.settle();
    const imos::profiles::RunningOrderModel& model = fx.device->runningOrderModel();
    EXPECT_EQ(model.storyIDs("RO1"), (std::vector<std::string>{"s1", "s2"}));
    EXPECT_EQ(model.itemIDs("RO1", "s1"), (std::vector<std::string>{"s1-i0", "s1-i1"}));

    const std::vector<incs::CommitEvent> commits = drainCommits(*fx.ncs);
    ASSERT_EQ(commits.size(), 1U);
    EXPECT_TRUE(commits[0].ok);
    EXPECT_EQ(fx.ncs->author().pendingCount(), 0U);
    EXPECT_TRUE(fx.device->session().channelIdle(Channel::Ro));
    EXPECT_TRUE(fx.ncs->session().channelIdle(Channel::Ro));

    // An uncommitted-then-committed no-op diff sends nothing.
    imos::Result<std::vector<imos::Seq>> noop = fx.ncs->author().commit("RO1");
    ASSERT_TRUE(noop.ok());
    EXPECT_TRUE(noop.value().empty());
}

// 2. Every mutation kind converges after its commit+ack round.
TEST(Profile2, MutationsConvergeAfterEachCommit) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");
    const auto& model = fx.device->runningOrderModel();

    const auto commitAndSettle = [&] {
        ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
        fx.settle();
        EXPECT_EQ(model.storyIDs("RO1"), fx.ncs->author().desired().storyIDs("RO1"));
        for (const std::string& storyID : model.storyIDs("RO1")) {
            EXPECT_EQ(model.itemIDs("RO1", storyID),
                      fx.ncs->author().desired().itemIDs("RO1", storyID));
        }
        for (const incs::CommitEvent& event : drainCommits(*fx.ncs)) {
            EXPECT_TRUE(event.ok) << event.roStatus;
        }
    };

    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", storyWithItems("s0", 1), "s1").ok());
    commitAndSettle();  // head insert
    ASSERT_TRUE(fx.ncs->author().moveStory("RO1", "s3", "s0").ok());
    commitAndSettle();  // move to head
    ASSERT_TRUE(fx.ncs->author().swapStories("RO1", "s1", "s2").ok());
    commitAndSettle();  // adjacent swap
    ASSERT_TRUE(fx.ncs->author().removeStory("RO1", "s4").ok());
    commitAndSettle();  // delete
    ASSERT_TRUE(fx.ncs->author().replaceStory("RO1", "s2", storyWithItems("s2", 3, "v2")).ok());
    commitAndSettle();  // replace with a changed item set
    ASSERT_TRUE(
        fx.ncs->author()
            .insertItem("RO1", "s1", RunningOrderAuthor::makeItem("late", "OBJX", "m"), "s1-i1")
            .ok());
    commitAndSettle();  // item insert
    ASSERT_TRUE(fx.ncs->author().moveItem("RO1", "s1", "s1-i1").ok());
    commitAndSettle();  // item move to tail
    ASSERT_TRUE(fx.ncs->author().removeItem("RO1", "s1", "s1-i0").ok());
    commitAndSettle();  // item delete

    ASSERT_TRUE(fx.ncs->author().destroy("RO1").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();
    EXPECT_FALSE(model.has("RO1"));  // final roDelete (the profile's workflow end)
}

// 3. Diff quality: specific edits produce specific minimal sequences.
TEST(Profile2, DiffProducesMinimalSpecSequences) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");

    const auto singleMessage = [&](MessageType type, const std::string& operation) -> void {
        imos::Result<std::vector<imos::Seq>> seqs = fx.ncs->author().commit("RO1");
        ASSERT_TRUE(seqs.ok());
        ASSERT_EQ(seqs.value().size(), 1U);
        ASSERT_EQ(roQueue(fx.ncsTransport).size(), 1U);
        Message sent = parseFrame(roQueue(fx.ncsTransport)[0]);
        EXPECT_EQ(sent.type(), type);
        if (!operation.empty()) {
            EXPECT_EQ(sent.body()->attribute("operation").value_or(""), operation);
        }
        fx.settle();
        EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"),
                  fx.ncs->author().desired().storyIDs("RO1"));
    };

    // Header-only edit → roMetadataReplace, nothing else.
    ASSERT_TRUE(fx.ncs->author().setHeaderField("RO1", "roSlug", "6PM Rundown").ok());
    singleMessage(MessageType::RoMetadataReplace, "");

    // Reorder → one MOVE; never DELETE+INSERT.
    ASSERT_TRUE(fx.ncs->author().moveStory("RO1", "s4", "s2").ok());
    singleMessage(MessageType::RoElementAction, "MOVE");

    // Pure transposition → one SWAP.
    ASSERT_TRUE(fx.ncs->author().swapStories("RO1", "s1", "s3").ok());
    singleMessage(MessageType::RoElementAction, "SWAP");

    // Delta past the threshold — four content replacements plus three
    // non-adjacent inserts is 7 messages > 6 — → one wholesale roReplace.
    for (const char* id : {"s1", "s2", "s3", "s4"}) {
        ASSERT_TRUE(
            fx.ncs->author().replaceStory("RO1", id, storyWithItems(id, 1, "rewrite")).ok());
    }
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", storyWithItems("xA", 1), "s1").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", storyWithItems("xB", 1), "s2").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", storyWithItems("xC", 1), "s4").ok());
    singleMessage(MessageType::RoReplace, "");
}

// 4. roReadyToAir propagates the readiness flag.
TEST(Profile2, ReadyToAirReachesTheModel) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");

    ASSERT_TRUE(fx.ncs->author().setReadyToAir("RO1", "READY").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();
    EXPECT_EQ(fx.device->runningOrderModel().readyToAir("RO1").value_or(""), "READY");

    ASSERT_TRUE(fx.ncs->author().setReadyToAir("RO1", "NOT READY").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();
    EXPECT_EQ(fx.device->runningOrderModel().readyToAir("RO1").value_or(""), "NOT READY");
}

// 5. roElementStat flows both directions (2.8.5+ bidirectional semantics).
TEST(Profile2, ElementStatusRoundTripsBothDirections) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");

    // Device → NCS: a playback status with the clock-stamped time.
    ElementStatusReport play;
    play.element = RoElement::Item;
    play.roID = "RO1";
    play.storyID = "s1";
    play.itemID = "s1-i0";
    play.status = "PLAY";
    ASSERT_TRUE(fx.device->reportElementStatus(play).ok());
    fx.settle();
    std::optional<ElementStatusReport> seen = fx.ncs->nextElementStatus();
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(seen->element, RoElement::Item);
    EXPECT_EQ(seen->itemID, "s1-i0");
    EXPECT_EQ(seen->status, "PLAY");
    EXPECT_FALSE(seen->time.empty());
    EXPECT_EQ(fx.device->runningOrderModel().status(RoElement::Item, "RO1", "s1", "s1-i0"),
              std::optional<std::string>{"PLAY"});
    EXPECT_TRUE(fx.device->session().channelIdle(Channel::Ro));  // its roAck arrived

    // A status for an element the device does not hold is rejected locally.
    ElementStatusReport bogus = play;
    bogus.itemID = "nope";
    EXPECT_FALSE(fx.device->reportElementStatus(bogus).ok());

    // NCS → device: the status lands in the device model and surfaces.
    ElementStatusReport stop;
    stop.element = RoElement::Story;
    stop.roID = "RO1";
    stop.storyID = "s2";
    stop.status = "STOP";
    ASSERT_TRUE(fx.ncs->sendElementStat(stop).ok());
    fx.settle();
    EXPECT_EQ(fx.device->runningOrderModel().status(RoElement::Story, "RO1", "s2", ""),
              std::optional<std::string>{"STOP"});
    std::optional<ElementStatusReport> deviceSeen = fx.device->nextElementStatus();
    ASSERT_TRUE(deviceSeen.has_value());
    EXPECT_EQ(deviceSeen->status, "STOP");
    EXPECT_TRUE(fx.ncs->session().channelIdle(Channel::Ro));
}

// 6. Resync (§3.3): a device that lost its shadow state sees actions for an
//    unknown roID → exactly one ResyncNeeded, one roReq/roList rebuild, and
//    subsequent actions apply cleanly. Two commits are in flight during the
//    rebuild, so the roAck replies must cross the device's own outstanding
//    roReq — the reply-bypass path — without deadlocking.
TEST(Profile2, UnknownIdTriggersSingleResyncAndRebuild) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");

    restartDevice(fx);  // shadow model gone; session store survives
    EXPECT_FALSE(fx.device->runningOrderModel().has("RO1"));

    // Two separate commits: the first roElementAction meets an unknown roID;
    // the second is already queued while the device resyncs.
    ASSERT_TRUE(fx.ncs->author().moveStory("RO1", "s4", "s1").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    ASSERT_TRUE(fx.ncs->author().swapStories("RO1", "s2", "s3").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();

    EXPECT_EQ(countResyncEvents(*fx.device), 1);  // one state, not a stream
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"),
              fx.ncs->author().desired().storyIDs("RO1"));
    EXPECT_TRUE(fx.device->session().channelIdle(Channel::Ro));
    EXPECT_TRUE(fx.ncs->session().channelIdle(Channel::Ro));

    // The confused actions were NACKed; the rebuild answered them anyway.
    const std::vector<incs::CommitEvent> commits = drainCommits(*fx.ncs);
    ASSERT_EQ(commits.size(), 2U);
    EXPECT_FALSE(commits[0].ok);
    EXPECT_TRUE(commits[0].roStatus.starts_with("NACK"));

    // After the rebuild, the next mutation applies cleanly, no new resync.
    ASSERT_TRUE(fx.ncs->author().removeStory("RO1", "s4").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();
    EXPECT_EQ(countResyncEvents(*fx.device), 0);
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"),
              fx.ncs->author().desired().storyIDs("RO1"));
    for (const incs::CommitEvent& event : drainCommits(*fx.ncs)) {
        EXPECT_TRUE(event.ok) << event.roStatus;
    }
}

// 6b. Resync NACK branch: the roReq names an RO the NCS no longer has → a
//     NACK roAck drops it on the device without looping.
TEST(Profile2, ResyncNackBranchDropsTheRunningOrder) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO2");

    restartDevice(fx);
    ASSERT_TRUE(fx.ncs->author().destroy("RO2").ok());  // gone from desired, uncommitted

    // The NCS still references RO2: its status update meets the restarted
    // device → NACK + roReq → the NCS answers NACK (unknown RO) → dropped.
    ElementStatusReport stat;
    stat.element = RoElement::RunningOrder;
    stat.roID = "RO2";
    stat.status = "STANDBY";
    ASSERT_TRUE(fx.ncs->sendElementStat(stat).ok());
    fx.settle();

    EXPECT_EQ(countResyncEvents(*fx.device), 1);
    EXPECT_FALSE(fx.device->runningOrderModel().has("RO2"));
    EXPECT_TRUE(fx.device->session().channelIdle(Channel::Ro));  // no roReq loop
    EXPECT_TRUE(fx.ncs->session().channelIdle(Channel::Ro));
}

// 7. Store-backed resume (M3 exit criterion): the NCS crashes with one
//    roElementAction unacked and another construction message queued;
//    rebuilt from the FileMessageStore, the sequence resumes exactly once
//    and the device model converges.
TEST(Profile2, StoreBackedResumeReplaysConstructionExactlyOnce) {
    FakeClock clock;
    FakeRandom random;
    const std::filesystem::path root =
        std::filesystem::path{::testing::TempDir()} / "imos-p2-resume";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    imos::store::FileStoreOptions options;
    options.root = root;
    const imos::SessionConfig config = TwoRoleFixture::sessionConfig();

    LoopbackEndpoint deviceTx;
    LoopbackEndpoint ncsTx;
    imos::InMemoryMessageStore deviceStore;
    imos::Device device{
        imos::DeviceConfig{config, TwoRoleFixture::machineInfo(imos::profiles::DeviceType::Mos)},
        imos::PeerSession::Dependencies{&clock, &deviceTx, &deviceStore, nullptr, &random,
                                        nullptr}};
    for (std::size_t i = 0; i < imos::kChannelCount; ++i) {
        deviceTx.connected[i] = true;
        ncsTx.connected[i] = true;
        device.onChannelConnected(static_cast<Channel>(i));
    }

    int actionsDelivered = 0;
    const auto pumpWith = [&](incs::Ncs& ncs, int rounds) {
        for (int round = 0; round < rounds; ++round) {
            device.poll();
            ncs.poll();
            for (std::size_t i = 0; i < imos::kChannelCount; ++i) {
                const auto channel = static_cast<Channel>(i);
                std::deque<std::vector<std::uint8_t>> toNcs;
                std::deque<std::vector<std::uint8_t>> toDevice;
                toNcs.swap(deviceTx.outbound[i]);
                toDevice.swap(ncsTx.outbound[i]);
                for (const auto& frame : toNcs) {
                    ncs.onFrameReceived(channel, frame);
                }
                for (const auto& frame : toDevice) {
                    actionsDelivered +=
                        parseFrame(frame).type() == MessageType::RoElementAction ? 1 : 0;
                    device.onFrameReceived(channel, frame);
                }
            }
        }
    };

    // First incarnation: converge the RO, then commit two mutations and
    // crash before anything is delivered.
    {
        imos::store::FileMessageStore ncsStore{options};
        incs::Ncs ncs{
            incs::NcsConfig{config, TwoRoleFixture::machineInfo(imos::profiles::DeviceType::Ncs)},
            imos::PeerSession::Dependencies{&clock, &ncsTx, &ncsStore, nullptr, &random, nullptr}};
        for (std::size_t i = 0; i < imos::kChannelCount; ++i) {
            ncs.onChannelConnected(static_cast<Channel>(i));
        }
        ASSERT_TRUE(ncs.author().create("RO1", "Rundown").ok());
        for (const char* id : {"s1", "s2", "s3"}) {
            ASSERT_TRUE(ncs.author().insertStory("RO1", storyWithItems(id, 1)).ok());
        }
        ASSERT_TRUE(ncs.author().commit("RO1").ok());
        pumpWith(ncs, 8);
        ASSERT_EQ(device.runningOrderModel().storyIDs("RO1"),
                  (std::vector<std::string>{"s1", "s2", "s3"}));

        ASSERT_TRUE(ncs.author().moveStory("RO1", "s3", "s1").ok());
        ASSERT_TRUE(ncs.author().commit("RO1").ok());  // in flight, unacked
        ASSERT_TRUE(ncs.author().removeStory("RO1", "s2").ok());
        ASSERT_TRUE(ncs.author().commit("RO1").ok());  // queued behind it
        for (auto& queue : ncsTx.outbound) {
            queue.clear();  // the transmitted-but-undelivered frame is lost
        }
    }

    // Second incarnation: the persisted queue resumes in order; the device
    // applies each action exactly once and converges.
    {
        imos::store::FileMessageStore ncsStore{options};
        incs::Ncs ncs{
            incs::NcsConfig{config, TwoRoleFixture::machineInfo(imos::profiles::DeviceType::Ncs)},
            imos::PeerSession::Dependencies{&clock, &ncsTx, &ncsStore, nullptr, &random, nullptr}};
        for (std::size_t i = 0; i < imos::kChannelCount; ++i) {
            ncs.onChannelConnected(static_cast<Channel>(i));
        }
        pumpWith(ncs, 8);
        EXPECT_EQ(device.runningOrderModel().storyIDs("RO1"),
                  (std::vector<std::string>{"s3", "s1"}));
        EXPECT_TRUE(device.session().channelIdle(Channel::Ro));
    }
    EXPECT_EQ(actionsDelivered, 2);  // exactly once each, no duplicates
    std::filesystem::remove_all(root, ec);
}

// 8. Legacy-13 receive: 2.8-era wire messages arrive in 4.0 mode normalized
//    onto roElementAction/roElementStat semantics before they reach the
//    model; outbound they have no send path at all.
TEST(Profile2, LegacyThirteenNormalizeOnReceiveAndCannotBeSent) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");

    // roStoryInsert → roElementAction INSERT before the named story.
    fx.device->onFrameReceived(
        Channel::Ro, encode("<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
                            "<messageID>9001</messageID><roStoryInsert><roID>RO1</roID>"
                            "<storyID>s2</storyID><story><storyID>legacy</storyID></story>"
                            "</roStoryInsert></mos>"));
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"),
              (std::vector<std::string>{"s1", "legacy", "s2", "s3", "s4"}));

    // roStat → roElementStat element="RO".
    fx.device->onFrameReceived(
        Channel::Ro, encode("<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
                            "<messageID>9002</messageID><roStat><roID>RO1</roID>"
                            "<status>PLAY</status><time>2009-04-11T14:13:53</time>"
                            "</roStat></mos>"));
    EXPECT_EQ(fx.device->runningOrderModel().status(RoElement::RunningOrder, "RO1", "", ""),
              std::optional<std::string>{"PLAY"});

    // Both were acknowledged like their 4.0 equivalents.
    fx.settle();
    EXPECT_TRUE(fx.device->session().channelIdle(Channel::Ro));

    // Outbound: the removed thirteen have no send path in 4.0 mode.
    imos::Result<imos::Seq> rejected =
        fx.device->session().enqueue(imos::codec::makeMessage(MessageType::RoStoryInsert));
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().code, imos::ErrorCode::ProtocolViolation);
}

// 9a. Wrong-state negative: a duplicate roCreate (fresh messageID, so no
//     dedupe) draws a NACK roAck and mutates nothing.
TEST(Profile2, DuplicateRoCreateIsNackedNotApplied) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeBaseRo(fx, "RO1");
    roQueue(fx.deviceTransport).clear();

    fx.device->onFrameReceived(
        Channel::Ro, encode("<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
                            "<messageID>8001</messageID><roCreate><roID>RO1</roID>"
                            "<roSlug>imposter</roSlug></roCreate></mos>"));
    ASSERT_EQ(roQueue(fx.deviceTransport).size(), 1U);
    const Message ack = parseFrame(roQueue(fx.deviceTransport)[0]);
    ASSERT_EQ(ack.type(), MessageType::RoAck);
    EXPECT_TRUE(imos::profiles::roAckIsNack(ack));
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1").size(), 4U);  // untouched
    EXPECT_EQ(countResyncEvents(*fx.device), 0);  // wrong-state, not unknown-ID
}

// 9b. Oversized RO: the parse limit rejects the document before the model
//     ever sees it.
TEST(Profile2, OversizedRunningOrderIsRejectedByParseLimits) {
    FakeClock clock;
    FakeRandom random;
    LoopbackEndpoint transport;
    imos::InMemoryMessageStore store;
    imos::SessionConfig config = TwoRoleFixture::sessionConfig();
    config.parseLimits.maxDocumentBytes = 512;
    imos::Device device{
        imos::DeviceConfig{config, TwoRoleFixture::machineInfo(imos::profiles::DeviceType::Mos)},
        imos::PeerSession::Dependencies{&clock, &transport, &store, nullptr, &random, nullptr}};
    transport.connected.fill(true);
    device.onChannelConnected(Channel::Ro);

    std::string huge =
        "<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
        "<messageID>1</messageID><roCreate><roID>BIG</roID><roSlug>";
    huge.append(2048, 'x');
    huge += "</roSlug></roCreate></mos>";
    device.onFrameReceived(Channel::Ro, encode(huge));

    bool sawProtocolError = false;
    while (std::optional<imos::SessionEvent> event = device.nextEvent()) {
        sawProtocolError |= event->type == imos::SessionEventType::ProtocolError;
    }
    EXPECT_TRUE(sawProtocolError);
    EXPECT_FALSE(device.runningOrderModel().has("BIG"));
}

// 10. Per-element roAck statuses propagate into the commit result: with the
//     Reject policy an unknown objID NACKs the roCreate and the tuples ride
//     the CommitEvent; after switching to Accept, the recovery commit
//     (roReplace, since the projection went unknown) applies with the same
//     tuples as informational statuses.
namespace {

class MapObjectSource : public imos::profiles::IObjectSource {
public:
    std::optional<Message> object(std::string_view objID) override {
        const auto it = objects_.find(std::string{objID});
        return it == objects_.end() ? std::nullopt : std::optional<Message>{it->second};
    }
    std::vector<std::string> objectIDs() override {
        std::vector<std::string> ids;
        for (const auto& [id, message] : objects_) {
            ids.push_back(id);
        }
        return ids;
    }
    void put(const std::string& id) {
        Message object = imos::codec::makeMessage(MessageType::MosObj);
        imos::codec::view::MosObj{*object.body()}.setObjID(id);
        objects_.insert_or_assign(id, std::move(object));
    }

private:
    std::map<std::string, Message> objects_;
};

}  // namespace

TEST(Profile2, NackElementStatusesReachTheCommitResult) {
    TwoRoleFixture fx;
    MapObjectSource objects;
    objects.put("KNOWN");
    fx.device->attach(objects);
    fx.device->setUnknownObjectPolicy(
        imos::profiles::Profile2Responder::UnknownObjectPolicy::Reject);
    fx.connectAll();

    XmlNode story = RunningOrderAuthor::makeStory("s1", "Mixed");
    story.children.push_back(RunningOrderAuthor::makeItem("i1", "KNOWN", "m"));
    story.children.push_back(RunningOrderAuthor::makeItem("i2", "MISSING", "m"));
    ASSERT_TRUE(fx.ncs->author().create("RO9", "Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO9", story).ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO9").ok());
    fx.settle();

    EXPECT_FALSE(fx.device->runningOrderModel().has("RO9"));  // rejected outright
    std::vector<incs::CommitEvent> commits = drainCommits(*fx.ncs);
    ASSERT_EQ(commits.size(), 1U);
    EXPECT_FALSE(commits[0].ok);
    EXPECT_TRUE(commits[0].roStatus.starts_with("NACK"));
    ASSERT_EQ(commits[0].statuses.size(), 2U);
    EXPECT_EQ(commits[0].statuses[0].objID, "KNOWN");
    EXPECT_EQ(commits[0].statuses[0].status, "LOADED");
    EXPECT_EQ(commits[0].statuses[1].objID, "MISSING");
    EXPECT_EQ(commits[0].statuses[1].status, "UNKNOWN");

    // Accept policy (the spec example's behavior): the RO applies and the
    // unresolved object rides the ack as an informational status.
    fx.device->setUnknownObjectPolicy(
        imos::profiles::Profile2Responder::UnknownObjectPolicy::Accept);
    ASSERT_TRUE(fx.ncs->author().commit("RO9").ok());  // NACK marked it unknown → roReplace
    fx.settle();
    EXPECT_TRUE(fx.device->runningOrderModel().has("RO9"));
    commits = drainCommits(*fx.ncs);
    ASSERT_EQ(commits.size(), 1U);
    EXPECT_TRUE(commits[0].ok);
    EXPECT_TRUE(commits[0].roStatus.starts_with("Unknown object"));
    ASSERT_EQ(commits[0].statuses.size(), 2U);
    EXPECT_EQ(commits[0].statuses[1].status, "UNKNOWN");
}
