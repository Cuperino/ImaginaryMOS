// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 7 — MOS RO / Content List Modification conformance (plan §3.2 row 7,
// reference/profile7-plan.md §4, spec §3.9.1, milestone M4). A real imos::Device
// against a real incs::Ncs over the loopback pair: the device originates
// roReqStoryAction (NEW/UPDATE/DELETE/MOVE wrapping a roStorySend), the NCS is
// authoritative — it validates, consults the lease table, mutates the authoring
// store, and answers roAck; the requesting device converges only through the
// ordinary P2/P4 echo (never self-applies). Covers: built messages match the
// five gated spec fixtures; per-operation round-trips mutate the store; the
// no-self-apply invariant; Read1stMEMasBody payload fidelity in both wire
// encodings; single-holder lease grant/conflict/expiry; strict-vs-lenient
// capability gating (advertised via listMachInfo mosProfile 7); and the NACK
// negatives (unknown operation, missing story/roID).

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/message.h>
#include <imos/codec/story_body.h>
#include <imos/codec/xml.h>
#include <imos/encoding/encoding.h>
#include <imos/profiles/profile2.h>
#include <imos/profiles/profile4.h>
#include <imos/profiles/profile7.h>
#include <incs/profile7_ncs.h>
#include <incs/running_order_author.h>

#include "fakes.h"
#include "two_role_fixture.h"

using imos::Channel;
using imos::codec::Message;
using imos::codec::MessageType;
using imos::codec::ParagraphRuns;
using imos::codec::parseMessage;
using imos::codec::serializeXmlNode;
using imos::codec::XmlNode;
using imos::encoding::WireEncoding;
using imos::profiles::StoryAction;
using imos::profiles::StoryActionRequest;
using imos::test::FakeClock;
using imos::test::LoopbackEndpoint;
using imos::test::TwoRoleFixture;
using incs::Profile7NcsResponder;
using incs::RunningOrderAuthor;
using incs::SingleHolderLeasePolicy;

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

/// Parse `inner` inside a <storyBody> wrapper and return the subtree.
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

/// Host-side story consumer: records the agency payload bytes (serialized at
/// callback time) and the first paragraph's text for literal bodies.
class RecordingStorySink : public imos::profiles::IStorySink {
public:
    struct Received {
        std::string roID;
        std::string storyID;
        std::string storySlug;
        bool agency = false;
        std::string agencyBytes;
        std::string firstParagraphText;
    };

    std::vector<Received> received;

    imos::Result<void> onStory(const imos::profiles::StoryView& story) override {
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

/// Author one RO with `count` stories s1..sN (no items) and converge the
/// device's shadow model onto it.
void convergePlainRo(TwoRoleFixture& fx, const std::string& roID, int count) {
    ASSERT_TRUE(fx.ncs->author().create(roID, roID + " Rundown").ok());
    for (int i = 1; i <= count; ++i) {
        ASSERT_TRUE(
            fx.ncs->author()
                .insertStory(roID, RunningOrderAuthor::makeStory("s" + std::to_string(i), "slug"))
                .ok());
    }
    ASSERT_TRUE(fx.ncs->author().commit(roID).ok());
    fx.settle();
    ASSERT_EQ(fx.device->runningOrderModel().storyIDs(roID),
              fx.ncs->author().desired().storyIDs(roID));
    while (fx.ncs->author().nextEvent().has_value()) {
    }
}

/// The device's next resolved roReqStoryAction (its roAck outcome).
imos::profiles::StoryActionResult nextResult(TwoRoleFixture& fx) {
    std::optional<imos::profiles::StoryActionResult> result = fx.device->nextStoryActionResult();
    EXPECT_TRUE(result.has_value());
    return result.value_or(imos::profiles::StoryActionResult{});
}

/// Reconstruct the StoryActionRequest a spec fixture encodes, so the device
/// builds the byte-equivalent message.
StoryActionRequest requestFromFixture(const Message& fixture) {
    const XmlNode* body = fixture.body();
    const XmlNode* rss = body->firstChildElement("roStorySend");
    StoryActionRequest req{};
    req.roID = rss->childText("roID");
    req.username = std::string(body->attribute("username").value_or(std::string_view{}));
    if (const auto lease = body->attribute("leaseLock"); lease.has_value()) {
        req.leaseSeconds = std::stoi(std::string(*lease));
    }
    const XmlNode* target = rss->firstChildElement("element_target");
    const XmlNode* source = rss->firstChildElement("element_source");
    const std::string op{body->attribute("operation").value_or(std::string_view{})};
    if (op == "NEW") {
        req.operation = StoryAction::New;
        if (target != nullptr) {
            req.beforeStoryID = target->childText("storyID");
        }
        if (source != nullptr) {
            if (const XmlNode* story = source->firstChildElement("story")) {
                req.story = *story;
            }
        }
    } else if (op == "UPDATE") {
        req.operation = StoryAction::Update;
        req.storyID = rss->childText("storyID");
        req.storySlug = rss->childText("storySlug");
        req.storyNum = rss->childText("storyNum");
        if (const XmlNode* sb = rss->firstChildElement("storyBody")) {
            req.storyBody = *sb;
        }
    } else if (op == "DELETE") {
        req.operation = StoryAction::Delete;
        req.storyID = rss->childText("storyID");
    } else if (op == "MOVE") {
        req.operation = StoryAction::Move;
        if (target != nullptr) {
            req.beforeStoryID = target->childText("storyID");
        }
        if (source != nullptr) {
            for (const XmlNode& child : source->children) {
                if (child.isElement("storyID")) {
                    req.storyIDs.push_back(child.text());
                }
            }
        }
    }
    return req;
}

/// A roReqStoryAction subtree with any direct-child <mosExternalMetadata>
/// removed. The device's UPDATE builder models the two body forms (literal
/// script / agency payload) and does not originate story-level external
/// metadata, so that node in the spec's UPDATE example is out of the
/// build-match's scope; every other element/attribute is compared.
XmlNode withoutTrailingMetadata(const XmlNode& reqStoryAction) {
    XmlNode copy = reqStoryAction;
    for (XmlNode& child : copy.children) {
        if (child.isElement("roStorySend")) {
            auto& kids = child.children;
            kids.erase(
                std::remove_if(kids.begin(), kids.end(),
                               [](const XmlNode& n) { return n.isElement("mosExternalMetadata"); }),
                kids.end());
        }
    }
    return copy;
}

/// Drop pure-formatting (all-whitespace) text-node children recursively. The
/// codec retains the spec fixtures' hand-indentation as whitespace text nodes
/// for byte-clean round-trips; the device builds the tree programmatically with
/// none, so the comparison ignores that formatting while keeping every element,
/// attribute, and significant text run.
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

/// Structural canonical form for the build-match: trailing story-level metadata
/// removed, formatting whitespace removed, then serialized.
std::string structuralForm(const XmlNode& reqStoryAction) {
    return serializeXmlNode(dropBlankText(withoutTrailingMetadata(reqStoryAction)));
}

/// Prime the NCS with the device's advertised machine info by making the device
/// answer a reqMachInfo (its listMachInfo carries the real profile flags).
void exchangeMachineInfo(TwoRoleFixture& fx) {
    const std::string reqXml = "<mos><mosID>" + std::string{TwoRoleFixture::kMosID} +
                               "</mosID><ncsID>" + std::string{TwoRoleFixture::kNcsID} +
                               "</ncsID><messageID>MI-1</messageID><reqMachInfo/></mos>";
    fx.device->onFrameReceived(Channel::Ro, encode(reqXml));
    fx.settle();
}

/// Build a device roReqStoryAction from `req` and return the on-wire message.
Message buildOnWire(TwoRoleFixture& fx, const StoryActionRequest& req) {
    auto& queue = roQueue(fx.deviceTransport);
    queue.clear();
    EXPECT_TRUE(fx.device->requestStoryAction(req).ok());
    EXPECT_FALSE(queue.empty());
    return parseFrame(queue.front());
}

/// A raw roReqStoryAction envelope addressed device → NCS.
std::string reqStoryActionXml(const std::string& messageID, const std::string& operation,
                              const std::string& roStorySendInner) {
    return "<mos><mosID>" + std::string{TwoRoleFixture::kMosID} + "</mosID><ncsID>" +
           std::string{TwoRoleFixture::kNcsID} + "</ncsID><messageID>" + messageID +
           "</messageID><roReqStoryAction operation=\"" + operation + "\"><roStorySend>" +
           roStorySendInner + "</roStorySend></roReqStoryAction></mos>";
}

}  // namespace

// 1. The device builds each of the five gated spec fixtures byte-for-byte
//    (spec §3.9.1: NEW/MOVE use element_target?/element_source; UPDATE/DELETE
//    use the flat roStorySend), attributes and all.
TEST(Profile7, BuiltMessagesMatchSpecFixtures) {
    for (const char* name :
         {"roReqStoryAction.xml", "roReqStoryAction-2.xml", "roReqStoryAction-3.xml",
          "roReqStoryAction-4.xml", "roReqStoryAction-5.xml"}) {
        SCOPED_TRACE(name);
        TwoRoleFixture fx;
        fx.connectAll();
        const Message fixture = loadFixture(name);
        const Message built = buildOnWire(fx, requestFromFixture(fixture));
        EXPECT_EQ(built.type(), MessageType::RoReqStoryAction);
        EXPECT_EQ(structuralForm(*built.body()), structuralForm(*fixture.body()));
    }
}

// 2. NEW inserts the story in the NCS store and reaches the device via the
//    ordinary P2 echo; the device never self-applies (its model is untouched
//    until it settles), and the ack carries the assigned storyID.
TEST(Profile7, NewInsertsStoryAndDeviceWaitsForEcho) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergePlainRo(fx, "RO1", 1);

    StoryActionRequest req{};
    req.operation = StoryAction::New;
    req.roID = "RO1";
    req.beforeStoryID = "s1";
    req.story = RunningOrderAuthor::makeStory("sNew", "New Story", "N1");
    req.username = "editor";
    ASSERT_TRUE(fx.device->requestStoryAction(req).ok());

    // No self-apply: the request is on the wire but nothing pumped yet.
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"), (std::vector<std::string>{"s1"}));

    fx.settle();

    EXPECT_EQ(fx.ncs->author().desired().storyIDs("RO1"), (std::vector<std::string>{"sNew", "s1"}));
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"),
              (std::vector<std::string>{"sNew", "s1"}));

    const imos::profiles::StoryActionResult result = nextResult(fx);
    EXPECT_TRUE(result.ok) << result.roStatus;
    EXPECT_EQ(result.assignedStoryID, "sNew");
}

// 3. NEW with an empty storyID lets the NCS assign one, returned in roStatus.
TEST(Profile7, NewWithEmptyStoryIdGetsNcsAssignedId) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergePlainRo(fx, "RO1", 1);

    StoryActionRequest req{};
    req.operation = StoryAction::New;
    req.roID = "RO1";
    req.story = RunningOrderAuthor::makeStory("", "Auto", "A1");
    ASSERT_TRUE(fx.device->requestStoryAction(req).ok());
    fx.settle();

    const imos::profiles::StoryActionResult result = nextResult(fx);
    ASSERT_TRUE(result.ok) << result.roStatus;
    EXPECT_FALSE(result.assignedStoryID.empty());

    const std::vector<std::string> ids = fx.ncs->author().desired().storyIDs("RO1");
    EXPECT_NE(std::find(ids.begin(), ids.end(), result.assignedStoryID), ids.end());
    EXPECT_TRUE(fx.device->runningOrderModel().story("RO1", result.assignedStoryID) != nullptr);
}

// 4. UPDATE replaces the story header (slug/num carried verbatim, per spec
//    "replaces an existing Story") and delivers the new body to the sink.
TEST(Profile7, UpdateReplacesHeaderAndBody) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergePlainRo(fx, "RO1", 1);

    RecordingStorySink sink;
    fx.device->attach(sink);

    StoryActionRequest req{};
    req.operation = StoryAction::Update;
    req.roID = "RO1";
    req.storyID = "s1";
    req.storySlug = "Rewritten";
    req.storyNum = "B2";
    req.storyBody = makeStoryBody("<p>Updated take</p>");
    ASSERT_TRUE(fx.device->requestStoryAction(req).ok());
    fx.settle();

    const XmlNode* stored = fx.ncs->author().desired().story("RO1", "s1");
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(stored->childText("storySlug"), "Rewritten");
    EXPECT_EQ(stored->childText("storyNum"), "B2");

    const XmlNode* deviceStory = fx.device->runningOrderModel().story("RO1", "s1");
    ASSERT_NE(deviceStory, nullptr);
    EXPECT_EQ(deviceStory->childText("storySlug"), "Rewritten");

    ASSERT_EQ(sink.received.size(), 1U);
    EXPECT_EQ(sink.received[0].firstParagraphText, "Updated take");
    EXPECT_EQ(nextResult(fx).roStatus, "OK");
}

// 5. M4 payload fidelity: an UPDATE wrapping a Read1stMEMasBody agency body
//    (NewsML) reaches the device sink byte-identical, in both wire encodings.
TEST(Profile7, UpdateWithAgencyPayloadPreservesBytesInBothEncodings) {
    for (const WireEncoding encoding : {WireEncoding::Ucs2Be, WireEncoding::Utf8}) {
        SCOPED_TRACE(imos::encoding::wireEncodingName(encoding));
        TwoRoleFixture::Options options;
        options.wireEncoding = encoding;
        TwoRoleFixture fx{options};
        fx.connectAll();
        convergePlainRo(fx, "RO1", 1);

        std::string newsml = "<NewsML>\n<NewsItem>\n  <Identification année=\"2026\"/>\n";
        for (int i = 0; i < 16; ++i) {
            newsml += "  <p>Dépêche nº" + std::to_string(i) + " — «détaillé» ±120; Ωμέγα.\t</p>\n";
        }
        newsml += "</NewsItem>\n</NewsML>";
        const XmlNode metadata = makeAgencyMetadata(newsml);
        const std::string authoredBytes = serializeXmlNode(metadata);

        RecordingStorySink sink;
        fx.device->attach(sink);

        StoryActionRequest req{};
        req.operation = StoryAction::Update;
        req.roID = "RO1";
        req.storyID = "s1";
        req.agencyMetadata = metadata;
        ASSERT_TRUE(fx.device->requestStoryAction(req).ok());
        fx.settle();

        ASSERT_EQ(sink.received.size(), 1U);
        ASSERT_TRUE(sink.received[0].agency);
        EXPECT_EQ(sink.received[0].agencyBytes, authoredBytes);
        EXPECT_TRUE(nextResult(fx).ok);
    }
}

// 6. DELETE removes the story; again the device converges only via the echo.
TEST(Profile7, DeleteRemovesStoryAndDeviceWaitsForEcho) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergePlainRo(fx, "RO1", 2);

    StoryActionRequest req{};
    req.operation = StoryAction::Delete;
    req.roID = "RO1";
    req.storyID = "s1";
    req.username = "editor";
    ASSERT_TRUE(fx.device->requestStoryAction(req).ok());

    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"),
              (std::vector<std::string>{"s1", "s2"}));

    fx.settle();

    EXPECT_EQ(fx.ncs->author().desired().storyIDs("RO1"), (std::vector<std::string>{"s2"}));
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"), (std::vector<std::string>{"s2"}));
    EXPECT_TRUE(nextResult(fx).ok);
}

// 7. MOVE reorders one or more stories in a single request.
TEST(Profile7, MoveReordersStories) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergePlainRo(fx, "RO1", 3);

    StoryActionRequest req{};
    req.operation = StoryAction::Move;
    req.roID = "RO1";
    req.beforeStoryID = "s1";
    req.storyIDs = {"s2", "s3"};
    ASSERT_TRUE(fx.device->requestStoryAction(req).ok());
    fx.settle();

    EXPECT_EQ(fx.ncs->author().desired().storyIDs("RO1"),
              (std::vector<std::string>{"s2", "s3", "s1"}));
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"),
              (std::vector<std::string>{"s2", "s3", "s1"}));
    EXPECT_TRUE(nextResult(fx).ok);
}

// 8. Lease handling (spec: the MOS holds the lock for leaseLock seconds; the
//    NCS reclaims on expiry): a granted lease NACKs a conflicting request from
//    a different holder, and is reclaimable once it expires.
TEST(Profile7, LeaseBlocksConflictingRequestUntilExpiry) {
    TwoRoleFixture fx;
    fx.connectAll();
    convergePlainRo(fx, "RO1", 1);

    const auto update = [&](const std::string& take) {
        StoryActionRequest req{};
        req.operation = StoryAction::Update;
        req.roID = "RO1";
        req.storyID = "s1";
        req.leaseSeconds = 5;
        req.storyBody = makeStoryBody("<p>" + take + "</p>");
        EXPECT_TRUE(fx.device->requestStoryAction(req).ok());
        fx.settle();
        return nextResult(fx);
    };

    EXPECT_TRUE(update("one").ok);  // grants the lease (held past success)

    const imos::profiles::StoryActionResult conflict =
        update("two");  // different holder, unexpired
    EXPECT_FALSE(conflict.ok);
    EXPECT_NE(conflict.roStatus.find("locked"), std::string::npos) << conflict.roStatus;

    fx.clock.advanceSeconds(6);       // lease expires
    EXPECT_TRUE(update("three").ok);  // reclaimable
}

// 9. The default lease policy in isolation: single holder, TTL from
//    durationSeconds, same-holder renewal, expiry, and release.
TEST(Profile7Lease, SingleHolderGrantConflictExpiryRelease) {
    FakeClock clock;
    SingleHolderLeasePolicy policy;

    EXPECT_TRUE(policy.acquire("ro", "s", "h1", 5, clock.now()).ok());   // grant
    EXPECT_FALSE(policy.acquire("ro", "s", "h2", 5, clock.now()).ok());  // conflict
    EXPECT_TRUE(policy.acquire("ro", "s", "h1", 5, clock.now()).ok());   // same-holder renewal

    clock.advanceSeconds(10);
    EXPECT_TRUE(policy.acquire("ro", "s", "h2", 5, clock.now()).ok());  // expired → h2 takes it

    policy.release("ro", "s", "h2");
    EXPECT_TRUE(policy.acquire("ro", "s", "h1", 5, clock.now()).ok());  // released → h1 takes it
}

// 10. Strict capability gating accepts a peer that advertised Profile 7.
TEST(Profile7, StrictModeAcceptsAdvertisedPeer) {
    TwoRoleFixture fx;  // advertiseProfile7 defaults on
    fx.connectAll();
    fx.ncs->setStoryActionCapabilityPolicy(Profile7NcsResponder::CapabilityPolicy::Strict);
    exchangeMachineInfo(fx);  // the NCS learns the device advertises Profile 7
    convergePlainRo(fx, "RO1", 1);

    StoryActionRequest req{};
    req.operation = StoryAction::Delete;
    req.roID = "RO1";
    req.storyID = "s1";
    ASSERT_TRUE(fx.device->requestStoryAction(req).ok());
    fx.settle();

    EXPECT_TRUE(nextResult(fx).ok);
}

// 11. Strict gating NACKs a peer that never advertised Profile 7.
TEST(Profile7, StrictModeRejectsUnadvertisedPeer) {
    TwoRoleFixture::Options options;
    options.advertiseProfile7 = false;
    TwoRoleFixture fx{options};
    fx.connectAll();
    fx.ncs->setStoryActionCapabilityPolicy(Profile7NcsResponder::CapabilityPolicy::Strict);
    exchangeMachineInfo(fx);  // the NCS learns the device does NOT advertise Profile 7
    convergePlainRo(fx, "RO1", 1);

    StoryActionRequest req{};
    req.operation = StoryAction::Delete;
    req.roID = "RO1";
    req.storyID = "s1";
    ASSERT_TRUE(fx.device->requestStoryAction(req).ok());
    fx.settle();

    const imos::profiles::StoryActionResult result = nextResult(fx);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.roStatus.find("Profile 7"), std::string::npos) << result.roStatus;
    // The story survived the rejected request.
    EXPECT_EQ(fx.ncs->author().desired().storyIDs("RO1"), (std::vector<std::string>{"s1"}));
}

// 12. Lenient gating (the default) accepts an unadvertised peer and records an
//     audit entry (attribution + verdict).
TEST(Profile7, LenientModeAcceptsUnadvertisedPeerAndAudits) {
    TwoRoleFixture::Options options;
    options.advertiseProfile7 = false;
    TwoRoleFixture fx{options};
    fx.connectAll();
    convergePlainRo(fx, "RO1", 1);

    StoryActionRequest req{};
    req.operation = StoryAction::Delete;
    req.roID = "RO1";
    req.storyID = "s1";
    req.username = "editor";
    ASSERT_TRUE(fx.device->requestStoryAction(req).ok());
    fx.settle();

    EXPECT_TRUE(nextResult(fx).ok);
    std::optional<incs::StoryActionAudit> audit = fx.ncs->nextStoryActionAudit();
    ASSERT_TRUE(audit.has_value());
    EXPECT_TRUE(audit->accepted);
    EXPECT_EQ(audit->username, "editor");
    EXPECT_EQ(audit->operation, StoryAction::Delete);
}

// 13. Negatives: the NCS NACKs an unknown operation, a NEW with no story, and
//     a missing roID (spec §3.9.1: "A NACK response is perfectly valid").
TEST(Profile7, NcsNacksMalformedRequests) {
    TwoRoleFixture fx;
    fx.connectAll();

    const auto inject = [&](const std::string& messageID, const std::string& operation,
                            const std::string& inner) {
        auto& queue = roQueue(fx.ncsTransport);
        queue.clear();
        fx.ncs->onFrameReceived(Channel::Ro,
                                encode(reqStoryActionXml(messageID, operation, inner)));
        EXPECT_FALSE(queue.empty());
        return parseFrame(queue.back());
    };

    const Message unknownOp = inject("9001", "FROB", "<roID>RO1</roID>");
    EXPECT_EQ(unknownOp.type(), MessageType::RoAck);
    EXPECT_TRUE(imos::profiles::roAckIsNack(unknownOp));
    EXPECT_NE(imos::profiles::roAckStatusText(unknownOp).find("unknown operation"),
              std::string::npos);

    const Message newNoStory = inject("9002", "NEW", "<roID>RO1</roID>");
    EXPECT_TRUE(imos::profiles::roAckIsNack(newNoStory));
    EXPECT_NE(imos::profiles::roAckStatusText(newNoStory).find("missing story"), std::string::npos);

    const Message noRoID =
        inject("9003", "DELETE", "<roID></roID><storyID>s1</storyID><storyBody/>");
    EXPECT_TRUE(imos::profiles::roAckIsNack(noRoID));
    EXPECT_NE(imos::profiles::roAckStatusText(noRoID).find("missing roID"), std::string::npos);
}
