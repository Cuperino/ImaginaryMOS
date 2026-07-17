// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 1 — Basic Object Based Workflow conformance (plan §3.2 row 1, §10,
// M3). A real imos::Device against a real incs::Ncs over the loopback pair:
// object push (create/update/delete), single-object fetch (hit and NACK
// miss), full sync in both batch (pause=0 → one mosListAll) and paced
// (pause>0 → spaced mosObj, interleavable) modes, the reliability rules
// (duplicate suppression, retry, store-backed resume), the terminal-reply
// contract, and byte-clean object fidelity.

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/gen/views.h>
#include <imos/codec/message.h>
#include <imos/encoding/encoding.h>
#include <imos/profiles/profile1.h>
#include <imos/store/file_store.h>

#include "two_role_fixture.h"

using imos::codec::Message;
using imos::codec::MessageType;
using imos::profiles::DirectoryEvent;
using imos::profiles::DirectoryEventType;
using imos::test::FakeClock;
using imos::test::FakeRandom;
using imos::test::LoopbackEndpoint;
using imos::test::TwoRoleFixture;

namespace {

/// Editable object provider standing in for a device's media store. A
/// std::map keeps objectIDs() deterministic so paced-sync order is stable.
class MapObjectSource : public imos::profiles::IObjectSource {
public:
    std::optional<Message> object(std::string_view objID) override {
        const auto it = objects_.find(std::string{objID});
        if (it == objects_.end()) {
            return std::nullopt;
        }
        return it->second;  // verbatim copy
    }
    std::vector<std::string> objectIDs() override {
        std::vector<std::string> ids;
        for (const auto& [id, message] : objects_) {
            ids.push_back(id);
        }
        return ids;
    }
    void put(const std::string& id, Message message) {
        objects_.insert_or_assign(id, std::move(message));
    }
    void erase(const std::string& id) { objects_.erase(id); }

private:
    std::map<std::string, Message> objects_;
};

Message makeObject(std::string_view objID, std::string_view status,
                   std::string_view slug = "clip") {
    Message message = imos::codec::makeMessage(MessageType::MosObj);
    imos::codec::XmlNode* body = message.body();
    imos::codec::view::MosObj view{*body};
    view.setObjID(objID);
    view.setObjSlug(slug);
    view.setObjType("VIDEO");
    view.setStatus(status);
    return message;
}

std::string objStatus(const Message& mosObj) {
    if (const imos::codec::XmlNode* body = mosObj.body()) {
        if (const imos::codec::XmlNode* status = body->firstChildElement("status")) {
            return status->text();
        }
    }
    return {};
}

std::vector<DirectoryEvent> drainDirectory(incs::Ncs& ncs) {
    std::vector<DirectoryEvent> events;
    while (std::optional<DirectoryEvent> event = ncs.nextDirectoryEvent()) {
        events.push_back(std::move(*event));
    }
    return events;
}

int countType(const std::vector<DirectoryEvent>& events, DirectoryEventType type) {
    int count = 0;
    for (const DirectoryEvent& event : events) {
        count += event.type == type ? 1 : 0;
    }
    return count;
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

std::size_t momDepth(LoopbackEndpoint& endpoint) {
    return endpoint.outbound[static_cast<std::size_t>(imos::Channel::Mom)].size();
}

}  // namespace

// 1. Push flow: create / update / delete reflected in the NCS mirror.
TEST(Profile1, UnsolicitedPushKeepsDirectoryInSync) {
    TwoRoleFixture fx;
    fx.connectAll();

    ASSERT_TRUE(fx.device->announceObject(makeObject("M1", "NEW", "Hotel Fire")).ok());
    fx.settle();

    const Message* mirrored = fx.ncs->object("M1");
    ASSERT_NE(mirrored, nullptr);
    EXPECT_EQ(objStatus(*mirrored), "NEW");
    auto events = drainDirectory(*fx.ncs);
    EXPECT_EQ(countType(events, DirectoryEventType::ObjectUpdated), 1);

    // Update: same objID, new status/slug replaces the mirror entry.
    ASSERT_TRUE(fx.device->announceObject(makeObject("M1", "UPDATED", "Hotel Fire v2")).ok());
    fx.settle();
    mirrored = fx.ncs->object("M1");
    ASSERT_NE(mirrored, nullptr);
    EXPECT_EQ(objStatus(*mirrored), "UPDATED");

    // Delete: DELETED status removes the entry.
    ASSERT_TRUE(fx.device->announceObject(makeObject("M1", "DELETED")).ok());
    fx.settle();
    EXPECT_EQ(fx.ncs->object("M1"), nullptr);
    events = drainDirectory(*fx.ncs);
    EXPECT_EQ(countType(events, DirectoryEventType::ObjectDeleted), 1);

    // Every exchange completed: both mom channels are idle.
    EXPECT_TRUE(fx.device->session().channelIdle(imos::Channel::Mom));
    EXPECT_TRUE(fx.ncs->session().channelIdle(imos::Channel::Mom));
}

// 2. mosReqObj: hit ingests without a re-ack; miss surfaces a NACK.
TEST(Profile1, RequestObjectHitAndMiss) {
    TwoRoleFixture fx;
    MapObjectSource source;
    source.put("M000123", makeObject("M000123", "UPDATED", "Hotel Fire"));
    fx.device->attach(source);
    fx.connectAll();

    // Hit → the full mosObj comes back and is ingested.
    ASSERT_TRUE(fx.ncs->requestObject("M000123").ok());
    fx.settle();
    ASSERT_NE(fx.ncs->object("M000123"), nullptr);
    auto events = drainDirectory(*fx.ncs);
    EXPECT_EQ(countType(events, DirectoryEventType::ObjectUpdated), 1);
    EXPECT_EQ(countType(events, DirectoryEventType::RequestFailed), 0);

    // Miss → a NACK mosAck with a statusDescription surfaced to the app.
    ASSERT_TRUE(fx.ncs->requestObject("UNKNOWN").ok());
    fx.settle();
    EXPECT_EQ(fx.ncs->object("UNKNOWN"), nullptr);
    events = drainDirectory(*fx.ncs);
    ASSERT_EQ(countType(events, DirectoryEventType::RequestFailed), 1);
    for (const DirectoryEvent& event : events) {
        if (event.type == DirectoryEventType::RequestFailed) {
            EXPECT_EQ(event.objID, "UNKNOWN");
            EXPECT_FALSE(event.statusDescription.empty());
        }
    }
}

// 6. Terminal-reply contract: answering mosReqObj leaves no phantom in-flight.
TEST(Profile1, ReplyToRequestLeavesChannelIdle) {
    TwoRoleFixture fx;
    MapObjectSource source;
    source.put("M1", makeObject("M1", "NEW"));
    fx.device->attach(source);
    fx.connectAll();

    ASSERT_TRUE(fx.ncs->requestObject("M1").ok());
    fx.settle();

    EXPECT_TRUE(fx.device->session().channelIdle(imos::Channel::Mom));
    EXPECT_TRUE(fx.ncs->session().channelIdle(imos::Channel::Mom));
    EXPECT_EQ(fx.device->session().queueDepth(imos::Channel::Mom), 0U);
}

// 3. mosReqAll pause=0 → the whole catalog in one mosListAll, acked.
TEST(Profile1, RequestAllBatchModeMirrorsWholeCatalog) {
    TwoRoleFixture fx;
    MapObjectSource source;
    for (const char* id : {"A1", "B2", "C3", "D4"}) {
        source.put(id, makeObject(id, "UPDATED"));
    }
    fx.device->attach(source);
    fx.connectAll();

    ASSERT_TRUE(fx.ncs->requestAll(0).ok());
    fx.settle();

    EXPECT_EQ(fx.ncs->objectIDs().size(), 4U);
    for (const char* id : {"A1", "B2", "C3", "D4"}) {
        EXPECT_NE(fx.ncs->object(id), nullptr) << id;
    }
    const auto events = drainDirectory(*fx.ncs);
    EXPECT_EQ(countType(events, DirectoryEventType::SyncAcknowledged), 1);
    EXPECT_EQ(countType(events, DirectoryEventType::ObjectUpdated), 4);
    EXPECT_TRUE(fx.device->session().channelIdle(imos::Channel::Mom));
}

// 4. mosReqAll pause=N → mosObj spaced exactly N seconds, interleaved with a
//    live push, and an object that vanishes mid-stream is skipped.
TEST(Profile1, RequestAllPacedModeSpacesInterleavesAndSkips) {
    TwoRoleFixture fx;
    MapObjectSource source;
    source.put("A", makeObject("A", "UPDATED"));
    source.put("B", makeObject("B", "UPDATED"));
    source.put("C", makeObject("C", "UPDATED"));
    fx.device->attach(source);
    fx.connectAll();

    // reqAll handled at t=1000 → first release due at t=3000 (pause = 2 s).
    ASSERT_TRUE(fx.ncs->requestAll(2).ok());
    fx.settle();
    EXPECT_EQ(fx.ncs->objectIDs().size(), 0U);  // acked, but no objects yet
    EXPECT_EQ(countType(drainDirectory(*fx.ncs), DirectoryEventType::SyncAcknowledged), 1);

    // Just short of the interval: nothing releases.
    fx.clock.advance(1999);
    fx.settle();
    EXPECT_EQ(fx.ncs->objectIDs().size(), 0U);

    // Exactly at the interval: the first object (A) is released.
    fx.clock.advance(1);
    fx.settle();
    EXPECT_EQ(fx.ncs->objectIDs().size(), 1U);
    EXPECT_NE(fx.ncs->object("A"), nullptr);

    // A live push interleaves mid-sync (the spec's stated purpose for pacing).
    ASSERT_TRUE(fx.device->announceObject(makeObject("LIVE", "NEW")).ok());
    fx.settle();
    EXPECT_NE(fx.ncs->object("LIVE"), nullptr);

    // Next interval releases B.
    fx.clock.advanceSeconds(2);
    fx.settle();
    EXPECT_NE(fx.ncs->object("B"), nullptr);

    // C vanishes before its slot: the paced stream skips it and finishes.
    source.erase("C");
    fx.clock.advanceSeconds(2);
    fx.settle();
    EXPECT_EQ(fx.ncs->object("C"), nullptr);

    // Final mirror: A, B, and the interleaved LIVE — never C.
    EXPECT_EQ(fx.ncs->objectIDs().size(), 3U);
    EXPECT_FALSE(fx.device->nextDeadline().has_value());  // paced job drained
}

// 5a. Duplicate push (peer retry) is suppressed by messageID dedupe and
//     re-acked, without a second ingest.
TEST(Profile1, DuplicatePushSuppressedAndReAcked) {
    TwoRoleFixture fx;
    fx.connectAll();

    ASSERT_TRUE(fx.device->announceObject(makeObject("M", "NEW")).ok());
    ASSERT_EQ(momDepth(fx.deviceTransport), 1U);
    const std::vector<std::uint8_t> push =
        fx.deviceTransport.outbound[static_cast<std::size_t>(imos::Channel::Mom)][0];

    // Deliver the identical frame twice, as a retrying peer would.
    fx.ncs->onFrameReceived(imos::Channel::Mom, push);
    fx.ncs->onFrameReceived(imos::Channel::Mom, push);

    EXPECT_EQ(fx.ncs->objectIDs().size(), 1U);
    EXPECT_EQ(countType(drainDirectory(*fx.ncs), DirectoryEventType::ObjectUpdated), 1);
    // Both deliveries produced a mosAck (directory ack + session re-ack).
    EXPECT_EQ(momDepth(fx.ncsTransport), 2U);
    for (std::size_t i = 0; i < 2; ++i) {
        const Message ack =
            parseFrame(fx.ncsTransport.outbound[static_cast<std::size_t>(imos::Channel::Mom)][i]);
        EXPECT_EQ(ack.type(), MessageType::MosAck);
    }
}

// 5b. An unacked push is retransmitted after the ack timeout.
TEST(Profile1, UnackedPushRetriesAfterTimeout) {
    TwoRoleFixture fx;
    fx.connectAll();

    ASSERT_TRUE(fx.device->announceObject(makeObject("M", "NEW")).ok());
    ASSERT_EQ(momDepth(fx.deviceTransport), 1U);

    // Withhold the ack; past the retry timeout the device resends the bytes.
    fx.clock.advance(10'001);
    fx.device->poll();
    ASSERT_EQ(momDepth(fx.deviceTransport), 2U);
    const auto& mom = fx.deviceTransport.outbound[static_cast<std::size_t>(imos::Channel::Mom)];
    EXPECT_EQ(mom[0], mom[1]);  // same messageID, same bytes
}

// 5c. Store-backed resume (M3 exit criterion): a push queued while offline is
//     delivered exactly once after the device is rebuilt from a file store.
TEST(Profile1, StoreBackedResumeDeliversPendingPushExactlyOnce) {
    FakeClock clock;
    FakeRandom random;
    const std::filesystem::path root =
        std::filesystem::path{::testing::TempDir()} / "imos-p1-resume";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);  // best-effort: leftovers from a prior run

    imos::store::FileStoreOptions options;
    options.root = root;

    const imos::SessionConfig config = TwoRoleFixture::sessionConfig();

    // First incarnation: never connected, so the push only reaches the store.
    {
        imos::store::FileMessageStore deviceStore{options};
        LoopbackEndpoint offline;
        imos::Device device{imos::DeviceConfig{config, TwoRoleFixture::machineInfo(
                                                           imos::profiles::DeviceType::Mos)},
                            imos::PeerSession::Dependencies{&clock, &offline, &deviceStore, nullptr,
                                                            &random, nullptr}};
        ASSERT_TRUE(device.announceObject(makeObject("M9", "NEW")).ok());
        EXPECT_EQ(offline.outbound[static_cast<std::size_t>(imos::Channel::Mom)].size(), 0U);
    }

    // Rebuild from the same store and connect; the pending push resumes. Keep
    // the store scoped so its file handles are released before remove_all
    // below — Windows refuses to delete a directory with open handles.
    bool mirrored = false;
    int pushesSeen = 0;
    {
        imos::store::FileMessageStore deviceStore{options};
        LoopbackEndpoint deviceTx;
        LoopbackEndpoint ncsTx;
        imos::InMemoryMessageStore ncsStore;
        imos::Device device{imos::DeviceConfig{config, TwoRoleFixture::machineInfo(
                                                           imos::profiles::DeviceType::Mos)},
                            imos::PeerSession::Dependencies{&clock, &deviceTx, &deviceStore,
                                                            nullptr, &random, nullptr}};
        incs::Ncs ncs{
            incs::NcsConfig{config, TwoRoleFixture::machineInfo(imos::profiles::DeviceType::Ncs)},
            imos::PeerSession::Dependencies{&clock, &ncsTx, &ncsStore, nullptr, &random, nullptr}};

        for (std::size_t i = 0; i < imos::kChannelCount; ++i) {
            deviceTx.connected[i] = true;
            ncsTx.connected[i] = true;
            device.onChannelConnected(static_cast<imos::Channel>(i));
            ncs.onChannelConnected(static_cast<imos::Channel>(i));
        }

        for (int round = 0; round < 16; ++round) {
            device.poll();
            ncs.poll();
            auto& deviceMom = deviceTx.outbound[static_cast<std::size_t>(imos::Channel::Mom)];
            auto& ncsMom = ncsTx.outbound[static_cast<std::size_t>(imos::Channel::Mom)];
            std::vector<std::vector<std::uint8_t>> toNcs(deviceMom.begin(), deviceMom.end());
            std::vector<std::vector<std::uint8_t>> toDevice(ncsMom.begin(), ncsMom.end());
            deviceMom.clear();
            ncsMom.clear();
            for (const auto& frame : toNcs) {
                pushesSeen += parseFrame(frame).type() == MessageType::MosObj ? 1 : 0;
                ncs.onFrameReceived(imos::Channel::Mom, frame);
            }
            for (const auto& frame : toDevice) {
                device.onFrameReceived(imos::Channel::Mom, frame);
            }
        }
        mirrored = ncs.object("M9") != nullptr;
    }

    EXPECT_TRUE(mirrored);     // the pending push resumed after restart
    EXPECT_EQ(pushesSeen, 1);  // exactly once, no duplicate on resume
    std::filesystem::remove_all(root, ec);
}

// Edge: a mosReqAll that omits <pause> defaults to batch mode (spec §3.2.1).
TEST(Profile1, RequestAllWithoutPauseDefaultsToBatch) {
    TwoRoleFixture fx;
    MapObjectSource source;
    source.put("A", makeObject("A", "UPDATED"));
    source.put("B", makeObject("B", "UPDATED"));
    fx.device->attach(source);
    fx.connectAll();

    // Feed the device a mosReqAll with no <pause> element directly.
    fx.device->onFrameReceived(
        imos::Channel::Mom,
        encode("<mos><mosID>device.example.mos</mosID><ncsID>ncs.example.com</ncsID>"
               "<messageID>77</messageID><mosReqAll></mosReqAll></mos>"));

    // The device answers with a mosAck then one mosListAll carrying both.
    auto& mom = fx.deviceTransport.outbound[static_cast<std::size_t>(imos::Channel::Mom)];
    ASSERT_EQ(mom.size(), 2U);
    EXPECT_EQ(parseFrame(mom[0]).type(), MessageType::MosAck);
    Message list = parseFrame(mom[1]);
    ASSERT_EQ(list.type(), MessageType::MosListAll);
    imos::codec::view::MosListAll view{*list.body()};
    EXPECT_EQ(view.mosObjCount(), 2U);
}

// 7. Codec fidelity: a pushed spec object arrives byte-identical — unknown
//    tags and the opaque mosPayload ride through untouched.
TEST(Profile1, PushedObjectRoundTripsByteClean) {
    std::ifstream file(std::string{IMOS_FIXTURES_DIR} + "/spec-examples/mosObj.xml");
    ASSERT_TRUE(file.is_open());
    std::stringstream buffer;
    buffer << file.rdbuf();
    auto parsed = imos::codec::parseMessage(buffer.str());
    ASSERT_TRUE(parsed.ok());
    const Message fixture = std::move(parsed).value();
    const std::string originalObj = imos::codec::serializeXmlNode(*fixture.body());

    TwoRoleFixture fx;
    fx.connectAll();
    ASSERT_TRUE(fx.device->announceObject(fixture).ok());
    fx.settle();

    const Message* mirrored = fx.ncs->object("M000123");
    ASSERT_NE(mirrored, nullptr);
    EXPECT_EQ(imos::codec::serializeXmlNode(*mirrored->body()), originalObj);
}
