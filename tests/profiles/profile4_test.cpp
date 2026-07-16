// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 4 — Advanced RO / Content List Workflow conformance (plan §3.2
// row 4, §10, M4). A real imos::Device against a real incs::Ncs over the
// loopback pair: cold-start hydration (roReqAll → headers-only roListAll →
// roReq per RO → model convergence, plus the hydrate-on-connect option),
// story delivery (roStorySend resolved through the storyBody helpers,
// replace-on-resend, sink NACK propagation), the M4 exit criterion that a
// Read1stMEMasBody agency payload (NewsML) reaches the sink byte-verbatim
// in both wire encodings, the mandatory MOVE rule for story-send-linked ROs
// (no DELETE+INSERT, no wholesale roReplace collapse — with the unflagged
// control still collapsing), the §3.3 resync escalation for a roStorySend
// naming an unknown RO, oversized-input rejection ahead of profile logic,
// and the pre-M9 scale probes (recorded, not gated).

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/message.h>
#include <imos/codec/story_body.h>
#include <imos/encoding/encoding.h>
#include <imos/profiles/profile4.h>

#include "two_role_fixture.h"

using imos::Channel;
using imos::codec::Message;
using imos::codec::MessageType;
using imos::codec::ParagraphRuns;
using imos::codec::parseMessage;
using imos::codec::serializeXmlNode;
using imos::codec::XmlNode;
using imos::encoding::WireEncoding;
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

/// settle(), recording every NCS → device frame on the ro channel. This is
/// how the MOVE-rule regression looks at the literal wire sequence.
std::vector<Message> settleCapturingNcsRoWire(TwoRoleFixture& fx,
                                              WireEncoding encoding = WireEncoding::Ucs2Be,
                                              int maxRounds = 64) {
    std::vector<Message> sent;
    for (int round = 0; round < maxRounds; ++round) {
        fx.device->poll();
        fx.ncs->poll();
        for (const auto& frame : roQueue(fx.ncsTransport)) {
            sent.push_back(parseFrame(frame, encoding));
        }
        if (!fx.pump()) {
            break;
        }
    }
    return sent;
}

/// Parse `inner` inside a <storyBody> wrapper and return the subtree, so
/// authored bodies in tests are ordinary retained XML.
XmlNode makeStoryBody(const std::string& inner) {
    const std::string xml =
        "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>1</messageID>"
        "<roStorySend><roID>r</roID><storyID>s</storyID><storyBody>" +
        inner + "</storyBody></roStorySend></mos>";
    auto parsed = parseMessage(xml);
    EXPECT_TRUE(parsed.ok());
    return *parsed.value().body()->firstChildElement("storyBody");
}

/// Parse a <mosExternalMetadata> block (agency form) the same way.
XmlNode makeAgencyMetadata(const std::string& payloadInner) {
    const std::string xml =
        "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>1</messageID>"
        "<roStorySend><roID>r</roID><storyID>s</storyID><storyBody/>"
        "<mosExternalMetadata><mosScope>STORY</mosScope>"
        "<mosSchema>http://agency.example/newsml</mosSchema>"
        "<mosPayload>" +
        payloadInner + "</mosPayload></mosExternalMetadata></roStorySend></mos>";
    auto parsed = parseMessage(xml);
    EXPECT_TRUE(parsed.ok());
    return *parsed.value().body()->firstChildElement("mosExternalMetadata");
}

/// Host-side story consumer: records what arrived (agency payload bytes
/// serialized at callback time) and can be told to reject one story.
class RecordingStorySink : public imos::profiles::IStorySink {
public:
    struct Received {
        std::string roID;
        std::string storyID;
        std::string storySlug;
        bool agency = false;
        std::string agencyBytes;  ///< serializeXmlNode of the MEM, at callback time
        std::string firstParagraphText;
    };

    std::vector<Received> received;
    std::map<std::string, std::string> rejectWith;  ///< storyID → NACK detail

    imos::Result<void> onStory(const imos::profiles::StoryView& story) override {
        if (const auto it = rejectWith.find(story.storyID); it != rejectWith.end()) {
            return imos::makeError(imos::ErrorCode::WrongState, it->second);
        }
        Received entry;
        entry.roID = story.roID;
        entry.storyID = story.storyID;
        entry.storySlug = story.storySlug;
        if (story.body.bodyIsAgencyPayload()) {
            entry.agency = true;
            entry.agencyBytes = serializeXmlNode(*story.body.agencyMetadata);
        } else if (const XmlNode* p = story.body.storyBody->firstChildElement("p")) {
            for (const ParagraphRuns::Run& run : ParagraphRuns{*p}) {
                entry.firstParagraphText += run.text;
            }
        }
        received.push_back(std::move(entry));
        return {};
    }
};

/// Author one RO with `count` stories s1..sN (no items) and converge.
void convergePlainRo(TwoRoleFixture& fx, const std::string& roID, int count) {
    ASSERT_TRUE(fx.ncs->author().create(roID, roID + " Rundown").ok());
    for (int i = 1; i <= count; ++i) {
        ASSERT_TRUE(
            fx.ncs->author().insertStory(roID, storyWithItems("s" + std::to_string(i), 0)).ok());
    }
    ASSERT_TRUE(fx.ncs->author().commit(roID).ok());
    fx.settle();
    ASSERT_EQ(fx.device->runningOrderModel().storyIDs(roID),
              fx.ncs->author().desired().storyIDs(roID));
    for (const incs::CommitEvent& event : drainCommits(*fx.ncs)) {
        ASSERT_TRUE(event.ok) << event.roStatus;
    }
}

}  // namespace

// 1. Cold-start hydration: roReqAll returns every MOS-active RO as a header,
//    and the hydrate option fans out roReq per RO until the shadow model
//    matches the authoring store — with no ResyncNeeded noise (nothing was
//    out of sync) and the author's device projection primed for incremental
//    commits.
TEST(Profile4, ColdDeviceHydratesFromRoReqAll) {
    TwoRoleFixture fx;
    fx.connectAll();

    ASSERT_TRUE(fx.ncs->author().create("RO1", "5PM Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", storyWithItems("s1", 2)).ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", storyWithItems("s2", 1)).ok());
    ASSERT_TRUE(fx.ncs->author().create("RO2", "6PM Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO2", storyWithItems("s3", 1)).ok());

    ASSERT_TRUE(fx.device->requestAllRunningOrders(true).ok());
    fx.settle();

    EXPECT_EQ(fx.device->runningOrderHeaderIDs(), (std::vector<std::string>{"RO1", "RO2"}));
    const XmlNode* header = fx.device->runningOrderHeader("RO1");
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->firstChildElement("roSlug")->text(), "5PM Rundown");

    const auto& model = fx.device->runningOrderModel();
    ASSERT_TRUE(model.has("RO1"));
    ASSERT_TRUE(model.has("RO2"));
    EXPECT_EQ(model.storyIDs("RO1"), fx.ncs->author().desired().storyIDs("RO1"));
    EXPECT_EQ(model.itemIDs("RO1", "s1"), fx.ncs->author().desired().itemIDs("RO1", "s1"));
    EXPECT_EQ(model.storyIDs("RO2"), fx.ncs->author().desired().storyIDs("RO2"));
    EXPECT_EQ(countResyncEvents(*fx.device), 0);  // hydration is not a resync

    // The roReq answers primed the author's projection: the next edit
    // commits incrementally (one roElementAction — SWAP, since reversing
    // two stories is a pure transposition), not as a wholesale send.
    ASSERT_TRUE(fx.ncs->author().moveStory("RO1", "s2", "s1").ok());
    auto seqs = fx.ncs->author().commit("RO1");
    ASSERT_TRUE(seqs.ok());
    ASSERT_EQ(seqs.value().size(), 1U);
    Message sent = parseFrame(roQueue(fx.ncsTransport)[0]);
    EXPECT_EQ(sent.type(), MessageType::RoElementAction);
    EXPECT_EQ(sent.body()->attribute("operation").value_or(""), "SWAP");
    fx.settle();
    EXPECT_EQ(model.storyIDs("RO1"), fx.ncs->author().desired().storyIDs("RO1"));
}

// 2. The hydrate-on-connect option runs the same flow with no host code.
TEST(Profile4, HydrateOnConnectConvergesWithoutHostCalls) {
    TwoRoleFixture::Options options;
    options.hydrateRunningOrdersOnConnect = true;
    TwoRoleFixture fx{options};

    ASSERT_TRUE(fx.ncs->author().create("RO1", "5PM Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", storyWithItems("s1", 2)).ok());

    fx.connectAll();
    fx.settle();

    EXPECT_EQ(fx.device->runningOrderHeaderIDs(), (std::vector<std::string>{"RO1"}));
    ASSERT_TRUE(fx.device->runningOrderModel().has("RO1"));
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"),
              fx.ncs->author().desired().storyIDs("RO1"));
}

// 3. roListAll is headers-only: the <ro> content model rides through
//    (metadata included), story/item detail and non-header fields do not,
//    and without the hydrate option the model stays untouched.
TEST(Profile4, RoListAllCarriesHeadersOnly) {
    TwoRoleFixture fx;
    fx.connectAll();

    ASSERT_TRUE(fx.ncs->author().create("RO1", "5PM Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().setHeaderField("RO1", "roEdStart", "2009-07-11T17:00:00").ok());
    ASSERT_TRUE(fx.ncs->author().setHeaderField("RO1", "macroIn", "c01/l04").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", storyWithItems("s1", 2)).ok());

    ASSERT_TRUE(fx.device->requestAllRunningOrders(false).ok());
    fx.settle();

    const XmlNode* header = fx.device->runningOrderHeader("RO1");
    ASSERT_NE(header, nullptr);
    EXPECT_EQ(header->firstChildElement("roEdStart")->text(), "2009-07-11T17:00:00");
    EXPECT_EQ(header->firstChildElement("story"), nullptr);
    EXPECT_EQ(header->firstChildElement("macroIn"), nullptr);  // not in the <ro> model
    EXPECT_FALSE(fx.device->runningOrderModel().has("RO1"));   // headers ≠ hydration
}

// 4. Story delivery: an authored body arrives resolved through the sink,
//    the RO becomes story-send-linked, and a resend replaces the stored
//    story (prompter semantics), never duplicating it.
TEST(Profile4, StorySendDeliversAndReplacesOnResend) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergePlainRo(fx, "RO1", 2);

    RecordingStorySink sink;
    fx.device->attach(sink);

    ASSERT_TRUE(fx.ncs->author().setStoryBody("RO1", "s1", makeStoryBody("<p>Take one</p>")).ok());
    EXPECT_TRUE(fx.ncs->author().storySendLinked("RO1"));  // implicit with a body
    ASSERT_TRUE(fx.ncs->sendStory("RO1", "s1").ok());
    fx.settle();

    ASSERT_EQ(sink.received.size(), 1U);
    EXPECT_EQ(sink.received[0].roID, "RO1");
    EXPECT_EQ(sink.received[0].storyID, "s1");
    EXPECT_EQ(sink.received[0].storySlug, "slug");
    EXPECT_EQ(sink.received[0].firstParagraphText, "Take one");
    ASSERT_NE(fx.device->story("RO1", "s1"), nullptr);
    std::vector<incs::CommitEvent> events = drainCommits(*fx.ncs);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(events[0].ok) << events[0].roStatus;

    // Resend with a rewritten script: replaced, not appended.
    ASSERT_TRUE(fx.ncs->author().setStoryBody("RO1", "s1", makeStoryBody("<p>Take two</p>")).ok());
    ASSERT_TRUE(fx.ncs->sendStory("RO1", "s1").ok());
    fx.settle();

    ASSERT_EQ(sink.received.size(), 2U);
    EXPECT_EQ(sink.received[1].firstParagraphText, "Take two");
    EXPECT_EQ(fx.device->storyIDs("RO1"), (std::vector<std::string>{"s1"}));
    const Message* stored = fx.device->story("RO1", "s1");
    ASSERT_NE(stored, nullptr);
    auto resolved = imos::codec::resolveStoryBody(*stored);
    ASSERT_TRUE(resolved.ok());
    EXPECT_EQ(resolved.value().storyBody->firstChildElement("p")->text(), "Take two");
}

// 5. A sink rejection NACKs the roStorySend with the sink's detail, the
//    story is not retained, and the NCS sees the failure.
TEST(Profile4, SinkRejectionPropagatesNackDetail) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergePlainRo(fx, "RO1", 2);

    RecordingStorySink sink;
    sink.rejectWith["s2"] = "prompter storage full";
    fx.device->attach(sink);

    ASSERT_TRUE(fx.ncs->author().setStoryBody("RO1", "s2", makeStoryBody("<p>x</p>")).ok());
    ASSERT_TRUE(fx.ncs->sendStory("RO1", "s2").ok());
    fx.settle();

    EXPECT_EQ(fx.device->story("RO1", "s2"), nullptr);
    std::vector<incs::CommitEvent> events = drainCommits(*fx.ncs);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_FALSE(events[0].ok);
    EXPECT_EQ(events[0].roStatus, "NACK: prompter storage full");
}

// 6. roStorySend for an RO the device does not know: NACK plus the §3.3
//    resync escalation — one ResyncNeeded, roReq/roList rebuild — and the
//    story applies once resent after the rebuild.
TEST(Profile4, UnknownRunningOrderTriggersResyncThenStoryApplies) {
    TwoRoleFixture fx;
    fx.connectAll();

    // Authored but never committed: the device has no idea RO9 exists.
    ASSERT_TRUE(fx.ncs->author().create("RO9", "Late Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO9", storyWithItems("s1", 1)).ok());
    ASSERT_TRUE(fx.ncs->author().setStoryBody("RO9", "s1", makeStoryBody("<p>late</p>")).ok());

    ASSERT_TRUE(fx.ncs->sendStory("RO9", "s1").ok());
    fx.settle();

    std::vector<incs::CommitEvent> events = drainCommits(*fx.ncs);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_FALSE(events[0].ok);
    EXPECT_NE(events[0].roStatus.find("unknown roID"), std::string::npos) << events[0].roStatus;
    EXPECT_EQ(countResyncEvents(*fx.device), 1);
    // The rebuild already ran within the settle: roReq → roList → model.
    ASSERT_TRUE(fx.device->runningOrderModel().has("RO9"));
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO9"),
              fx.ncs->author().desired().storyIDs("RO9"));

    ASSERT_TRUE(fx.ncs->sendStory("RO9", "s1").ok());
    fx.settle();
    events = drainCommits(*fx.ncs);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(events[0].ok) << events[0].roStatus;
    EXPECT_NE(fx.device->story("RO9", "s1"), nullptr);
}

// 7. The MOVE rule is mandatory for story-send-linked ROs (plan §1.3): the
//    same over-threshold edit batch collapses to a wholesale roReplace on a
//    plain RO, but on a linked RO it stays an incremental roElementAction
//    sequence — MOVEs for the reorders, and never DELETE+INSERT (or a
//    replace) that would make a prompter discard scripts.
TEST(Profile4, StorySendLinkedRosNeverCollapseMovesIntoReplace) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergePlainRo(fx, "PLAIN", 8);
    convergePlainRo(fx, "PROMPT", 8);
    fx.ncs->author().setStorySendLinked("PROMPT", true);

    // Five content rewrites + two reorders = 7 messages, past the default
    // replace threshold of 6.
    const auto edit = [&](const std::string& roID) {
        for (int i = 1; i <= 5; ++i) {
            const std::string id = "s" + std::to_string(i);
            ASSERT_TRUE(fx.ncs->author().replaceStory(roID, id, storyWithItems(id, 0, "v2")).ok());
        }
        ASSERT_TRUE(fx.ncs->author().moveStory(roID, "s8", "s1").ok());
        ASSERT_TRUE(fx.ncs->author().moveStory(roID, "s7", "s6").ok());
    };

    edit("PROMPT");
    ASSERT_TRUE(fx.ncs->author().commit("PROMPT").ok());
    std::vector<Message> prompt = settleCapturingNcsRoWire(fx);
    int moves = 0;
    for (const Message& message : prompt) {
        EXPECT_NE(message.type(), MessageType::RoReplace) << "linked RO collapsed to roReplace";
        EXPECT_NE(message.type(), MessageType::RoCreate);
        EXPECT_NE(message.type(), MessageType::RoDelete);
        if (message.type() == MessageType::RoElementAction) {
            const std::string operation{message.body()->attribute("operation").value_or("")};
            EXPECT_TRUE(operation == "MOVE" || operation == "REPLACE" || operation == "SWAP")
                << "forbidden operation '" << operation << "' on a story-send-linked RO";
            moves += operation == "MOVE" ? 1 : 0;
        }
    }
    EXPECT_GE(moves, 1);
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("PROMPT"),
              fx.ncs->author().desired().storyIDs("PROMPT"));
    for (const incs::CommitEvent& event : drainCommits(*fx.ncs)) {
        EXPECT_TRUE(event.ok) << event.roStatus;
    }

    // Control: the identical batch on the unflagged RO still collapses.
    edit("PLAIN");
    ASSERT_TRUE(fx.ncs->author().commit("PLAIN").ok());
    std::vector<Message> plain = settleCapturingNcsRoWire(fx);
    int replaces = 0;
    for (const Message& message : plain) {
        replaces += message.type() == MessageType::RoReplace ? 1 : 0;
    }
    EXPECT_EQ(replaces, 1);
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("PLAIN"),
              fx.ncs->author().desired().storyIDs("PLAIN"));
}

// 8. M4 exit criterion: a multi-kilobyte NewsML payload authored as an
//    agency story (Read1stMEMasBody) reaches the device sink byte-identical
//    in both wire encodings — UCS-2 BE (binary frames, the default) and the
//    per-peer UTF-8 option.
TEST(Profile4, NewsMlAgencyPayloadIsPreservedVerbatimInBothWireEncodings) {
    for (const WireEncoding encoding : {WireEncoding::Ucs2Be, WireEncoding::Utf8}) {
        SCOPED_TRACE(imos::encoding::wireEncodingName(encoding));
        TwoRoleFixture::Options options;
        options.wireEncoding = encoding;
        TwoRoleFixture fx{options};
        fx.connectAll();
        convergePlainRo(fx, "RO1", 1);

        // Several kilobytes of NewsML with attributes, nested markup, odd
        // whitespace, and non-ASCII (BMP) characters.
        std::string newsml = "<NewsML>\n<NewsItem>\n  <Identification année=\"2026\"/>\n";
        for (int i = 0; i < 64; ++i) {
            newsml += "  <p>Dépêche nº" + std::to_string(i) +
                      " — l'aperçu «détaillé» pèse ±120 octets; Ωμέγα.\t</p>\n";
        }
        newsml += "</NewsItem>\n</NewsML>";
        const XmlNode metadata = makeAgencyMetadata(newsml);
        const std::string authoredBytes = serializeXmlNode(metadata);
        ASSERT_GT(authoredBytes.size(), 4096U);

        RecordingStorySink sink;
        fx.device->attach(sink);
        ASSERT_TRUE(fx.ncs->author().setAgencyStoryBody("RO1", "s1", metadata).ok());
        ASSERT_TRUE(fx.ncs->sendStory("RO1", "s1").ok());
        fx.settle();

        ASSERT_EQ(sink.received.size(), 1U);
        ASSERT_TRUE(sink.received[0].agency);
        EXPECT_EQ(sink.received[0].agencyBytes, authoredBytes);  // byte-exact, per plan §4

        // The retained store agrees with what the sink saw.
        const Message* stored = fx.device->story("RO1", "s1");
        ASSERT_NE(stored, nullptr);
        auto resolved = imos::codec::resolveStoryBody(*stored);
        ASSERT_TRUE(resolved.ok());
        ASSERT_TRUE(resolved.value().bodyIsAgencyPayload());
        EXPECT_EQ(serializeXmlNode(*resolved.value().agencyMetadata), authoredBytes);

        for (const incs::CommitEvent& event : drainCommits(*fx.ncs)) {
            EXPECT_TRUE(event.ok) << event.roStatus;
        }
    }
}

// 9. Hardening (plan §12): an oversized roStorySend is rejected by the
//    parse limits before any Profile 4 logic runs.
TEST(Profile4, OversizedStorySendIsRejectedByParseLimits) {
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
        "<messageID>1</messageID><roStorySend><roID>RO1</roID><storyID>s1</storyID>"
        "<storyBody><p>";
    huge.append(2048, 'x');
    huge += "</p></storyBody></roStorySend></mos>";
    device.onFrameReceived(Channel::Ro, encode(huge));

    bool sawProtocolError = false;
    while (std::optional<imos::SessionEvent> event = device.nextEvent()) {
        sawProtocolError |= event->type == imos::SessionEventType::ProtocolError;
    }
    EXPECT_TRUE(sawProtocolError);
    EXPECT_EQ(device.story("RO1", "s1"), nullptr);
}

// 10. Scale probes (pre-M9 benchmark seeds, recorded but not gated): a
//     10k-RO roListAll through the real request path, and a multi-megabyte
//     roStorySend through the parser.
TEST(Profile4, ScaleProbeTenThousandHeadersAndMultiMegabyteStory) {
    TwoRoleFixture fx;
    fx.connectAll();
    for (int i = 0; i < 10'000; ++i) {
        ASSERT_TRUE(
            fx.ncs->author().create("RO" + std::to_string(i), "Rundown " + std::to_string(i)).ok());
    }
    ASSERT_TRUE(fx.device->requestAllRunningOrders(false).ok());
    const auto listStart = std::chrono::steady_clock::now();
    fx.settle();
    const auto listMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - listStart)
                                .count();
    EXPECT_EQ(fx.device->runningOrderHeaderIDs().size(), 10'000U);
    RecordProperty("roListAll10kMillis", static_cast<int>(listMillis));

    std::string story =
        "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>1</messageID>"
        "<roStorySend><roID>RO0</roID><storyID>s1</storyID><storyBody>";
    while (story.size() < 2U * 1024U * 1024U) {
        story += "<p>The quick brown fox jumps over the lazy dog, at length.</p>";
    }
    story += "</storyBody></roStorySend></mos>";
    const auto parseStart = std::chrono::steady_clock::now();
    auto parsed = parseMessage(story);  // default limits: 4 MiB is within bounds
    const auto parseMillis = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - parseStart)
                                 .count();
    ASSERT_TRUE(parsed.ok());
    EXPECT_TRUE(imos::codec::resolveStoryBody(parsed.value()).ok());
    RecordProperty("roStorySend2MiBParseMillis", static_cast<int>(parseMillis));
}
