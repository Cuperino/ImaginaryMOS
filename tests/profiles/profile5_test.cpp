// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 5 — Item Control conformance (plan §3.2 row 5,
// reference/profile5-plan.md §4, spec §3.7.2–§3.7.3, milestone M4). A real
// imos::Device against a real incs::Ncs over the loopback pair. Covers: the
// scope matrix (all five commands at RO/story/item scope, selected by empty
// lower-level IDs); target validation against the shadow model with the §3.3
// resync escalation on unknown IDs; the NACK negatives (inconsistent scope
// shape, unknown command word) and the unknown-tag lenient rule; roItemCue
// as an always-acked notification (no model mutation) with roEventTime
// parsed through time_format.h; prompter origination matching both gated
// spec fixtures — including the parent-MOS form's counterintuitive
// envelope <ncsID> assignment; and both §3.4 wire encodings.

#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/message.h>
#include <imos/codec/xml.h>
#include <imos/device.h>
#include <imos/encoding/encoding.h>
#include <imos/profiles/profile5.h>
#include <imos/session/in_memory_store.h>
#include <imos/time_format.h>
#include <incs/running_order_author.h>

#include "fakes.h"
#include "two_role_fixture.h"

using imos::Channel;
using imos::WallClockTime;
using imos::codec::Message;
using imos::codec::MessageType;
using imos::codec::parseMessage;
using imos::codec::serializeMessage;
using imos::codec::serializeXmlNode;
using imos::codec::XmlNode;
using imos::encoding::WireEncoding;
using imos::profiles::CtrlCommand;
using imos::profiles::CtrlRequest;
using imos::profiles::CtrlScope;
using imos::profiles::ItemCue;
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

Message parseFrame(const std::vector<std::uint8_t>& frame,
                   WireEncoding encoding = WireEncoding::Ucs2Be) {
    imos::encoding::DecodeOptions options;
    options.encoding = encoding;
    auto decoded = imos::encoding::decodeToUtf8(frame, options);
    EXPECT_TRUE(decoded.ok());
    auto parsed = parseMessage(decoded.value());
    EXPECT_TRUE(parsed.ok());
    return std::move(parsed).value();
}

std::deque<std::vector<std::uint8_t>>& roQueue(LoopbackEndpoint& endpoint) {
    return endpoint.outbound[static_cast<std::size_t>(Channel::Ro)];
}

Message loadFixture(const std::string& name) {
    const std::string path = std::string{IMOS_FIXTURES_DIR} + "/spec-examples/" + name;
    std::ifstream file(path);
    EXPECT_TRUE(file.is_open()) << path;
    std::stringstream buffer;
    buffer << file.rdbuf();
    auto parsed = parseMessage(buffer.str());
    EXPECT_TRUE(parsed.ok()) << (parsed.ok() ? "" : parsed.error().message);
    return std::move(parsed).value();
}

/// Drop pure-formatting (all-whitespace) text-node children recursively, so
/// a programmatically built tree compares against a hand-indented fixture on
/// elements, attributes, and significant text only.
XmlNode dropBlankText(XmlNode node) {
    std::vector<XmlNode> kept;
    for (XmlNode& child : node.children) {
        if (child.kind == imos::codec::XmlNodeKind::Text &&
            child.value.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }
        kept.push_back(dropBlankText(std::move(child)));
    }
    node.children = std::move(kept);
    return node;
}

/// Converge one RO with a story that carries one item, so every scope level
/// has a real target on both roles.
void convergeRoWithItem(TwoRoleFixture& fx, const std::string& roID) {
    ASSERT_TRUE(fx.ncs->author().create(roID, roID + " Rundown").ok());
    XmlNode story = RunningOrderAuthor::makeStory("s1", "Lead");
    story.children.push_back(RunningOrderAuthor::makeItem("i1", "OBJ1", TwoRoleFixture::kMosID));
    ASSERT_TRUE(fx.ncs->author().insertStory(roID, story).ok());
    ASSERT_TRUE(fx.ncs->author().commit(roID).ok());
    fx.settle();
    ASSERT_EQ(fx.device->runningOrderModel().storyIDs(roID), (std::vector<std::string>{"s1"}));
    while (fx.ncs->author().nextEvent().has_value()) {
    }
}

imos::profiles::CtrlResult nextCtrlResult(TwoRoleFixture& fx) {
    std::optional<imos::profiles::CtrlResult> result = fx.ncs->nextCtrlResult();
    EXPECT_TRUE(result.has_value());
    return result.value_or(imos::profiles::CtrlResult{});
}

/// Recording IControlSink. Copies keep no message pointer (it is only valid
/// during the callback), so significant wire content is captured as
/// serialized XML at callback time.
class RecordingControlSink : public imos::profiles::IControlSink {
public:
    imos::Result<void> onCtrl(const CtrlRequest& request) override {
        CtrlRequest copy = request;
        copy.message = nullptr;
        ctrls.push_back(std::move(copy));
        lastCtrlXml = request.message ? serializeMessage(*request.message) : std::string{};
        if (!nackDetail.empty()) {
            return imos::makeError(imos::ErrorCode::WrongState, nackDetail);
        }
        return {};
    }
    void onItemCue(const ItemCue& cue) override {
        ItemCue copy = cue;
        copy.message = nullptr;
        cues.push_back(std::move(copy));
        lastCueXml = cue.message ? serializeMessage(*cue.message) : std::string{};
    }

    std::vector<CtrlRequest> ctrls;
    std::vector<ItemCue> cues;
    std::string lastCtrlXml;
    std::string lastCueXml;
    std::string nackDetail;  ///< when set, onCtrl rejects with this detail
};

/// A raw roCtrl envelope addressed NCS → device (for shapes the typed
/// origination API refuses to build).
std::string roCtrlXml(const std::string& messageID, const std::string& inner) {
    return "<mos><mosID>" + std::string{TwoRoleFixture::kMosID} + "</mosID><ncsID>" +
           std::string{TwoRoleFixture::kNcsID} + "</ncsID><messageID>" + messageID +
           "</messageID><roCtrl>" + inner + "</roCtrl></mos>";
}

/// Inject a raw frame into the device and return its immediate roAck.
Message injectAndReadAck(TwoRoleFixture& fx, const std::string& xml) {
    roQueue(fx.deviceTransport).clear();
    fx.device->onFrameReceived(Channel::Ro, encode(xml));
    EXPECT_FALSE(roQueue(fx.deviceTransport).empty());
    Message ack = parseFrame(roQueue(fx.deviceTransport).back());
    EXPECT_EQ(ack.type(), MessageType::RoAck);
    return ack;
}

/// The cue the two spec roItemCue fixtures carry (they differ only in the
/// envelope addressing).
ItemCue fixtureCue(const Message& fixture) {
    auto parsed = imos::profiles::parseRoItemCue(fixture);
    EXPECT_TRUE(parsed.ok());
    ItemCue cue = std::move(parsed).value();
    cue.message = nullptr;
    return cue;
}

}  // namespace

// 1. The scope matrix: every command lands at every scope, resolved from
//    the empty-lower-ID shape, and acks flow back (spec §3.7.3).
TEST(Profile5, ScopeMatrixDeliversResolvedRequests) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeRoWithItem(fx, "RO1");
    RecordingControlSink sink;
    fx.device->attach(sink);

    const CtrlCommand commands[] = {CtrlCommand::Ready, CtrlCommand::Execute, CtrlCommand::Pause,
                                    CtrlCommand::Stop, CtrlCommand::Signal};
    const CtrlScope scopes[] = {CtrlScope::RunningOrder, CtrlScope::Story, CtrlScope::Item};
    std::size_t delivered = 0;
    for (const CtrlCommand command : commands) {
        for (const CtrlScope scope : scopes) {
            SCOPED_TRACE(std::string{imos::profiles::ctrlCommandName(command)} + " at scope " +
                         std::to_string(static_cast<int>(scope)));
            const std::string storyID = scope == CtrlScope::RunningOrder ? "" : "s1";
            const std::string itemID = scope == CtrlScope::Item ? "i1" : "";
            ASSERT_TRUE(fx.ncs->sendCtrl(scope, "RO1", storyID, itemID, command).ok());
            fx.settle();

            ASSERT_EQ(sink.ctrls.size(), delivered + 1);
            const CtrlRequest& request = sink.ctrls.back();
            EXPECT_EQ(request.scope, scope);
            EXPECT_EQ(request.command, command);
            EXPECT_EQ(request.roID, "RO1");
            EXPECT_EQ(request.storyID, storyID);
            EXPECT_EQ(request.itemID, itemID);
            const imos::profiles::CtrlResult result = nextCtrlResult(fx);
            EXPECT_TRUE(result.ok) << result.roStatus;
            ++delivered;
        }
    }
}

// 2. The built roCtrl carries all three ID elements — the lower levels
//    *empty*, not absent (the spec's scope-selection shape).
TEST(Profile5, BuiltRoCtrlEmitsEmptyLowerLevelIds) {
    const Message message = imos::profiles::buildRoCtrl("RO1", "", "", CtrlCommand::Execute);
    const std::string xml = serializeMessage(message);
    EXPECT_NE(xml.find("<roID>RO1</roID>"), std::string::npos);
    EXPECT_NE(xml.find("<storyID/>"), std::string::npos);
    EXPECT_NE(xml.find("<itemID/>"), std::string::npos);
    EXPECT_NE(xml.find("<command>EXECUTE</command>"), std::string::npos);
}

// 3. Without a sink, requests are still validated, acked "OK", and queue
//    for the pull-mode drain.
TEST(Profile5, SinklessCtrlQueuesAndAcks) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeRoWithItem(fx, "RO1");

    ASSERT_TRUE(fx.ncs->sendCtrl(CtrlScope::Story, "RO1", "s1", "", CtrlCommand::Ready).ok());
    fx.settle();

    EXPECT_TRUE(nextCtrlResult(fx).ok);
    std::optional<CtrlRequest> request = fx.device->nextCtrlRequest();
    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->scope, CtrlScope::Story);
    EXPECT_EQ(request->storyID, "s1");
    EXPECT_EQ(request->message, nullptr);
    EXPECT_FALSE(fx.device->nextCtrlRequest().has_value());
}

// 4. A sink rejection becomes the NACK detail on the wire (plan §3.1:
//    Result → ACK/NACK).
TEST(Profile5, SinkNackPropagatesDetail) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeRoWithItem(fx, "RO1");
    RecordingControlSink sink;
    sink.nackDetail = "transport wedged";
    fx.device->attach(sink);

    ASSERT_TRUE(
        fx.ncs->sendCtrl(CtrlScope::RunningOrder, "RO1", "", "", CtrlCommand::Execute).ok());
    fx.settle();

    const imos::profiles::CtrlResult result = nextCtrlResult(fx);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.roStatus, "NACK: transport wedged");
}

// 5. roCtrl for an RO the shadow model does not hold: NACK, and the §3.3
//    resync driver fires (shared path with Profile 2) — one ResyncNeeded
//    announcement surfaces to the application.
TEST(Profile5, UnknownRunningOrderNacksAndFiresResync) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeRoWithItem(fx, "RO1");
    RecordingControlSink sink;
    fx.device->attach(sink);

    ASSERT_TRUE(fx.ncs->sendCtrl(CtrlScope::RunningOrder, "GHOST", "", "", CtrlCommand::Stop).ok());
    fx.settle();

    const imos::profiles::CtrlResult result = nextCtrlResult(fx);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.roStatus.find("unknown roID 'GHOST'"), std::string::npos);
    EXPECT_TRUE(sink.ctrls.empty());  // never delivered to the host

    bool resyncAnnounced = false;
    while (std::optional<imos::SessionEvent> event = fx.device->nextEvent()) {
        if (event->type == imos::SessionEventType::ResyncNeeded) {
            resyncAnnounced = true;
        }
    }
    EXPECT_TRUE(resyncAnnounced);
    // The rebuild ran to completion: the NCS answered the roReq with a NACK
    // (it has no GHOST either), so the device is no longer resyncing.
    EXPECT_FALSE(fx.device->runningOrderModel().has("GHOST"));
}

// 6. An unknown story inside a known RO: NACK, resync rebuilds that RO, and
//    the model still matches the authoring store afterwards.
TEST(Profile5, UnknownStoryNacksAndRebuildsTheRo) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeRoWithItem(fx, "RO1");

    ASSERT_TRUE(fx.ncs->sendCtrl(CtrlScope::Story, "RO1", "sMissing", "", CtrlCommand::Ready).ok());
    fx.settle();

    const imos::profiles::CtrlResult result = nextCtrlResult(fx);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.roStatus.find("unknown storyID 'sMissing'"), std::string::npos);
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"),
              fx.ncs->author().desired().storyIDs("RO1"));
}

// 7. Inconsistent scope shape — itemID present but storyID empty — is a
//    diagnostic NACK, never a guess (plan §3.1). Injected raw: the typed
//    origination API refuses to build the shape.
TEST(Profile5, InconsistentScopeShapeNacks) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeRoWithItem(fx, "RO1");
    RecordingControlSink sink;
    fx.device->attach(sink);

    const Message ack =
        injectAndReadAck(fx, roCtrlXml("C-1",
                                       "<roID>RO1</roID><storyID></storyID><itemID>i1</itemID>"
                                       "<command>EXECUTE</command>"));
    const std::string status = imos::profiles::roAckStatusText(ack);
    EXPECT_TRUE(status.starts_with("NACK"));
    EXPECT_NE(status.find("inconsistent scope shape"), std::string::npos);
    EXPECT_TRUE(sink.ctrls.empty());
}

// 8. The command vocabulary is a closed enum: an unknown word is a NACK.
TEST(Profile5, UnknownCommandNacks) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeRoWithItem(fx, "RO1");

    const Message ack = injectAndReadAck(
        fx, roCtrlXml("C-2", "<roID>RO1</roID><storyID/><itemID/><command>DETONATE</command>"));
    const std::string status = imos::profiles::roAckStatusText(ack);
    EXPECT_TRUE(status.starts_with("NACK"));
    EXPECT_NE(status.find("'DETONATE'"), std::string::npos);
}

// 9. Unknown extra tags ride through — ignored for semantics, retained in
//    the message the sink sees (the codec's lenient rule).
TEST(Profile5, UnknownTagsIgnoredAndRetained) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeRoWithItem(fx, "RO1");
    RecordingControlSink sink;
    fx.device->attach(sink);

    const Message ack =
        injectAndReadAck(fx, roCtrlXml("C-3",
                                       "<roID>RO1</roID><storyID/><itemID/><command>PAUSE</command>"
                                       "<vendorHint mode=\"fast\">warp</vendorHint>"));
    EXPECT_EQ(imos::profiles::roAckStatusText(ack), "OK");
    ASSERT_EQ(sink.ctrls.size(), 1U);
    EXPECT_EQ(sink.ctrls.back().command, CtrlCommand::Pause);
    EXPECT_NE(sink.lastCtrlXml.find("<vendorHint mode=\"fast\">warp</vendorHint>"),
              std::string::npos);
}

// 10. A cue is a timed notification: delivered with roEventTime parsed
//     through time_format.h, always acked, and never a model mutation.
TEST(Profile5, ItemCueIsNotificationNotMutation) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeRoWithItem(fx, "RO1");
    RecordingControlSink sink;
    fx.device->attach(sink);
    const std::string modelBefore =
        serializeXmlNode(*fx.device->runningOrderModel().runningOrder("RO1"));

    ItemCue cue;
    cue.mosID = TwoRoleFixture::kMosID;
    cue.roID = "RO1";
    cue.storyID = "s1";
    cue.itemID = "i1";
    cue.roEventType = "Prompter";
    cue.roEventTime = "2000-03-20T10:45:00.00";
    ASSERT_TRUE(fx.ncs->sendItemCue(cue).ok());
    fx.settle();

    EXPECT_TRUE(nextCtrlResult(fx).ok);
    ASSERT_EQ(sink.cues.size(), 1U);
    const ItemCue& received = sink.cues.back();
    EXPECT_EQ(received.roEventType, "Prompter");
    EXPECT_EQ(received.roEventTime, "2000-03-20T10:45:00.00");
    ASSERT_TRUE(received.eventTime.has_value());
    using namespace std::chrono;
    const auto expected = sys_days{March / 20 / 2000} + hours{10} + minutes{45};
    EXPECT_EQ(received.eventTime->millisSinceEpoch,
              duration_cast<milliseconds>(expected.time_since_epoch()).count());
    // Notification, not construction: the shadow RO is untouched.
    EXPECT_EQ(serializeXmlNode(*fx.device->runningOrderModel().runningOrder("RO1")), modelBefore);
}

// 11. parseSpecTime tolerates the §6 dialect variants and rejects garbage;
//     formatSpecTime round-trips the fixture value to the plain UTC form.
TEST(Profile5, SpecTimeParsingToleranceAndRoundTrip) {
    const auto base = imos::parseSpecTime("2000-03-20T10:45:00");
    ASSERT_TRUE(base.ok());
    EXPECT_EQ(imos::formatSpecTime(base.value()), "2000-03-20T10:45:00");

    const auto fractional = imos::parseSpecTime("2000-03-20T10:45:00.00");
    ASSERT_TRUE(fractional.ok());
    EXPECT_EQ(fractional.value().millisSinceEpoch, base.value().millisSinceEpoch);
    EXPECT_EQ(imos::formatSpecTime(fractional.value()), "2000-03-20T10:45:00");

    const auto halfSecond = imos::parseSpecTime("2000-03-20T10:45:00.5");
    ASSERT_TRUE(halfSecond.ok());
    EXPECT_EQ(halfSecond.value().millisSinceEpoch, base.value().millisSinceEpoch + 500);

    const auto zulu = imos::parseSpecTime("2000-03-20T10:45:00Z");
    ASSERT_TRUE(zulu.ok());
    EXPECT_EQ(zulu.value().millisSinceEpoch, base.value().millisSinceEpoch);

    const auto offset = imos::parseSpecTime("2000-03-20T11:45:00+01:00");
    ASSERT_TRUE(offset.ok());
    EXPECT_EQ(offset.value().millisSinceEpoch, base.value().millisSinceEpoch);
    const auto compactOffset = imos::parseSpecTime("2000-03-20T09:15:00-0130");
    ASSERT_TRUE(compactOffset.ok());
    EXPECT_EQ(compactOffset.value().millisSinceEpoch, base.value().millisSinceEpoch);

    EXPECT_FALSE(imos::parseSpecTime("").ok());
    EXPECT_FALSE(imos::parseSpecTime("yesterday").ok());
    EXPECT_FALSE(imos::parseSpecTime("2000-13-20T10:45:00").ok());
    EXPECT_FALSE(imos::parseSpecTime("2000-03-20T24:45:00").ok());
    EXPECT_FALSE(imos::parseSpecTime("2000-03-20T10:45:00.").ok());
    EXPECT_FALSE(imos::parseSpecTime("2000-03-20T10:45:00X").ok());
}

// 12. The gated spec fixtures decode to the documented semantics.
TEST(Profile5, SpecFixturesDecode) {
    const auto ctrl = imos::profiles::parseRoCtrl(loadFixture("roCtrl.xml"));
    ASSERT_TRUE(ctrl.ok());
    EXPECT_EQ(ctrl.value().scope, CtrlScope::Item);
    EXPECT_EQ(ctrl.value().command, CtrlCommand::Execute);
    EXPECT_EQ(ctrl.value().roID, "3dedde9jd");
    EXPECT_EQ(ctrl.value().storyID, "A3fds3d");
    EXPECT_EQ(ctrl.value().itemID, "30848");

    const Message fixture = loadFixture("roItemCue.xml");
    const auto cue = imos::profiles::parseRoItemCue(fixture);
    ASSERT_TRUE(cue.ok());
    EXPECT_EQ(cue.value().mosID, "videoserver.station.group.com");
    EXPECT_EQ(cue.value().roEventType, "Prompter");
    ASSERT_TRUE(cue.value().eventTime.has_value());
    ASSERT_EQ(cue.value().metadata.size(), 1U);
    EXPECT_NE(serializeXmlNode(cue.value().metadata.front()).find("<netPropDelay>10"),
              std::string::npos);
}

// 13. Prompter origination reproduces both spec examples (§3.7.2): the
//     NCS-addressed form, and the parent-MOS form whose envelope <ncsID>
//     carries the *parent MOS* name — the deliberately counterintuitive
//     assignment — while <mosID> stays the originating prompter. Structural
//     comparison modulo the declared whitespace policy; the envelope IDs are
//     asserted exactly.
TEST(Profile5, PrompterOriginationMatchesSpecFixtures) {
    struct Case {
        const char* fixture;
        const char* addressee;  // sendItemCue override ("" = session default)
        const char* expectedNcsID;
    };
    const Case cases[] = {
        {"roItemCue.xml", "", "ncs.station.com"},
        {"roItemCue-2.xml", "videoserver.station.group.com", "videoserver.station.group.com"},
    };
    for (const Case& testCase : cases) {
        SCOPED_TRACE(testCase.fixture);

        // A prompter-shaped device session: mosID is the prompter itself,
        // ncsID the NCS it normally talks to.
        FakeClock clock;
        FakeRandom random;
        LoopbackEndpoint transport;
        imos::InMemoryMessageStore store;
        imos::SessionConfig config = TwoRoleFixture::sessionConfig();
        config.mosID = "prompt.station.com";
        config.ncsID = "ncs.station.com";
        imos::DeviceConfig deviceConfig{
            config, TwoRoleFixture::machineInfo(imos::profiles::DeviceType::Mos)};
        imos::Device prompter{
            std::move(deviceConfig),
            imos::PeerSession::Dependencies{&clock, &transport, &store, nullptr, &random, nullptr}};
        transport.connected[static_cast<std::size_t>(Channel::Ro)] = true;
        prompter.onChannelConnected(Channel::Ro);

        const Message fixture = loadFixture(testCase.fixture);
        ASSERT_TRUE(prompter.sendItemCue(fixtureCue(fixture), testCase.addressee).ok());
        ASSERT_FALSE(roQueue(transport).empty());
        const Message built = parseFrame(roQueue(transport).front());

        EXPECT_EQ(built.type(), MessageType::RoItemCue);
        EXPECT_EQ(built.mosID().value_or(""), "prompt.station.com");
        EXPECT_EQ(built.ncsID().value_or(""), testCase.expectedNcsID);
        EXPECT_EQ(serializeXmlNode(dropBlankText(*built.body())),
                  serializeXmlNode(dropBlankText(*fixture.body())));
    }
}

// 14. Cues flow device → NCS too: the NCS ingests and acks, and the
//     device's exchange resolves.
TEST(Profile5, DeviceOriginatedCueReachesNcs) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergeRoWithItem(fx, "RO1");

    ItemCue cue;
    cue.mosID = TwoRoleFixture::kMosID;
    cue.roID = "RO1";
    cue.storyID = "s1";
    cue.itemID = "i1";
    cue.roEventType = "PLAY";
    cue.roEventTime = "2026-07-16T12:00:00";
    ASSERT_TRUE(fx.device->sendItemCue(cue).ok());
    fx.settle();

    std::optional<ItemCue> received = fx.ncs->nextItemCue();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->roEventType, "PLAY");
    EXPECT_EQ(received->itemID, "i1");

    bool acked = false;
    while (std::optional<imos::SessionEvent> event = fx.device->nextEvent()) {
        if (event->type == imos::SessionEventType::AckReceived &&
            event->message.type() == MessageType::RoAck) {
            acked = !imos::profiles::roAckIsNack(event->message);
        }
    }
    EXPECT_TRUE(acked);
}

// 15. The typed origination API refuses IDs that contradict the scope —
//     the device never has to guess at an inconsistent shape we built.
TEST(Profile5, SendCtrlValidatesScopeShape) {
    TwoRoleFixture fx;
    fx.connectAll();

    EXPECT_FALSE(fx.ncs->sendCtrl(CtrlScope::RunningOrder, "", "", "", CtrlCommand::Ready).ok());
    EXPECT_FALSE(
        fx.ncs->sendCtrl(CtrlScope::RunningOrder, "RO1", "s1", "", CtrlCommand::Ready).ok());
    EXPECT_FALSE(fx.ncs->sendCtrl(CtrlScope::Story, "RO1", "", "", CtrlCommand::Ready).ok());
    EXPECT_FALSE(fx.ncs->sendCtrl(CtrlScope::Story, "RO1", "s1", "i1", CtrlCommand::Ready).ok());
    EXPECT_FALSE(fx.ncs->sendCtrl(CtrlScope::Item, "RO1", "s1", "", CtrlCommand::Ready).ok());
    EXPECT_TRUE(roQueue(fx.ncsTransport).empty());  // nothing reached the wire
}

// 16. The whole exchange in both §3.4 wire encodings (UCS-2 BE binary and
//     UTF-8 text): control at item scope and a metadata-carrying cue.
TEST(Profile5, ControlAndCueRoundTripInBothWireEncodings) {
    for (const WireEncoding encoding : {WireEncoding::Ucs2Be, WireEncoding::Utf8}) {
        SCOPED_TRACE(encoding == WireEncoding::Ucs2Be ? "UCS-2 BE" : "UTF-8");
        TwoRoleFixture::Options options;
        options.wireEncoding = encoding;
        TwoRoleFixture fx{options};
        fx.connectAll();
        convergeRoWithItem(fx, "RO1");
        RecordingControlSink sink;
        fx.device->attach(sink);

        ASSERT_TRUE(
            fx.ncs->sendCtrl(CtrlScope::Item, "RO1", "s1", "i1", CtrlCommand::Execute).ok());
        fx.settle();
        ASSERT_EQ(sink.ctrls.size(), 1U);
        EXPECT_EQ(sink.ctrls.back().scope, CtrlScope::Item);
        EXPECT_TRUE(nextCtrlResult(fx).ok);

        ItemCue cue = fixtureCue(loadFixture("roItemCue.xml"));
        cue.roID = "RO1";
        cue.storyID = "s1";
        cue.itemID = "i1";
        ASSERT_TRUE(fx.ncs->sendItemCue(cue).ok());
        fx.settle();
        ASSERT_EQ(sink.cues.size(), 1U);
        EXPECT_EQ(sink.cues.back().roEventType, "Prompter");
        ASSERT_EQ(sink.cues.back().metadata.size(), 1U);
        EXPECT_TRUE(nextCtrlResult(fx).ok);
    }
}
