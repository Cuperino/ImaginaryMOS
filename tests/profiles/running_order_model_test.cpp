// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// RunningOrderModel unit layer (Profile 2 plan §4, milestone M3): every
// roElementAction spec fixture and the construction fixtures driven through
// apply(), the corner cases master-plan risk R-10 worries about (MOVE to
// self, SWAP adjacent, head/tail INSERT via empty target, wholesale REPLACE
// item-set changes, transactional failure), unknown-tag retention on stored
// stories, per-element status tracking, and a property-style shuffle test
// against an independent naive list re-derivation.

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/message.h>
#include <imos/profiles/running_order_model.h>

using imos::ErrorCode;
using imos::codec::Message;
using imos::codec::MessageType;
using imos::codec::XmlNode;
using imos::profiles::RoElement;
using imos::profiles::RunningOrderModel;

namespace {

Message parse(const std::string& xml) {
    imos::Result<Message> parsed = imos::codec::parseMessage(xml);
    EXPECT_TRUE(parsed.ok()) << (parsed.ok() ? "" : parsed.error().message);
    return std::move(parsed).value();
}

Message loadFixture(const std::string& name) {
    const std::string path = std::string{IMOS_FIXTURES_DIR} + "/spec-examples/" + name;
    std::ifstream file(path);
    EXPECT_TRUE(file.is_open()) << path;
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse(buffer.str());
}

/// The RO the spec's own roElementAction examples mutate: roID 5PM with
/// stories 2 (items 12, 23, 24), 3, 5, 7, 12.
constexpr const char* kBaseRo =
    "<mos><mosID>a</mosID><ncsID>n</ncsID><messageID>1</messageID><roCreate>"
    "<roID>5PM</roID><roSlug>5PM Rundown</roSlug>"
    "<story><storyID>2</storyID><storySlug>Top</storySlug>"
    "<item><itemID>12</itemID><objID>M12</objID><mosID>t</mosID></item>"
    "<item><itemID>23</itemID><objID>M23</objID><mosID>t</mosID></item>"
    "<item><itemID>24</itemID><objID>M24</objID><mosID>t</mosID></item></story>"
    "<story><storyID>3</storyID></story><story><storyID>5</storyID></story>"
    "<story><storyID>7</storyID></story><story><storyID>12</storyID></story>"
    "</roCreate></mos>";

RunningOrderModel modelWithBase() {
    RunningOrderModel model;
    EXPECT_TRUE(model.apply(parse(kBaseRo)).ok());
    return model;
}

// ---- roElementAction builders for synthesized cases --------------------

Message action(const std::string& operation, const std::string& roID) {
    Message message = imos::codec::makeMessage(MessageType::RoElementAction);
    message.body()->setAttribute("operation", operation);
    message.body()->appendElement("roID").setText(roID);
    return message;
}

void setTarget(Message& message, const std::string& storyID, const std::string& itemID = {}) {
    XmlNode& target = message.body()->appendElement("element_target");
    if (!storyID.empty()) {
        target.appendElement("storyID").setText(storyID);
    }
    if (!itemID.empty()) {
        target.appendElement("itemID").setText(itemID);
    }
}

XmlNode& source(Message& message) {
    if (XmlNode* existing = message.body()->firstChildElement("element_source")) {
        return *existing;
    }
    return message.body()->appendElement("element_source");
}

void addSourceStory(Message& message, const std::string& storyID) {
    source(message).appendElement("story").appendElement("storyID").setText(storyID);
}

void addSourceId(Message& message, const std::string& name, const std::string& value) {
    source(message).appendElement(name).setText(value);
}

}  // namespace

// ---- construction fixtures ---------------------------------------------

TEST(RunningOrderModel, ConstructionFixturesRoundTrip) {
    RunningOrderModel model;

    ASSERT_TRUE(model.apply(loadFixture("roCreate.xml")).ok());
    ASSERT_TRUE(model.has("96857485"));
    EXPECT_EQ(model.storyIDs("96857485"), (std::vector<std::string>{"5983A501:0049B924:8390EF2B",
                                                                    "3854737F:0003A34D:983A0B28"}));

    // A duplicate roCreate is wrong-state, never applied.
    const imos::Result<void> duplicate = model.apply(loadFixture("roCreate.xml"));
    ASSERT_FALSE(duplicate.ok());
    EXPECT_EQ(duplicate.error().code, ErrorCode::WrongState);

    // roReplace carries the whole RO (fixture: same roID, new story slugs).
    ASSERT_TRUE(model.apply(loadFixture("roReplace.xml")).ok());
    ASSERT_TRUE(model.has("96857485"));

    // roMetadataReplace touches the header only; the stories survive.
    const std::vector<std::string> storiesBefore = model.storyIDs("96857485");
    ASSERT_TRUE(model.apply(loadFixture("roMetadataReplace.xml")).ok());
    EXPECT_EQ(model.storyIDs("96857485"), storiesBefore);
    const XmlNode* ro = model.runningOrder("96857485");
    ASSERT_NE(ro, nullptr);
    ASSERT_NE(ro->firstChildElement("mosExternalMetadata"), nullptr);

    // roDelete for an RO we do not hold is the unknown-ID condition.
    const imos::Result<void> unknownDelete = model.apply(loadFixture("roDelete.xml"));
    ASSERT_FALSE(unknownDelete.ok());
    EXPECT_EQ(unknownDelete.error().code, ErrorCode::NotFound);
    EXPECT_TRUE(model.has("96857485"));

    // roReadyToAir (fixture roID 5PM) against the base RO.
    ASSERT_TRUE(model.apply(parse(kBaseRo)).ok());
    ASSERT_TRUE(model.apply(loadFixture("roReadyToAir.xml")).ok());
    EXPECT_EQ(model.readyToAir("5PM").value_or(""), "READY");

    // roList rebuilds wholesale (the resync completion path).
    ASSERT_TRUE(model.apply(loadFixture("roList.xml")).ok());
    EXPECT_EQ(model.storyIDs("96857485").size(), 2U);
}

// ---- every roElementAction fixture through apply() ----------------------

struct FixtureCase {
    const char* file;
    std::vector<std::string> stories;      ///< expected story order of 5PM
    std::vector<std::string> story2Items;  ///< expected item order of story 2 ("" = story gone)
};

class ElementActionFixture : public ::testing::TestWithParam<FixtureCase> {};

TEST_P(ElementActionFixture, AppliesWithSpecSemantics) {
    RunningOrderModel model = modelWithBase();
    ASSERT_TRUE(model.apply(loadFixture(GetParam().file)).ok());
    EXPECT_EQ(model.storyIDs("5PM"), GetParam().stories);
    if (model.story("5PM", "2") != nullptr) {
        EXPECT_EQ(model.itemIDs("5PM", "2"), GetParam().story2Items);
    } else {
        EXPECT_TRUE(GetParam().story2Items.empty());
    }
}

INSTANTIATE_TEST_SUITE_P(
    SpecExamples, ElementActionFixture,
    ::testing::Values(
        // INSERT story 17 before story 2.
        FixtureCase{"roElementAction.xml", {"17", "2", "3", "5", "7", "12"}, {"12", "23", "24"}},
        // INSERT item 27 before item 23 in story 2.
        FixtureCase{"roElementAction-2.xml", {"2", "3", "5", "7", "12"}, {"12", "27", "23", "24"}},
        // REPLACE story 2 with story 17 (whole new item set).
        FixtureCase{"roElementAction-3.xml", {"17", "3", "5", "7", "12"}, {}},
        // REPLACE item 23 with item 27.
        FixtureCase{"roElementAction-4.xml", {"2", "3", "5", "7", "12"}, {"12", "27", "24"}},
        // MOVE story 7 before story 2.
        FixtureCase{"roElementAction-5.xml", {"7", "2", "3", "5", "12"}, {"12", "23", "24"}},
        // MOVE stories 7 and 12 before story 2.
        FixtureCase{"roElementAction-6.xml", {"7", "12", "2", "3", "5"}, {"12", "23", "24"}},
        // MOVE items 23, 24 before item 12 (within story 2).
        FixtureCase{"roElementAction-7.xml", {"2", "3", "5", "7", "12"}, {"23", "24", "12"}},
        // DELETE story 3.
        FixtureCase{"roElementAction-8.xml", {"2", "5", "7", "12"}, {"12", "23", "24"}},
        // DELETE items 23 and 24 from story 2.
        FixtureCase{"roElementAction-9.xml", {"2", "3", "5", "7", "12"}, {"12"}},
        // SWAP stories 3 and 5.
        FixtureCase{"roElementAction-10.xml", {"2", "5", "3", "7", "12"}, {"12", "23", "24"}},
        // SWAP items 23 and 24 (within story 2).
        FixtureCase{"roElementAction-11.xml", {"2", "3", "5", "7", "12"}, {"12", "24", "23"}}));

// ---- corner cases (master-plan risk R-10) --------------------------------

TEST(RunningOrderModel, MoveToSelfLeavesOrderUnchanged) {
    RunningOrderModel model = modelWithBase();
    Message move = action("MOVE", "5PM");
    setTarget(move, "3");
    addSourceId(move, "storyID", "3");
    ASSERT_TRUE(model.apply(move).ok());
    EXPECT_EQ(model.storyIDs("5PM"), (std::vector<std::string>{"2", "3", "5", "7", "12"}));
}

TEST(RunningOrderModel, InsertAtTailViaEmptyTargetAndAtHead) {
    RunningOrderModel model = modelWithBase();
    Message tail = action("INSERT", "5PM");  // no element_target at all
    addSourceStory(tail, "99");
    ASSERT_TRUE(model.apply(tail).ok());
    Message head = action("INSERT", "5PM");
    setTarget(head, "2");
    addSourceStory(head, "0");
    ASSERT_TRUE(model.apply(head).ok());
    EXPECT_EQ(model.storyIDs("5PM"),
              (std::vector<std::string>{"0", "2", "3", "5", "7", "12", "99"}));
}

TEST(RunningOrderModel, MoveToTailViaEmptyTarget) {
    RunningOrderModel model = modelWithBase();
    Message move = action("MOVE", "5PM");
    addSourceId(move, "storyID", "2");
    ASSERT_TRUE(model.apply(move).ok());
    EXPECT_EQ(model.storyIDs("5PM"), (std::vector<std::string>{"3", "5", "7", "12", "2"}));
}

TEST(RunningOrderModel, FailedActionMutatesNothing) {
    RunningOrderModel model = modelWithBase();

    // DELETE listing one unknown story fails wholesale: 3 survives.
    Message del = action("DELETE", "5PM");
    addSourceId(del, "storyID", "3");
    addSourceId(del, "storyID", "99");
    const imos::Result<void> failedDelete = model.apply(del);
    ASSERT_FALSE(failedDelete.ok());
    EXPECT_EQ(failedDelete.error().code, ErrorCode::NotFound);
    EXPECT_EQ(model.storyIDs("5PM"), (std::vector<std::string>{"2", "3", "5", "7", "12"}));

    // INSERT of a story whose ID already exists is a protocol violation.
    Message dup = action("INSERT", "5PM");
    addSourceStory(dup, "5");
    const imos::Result<void> failedInsert = model.apply(dup);
    ASSERT_FALSE(failedInsert.ok());
    EXPECT_EQ(failedInsert.error().code, ErrorCode::ProtocolViolation);
    EXPECT_EQ(model.storyIDs("5PM"), (std::vector<std::string>{"2", "3", "5", "7", "12"}));

    // The DELETE-then-act hazard: delete story 2, then a queued action
    // targeting an item inside it fails atomically.
    Message delStory2 = action("DELETE", "5PM");
    addSourceId(delStory2, "storyID", "2");
    ASSERT_TRUE(model.apply(delStory2).ok());
    Message orphanAction = action("MOVE", "5PM");
    setTarget(orphanAction, "2", "12");
    addSourceId(orphanAction, "itemID", "23");
    const imos::Result<void> orphaned = model.apply(orphanAction);
    ASSERT_FALSE(orphaned.ok());
    EXPECT_EQ(orphaned.error().code, ErrorCode::NotFound);
    EXPECT_EQ(model.storyIDs("5PM"), (std::vector<std::string>{"3", "5", "7", "12"}));
}

TEST(RunningOrderModel, MixedSourceKindsAreRejected) {
    RunningOrderModel model = modelWithBase();
    Message mixed = action("MOVE", "5PM");
    addSourceId(mixed, "storyID", "3");
    addSourceId(mixed, "itemID", "23");
    const imos::Result<void> outcome = model.apply(mixed);
    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.error().code, ErrorCode::ProtocolViolation);
}

TEST(RunningOrderModel, SwapRequiresExactlyTwoIds) {
    RunningOrderModel model = modelWithBase();
    Message swap = action("SWAP", "5PM");
    addSourceId(swap, "storyID", "3");
    addSourceId(swap, "storyID", "5");
    addSourceId(swap, "storyID", "7");
    const imos::Result<void> outcome = model.apply(swap);
    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.error().code, ErrorCode::ProtocolViolation);
}

TEST(RunningOrderModel, UnknownRoIsTheResyncCondition) {
    RunningOrderModel model = modelWithBase();
    Message move = action("MOVE", "6PM");
    addSourceId(move, "storyID", "3");
    const imos::Result<void> outcome = model.apply(move);
    ASSERT_FALSE(outcome.ok());
    EXPECT_EQ(outcome.error().code, ErrorCode::NotFound);
}

// ---- retention and status -------------------------------------------------

TEST(RunningOrderModel, UnknownTagsRideThroughStoredStories) {
    RunningOrderModel model;
    const std::string vendorStory =
        "<story><storyID>V</storyID>"
        "<vendorTag custom=\"1\"><nested>keep me</nested></vendorTag></story>";
    ASSERT_TRUE(model
                    .apply(parse("<mos><mosID>a</mosID><ncsID>n</ncsID><messageID>9</messageID>"
                                 "<roCreate><roID>R</roID><roSlug>S</roSlug>" +
                                 vendorStory +
                                 "<story><storyID>W</storyID></story></roCreate>"
                                 "</mos>"))
                    .ok());
    // Move V to the tail; its subtree must re-serialize verbatim.
    Message move = action("MOVE", "R");
    addSourceId(move, "storyID", "V");
    ASSERT_TRUE(model.apply(move).ok());
    const XmlNode* stored = model.story("R", "V");
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(imos::codec::serializeXmlNode(*stored), vendorStory);
}

TEST(RunningOrderModel, StatusTrackingValidatesAndSurvivesMutation) {
    RunningOrderModel model = modelWithBase();

    ASSERT_TRUE(model.setStatus(RoElement::Item, "5PM", "2", "23", "PLAY").ok());
    EXPECT_EQ(model.status(RoElement::Item, "5PM", "2", "23").value_or(""), "PLAY");

    // NCS-originated roElementStat (element="RO") records at RO scope.
    Message stat = imos::codec::makeMessage(MessageType::RoElementStat);
    stat.body()->setAttribute("element", "RO");
    stat.body()->appendElement("roID").setText("5PM");
    stat.body()->appendElement("status").setText("MANUAL CTRL");
    stat.body()->appendElement("time").setText("2009-04-11T14:13:53");
    ASSERT_TRUE(model.apply(stat).ok());
    EXPECT_EQ(model.status(RoElement::RunningOrder, "5PM", "", "").value_or(""), "MANUAL CTRL");

    // Unknown IDs are the resync condition here too.
    const imos::Result<void> unknown = model.setStatus(RoElement::Story, "5PM", "99", "", "PLAY");
    ASSERT_FALSE(unknown.ok());
    EXPECT_EQ(unknown.error().code, ErrorCode::NotFound);

    // A status for a deleted element is never reported again.
    Message del = action("DELETE", "5PM");
    setTarget(del, "2");
    addSourceId(del, "itemID", "23");
    ASSERT_TRUE(model.apply(del).ok());
    EXPECT_FALSE(model.status(RoElement::Item, "5PM", "2", "23").has_value());
}

// ---- property-style shuffle test ------------------------------------------

TEST(RunningOrderModel, RandomActionSequencesMatchNaiveDerivation) {
    std::mt19937 rng{20260716};
    const auto pick = [&](std::size_t bound) { return static_cast<std::size_t>(rng() % bound); };

    for (int round = 0; round < 60; ++round) {
        RunningOrderModel model;
        ASSERT_TRUE(model
                        .apply(parse("<mos><mosID>a</mosID><ncsID>n</ncsID><messageID>1"
                                     "</messageID><roCreate><roID>R</roID><roSlug>S</roSlug>"
                                     "<story><storyID>s0</storyID></story>"
                                     "<story><storyID>s1</storyID></story>"
                                     "<story><storyID>s2</storyID></story>"
                                     "<story><storyID>s3</storyID></story></roCreate></mos>"))
                        .ok());
        std::vector<std::string> naive{"s0", "s1", "s2", "s3"};
        int nextId = 4;

        for (int step = 0; step < 24; ++step) {
            switch (naive.size() < 2 ? 0 : pick(4)) {
                case 0: {  // INSERT one new story before a random anchor / at tail
                    const std::string id = "s" + std::to_string(nextId++);
                    const std::size_t at = pick(naive.size() + 1);
                    Message insert = action("INSERT", "R");
                    if (at < naive.size()) {
                        setTarget(insert, naive[at]);
                    }
                    addSourceStory(insert, id);
                    ASSERT_TRUE(model.apply(insert).ok());
                    naive.insert(naive.begin() + static_cast<std::ptrdiff_t>(at), id);
                    break;
                }
                case 1: {  // MOVE one story before a random non-self anchor / tail
                    const std::size_t from = pick(naive.size());
                    const std::size_t to = pick(naive.size() + 1);
                    const std::string id = naive[from];
                    const std::string anchor = to < naive.size() ? naive[to] : std::string{};
                    if (anchor == id) {
                        break;  // MOVE-to-self is pinned by its own test
                    }
                    Message move = action("MOVE", "R");
                    if (!anchor.empty()) {
                        setTarget(move, anchor);
                    }
                    addSourceId(move, "storyID", id);
                    ASSERT_TRUE(model.apply(move).ok());
                    naive.erase(naive.begin() + static_cast<std::ptrdiff_t>(from));
                    const auto anchorIt = anchor.empty()
                                              ? naive.end()
                                              : std::find(naive.begin(), naive.end(), anchor);
                    naive.insert(anchorIt, id);
                    break;
                }
                case 2: {  // DELETE one story
                    const std::size_t at = pick(naive.size());
                    Message del = action("DELETE", "R");
                    addSourceId(del, "storyID", naive[at]);
                    ASSERT_TRUE(model.apply(del).ok());
                    naive.erase(naive.begin() + static_cast<std::ptrdiff_t>(at));
                    break;
                }
                default: {  // SWAP two distinct stories
                    const std::size_t a = pick(naive.size());
                    std::size_t b = pick(naive.size());
                    if (a == b) {
                        break;
                    }
                    Message swap = action("SWAP", "R");
                    addSourceId(swap, "storyID", naive[a]);
                    addSourceId(swap, "storyID", naive[b]);
                    ASSERT_TRUE(model.apply(swap).ok());
                    std::swap(naive[a], naive[b]);
                    break;
                }
            }
            ASSERT_EQ(model.storyIDs("R"), naive) << "round " << round << " step " << step;
        }
    }
}

// ---- negative table: every reject arm answers with the right vocabulary ---
//
// ErrorCode::NotFound is the §3.3 resync trigger; structural problems must
// stay ProtocolViolation/InvalidArgument (a plain NACK, never a resync).

TEST(RunningOrderModel, ActionRejectVocabulary) {
    RunningOrderModel model = modelWithBase();

    // INSERT: no payload, payload missing its key, duplicate key in payload,
    // key colliding with the sequence, unknown before-target.
    Message insertEmpty = action("INSERT", "5PM");
    setTarget(insertEmpty, "3");
    EXPECT_EQ(model.apply(insertEmpty).error().code, ErrorCode::ProtocolViolation);

    Message insertKeyless = action("INSERT", "5PM");
    setTarget(insertKeyless, "3");
    source(insertKeyless).appendElement("story").appendElement("storySlug").setText("x");
    EXPECT_EQ(model.apply(insertKeyless).error().code, ErrorCode::ProtocolViolation);

    Message insertDup = action("INSERT", "5PM");
    setTarget(insertDup, "3");
    addSourceStory(insertDup, "50");
    addSourceStory(insertDup, "50");
    EXPECT_EQ(model.apply(insertDup).error().code, ErrorCode::ProtocolViolation);

    Message insertCollide = action("INSERT", "5PM");
    setTarget(insertCollide, "3");
    addSourceStory(insertCollide, "7");
    EXPECT_EQ(model.apply(insertCollide).error().code, ErrorCode::ProtocolViolation);

    Message insertBeforeGhost = action("INSERT", "5PM");
    setTarget(insertBeforeGhost, "999");
    addSourceStory(insertBeforeGhost, "50");
    EXPECT_EQ(model.apply(insertBeforeGhost).error().code, ErrorCode::NotFound);

    // REPLACE: no payload, no target, unknown target.
    Message replaceEmpty = action("REPLACE", "5PM");
    setTarget(replaceEmpty, "3");
    EXPECT_EQ(model.apply(replaceEmpty).error().code, ErrorCode::ProtocolViolation);

    Message replaceGhost = action("REPLACE", "5PM");
    setTarget(replaceGhost, "999");
    addSourceStory(replaceGhost, "3");
    EXPECT_EQ(model.apply(replaceGhost).error().code, ErrorCode::NotFound);

    // MOVE: no ids, duplicate ids, unknown id, unknown anchor.
    Message moveEmpty = action("MOVE", "5PM");
    setTarget(moveEmpty, "3");
    EXPECT_EQ(model.apply(moveEmpty).error().code, ErrorCode::ProtocolViolation);

    Message moveDup = action("MOVE", "5PM");
    setTarget(moveDup, "3");
    addSourceId(moveDup, "storyID", "5");
    addSourceId(moveDup, "storyID", "5");
    EXPECT_EQ(model.apply(moveDup).error().code, ErrorCode::ProtocolViolation);

    Message moveGhost = action("MOVE", "5PM");
    setTarget(moveGhost, "3");
    addSourceId(moveGhost, "storyID", "999");
    EXPECT_EQ(model.apply(moveGhost).error().code, ErrorCode::NotFound);

    Message moveBadAnchor = action("MOVE", "5PM");
    setTarget(moveBadAnchor, "999");
    addSourceId(moveBadAnchor, "storyID", "5");
    EXPECT_EQ(model.apply(moveBadAnchor).error().code, ErrorCode::NotFound);

    // SWAP: an unknown id among the two.
    Message swapGhost = action("SWAP", "5PM");
    addSourceId(swapGhost, "storyID", "3");
    addSourceId(swapGhost, "storyID", "999");
    EXPECT_EQ(model.apply(swapGhost).error().code, ErrorCode::NotFound);

    // Item level inside an unknown story.
    Message itemGhostStory = action("INSERT", "5PM");
    setTarget(itemGhostStory, "999", "12");
    source(itemGhostStory).appendElement("item").appendElement("itemID").setText("77");
    EXPECT_EQ(model.apply(itemGhostStory).error().code, ErrorCode::NotFound);

    // Nothing above may have mutated the model.
    EXPECT_EQ(model.storyIDs("5PM"), (std::vector<std::string>{"2", "3", "5", "7", "12"}));
}

TEST(RunningOrderModel, ConstructionRejectVocabulary) {
    RunningOrderModel model = modelWithBase();

    // roDelete / roReadyToAir / roMetadataReplace against an unknown RO.
    EXPECT_EQ(model
                  .apply(parse("<mos><mosID>a</mosID><ncsID>n</ncsID><messageID>9</messageID>"
                               "<roDelete><roID>ghost</roID></roDelete></mos>"))
                  .error()
                  .code,
              ErrorCode::NotFound);
    EXPECT_EQ(model
                  .apply(parse("<mos><mosID>a</mosID><ncsID>n</ncsID><messageID>9</messageID>"
                               "<roReadyToAir><roID>ghost</roID><roAir>READY</roAir>"
                               "</roReadyToAir></mos>"))
                  .error()
                  .code,
              ErrorCode::NotFound);
    EXPECT_EQ(model
                  .apply(parse("<mos><mosID>a</mosID><ncsID>n</ncsID><messageID>9</messageID>"
                               "<roMetadataReplace><roID>ghost</roID><roSlug>s</roSlug>"
                               "</roMetadataReplace></mos>"))
                  .error()
                  .code,
              ErrorCode::NotFound);
    // A roCreate of an existing roID is WrongState (use roReplace).
    EXPECT_EQ(model.apply(parse(kBaseRo)).error().code, ErrorCode::WrongState);
    // A non-construction message is rejected by name.
    EXPECT_EQ(model
                  .apply(parse("<mos><mosID>a</mosID><ncsID>n</ncsID><messageID>9</messageID>"
                               "<heartbeat/></mos>"))
                  .error()
                  .code,
              ErrorCode::InvalidArgument);

    // Query misses answer empty/nullopt, never crash.
    EXPECT_EQ(model.runningOrder("ghost"), nullptr);
    EXPECT_TRUE(model.storyIDs("ghost").empty());
    EXPECT_TRUE(model.itemIDs("ghost", "2").empty());
    EXPECT_TRUE(model.itemIDs("5PM", "ghost").empty());
    EXPECT_EQ(model.story("5PM", "ghost"), nullptr);
    EXPECT_EQ(model.item("5PM", "2", "ghost"), nullptr);
    EXPECT_EQ(model.item("5PM", "ghost", "12"), nullptr);
    EXPECT_FALSE(model.readyToAir("ghost").has_value());
    EXPECT_FALSE(model.status(RoElement::Story, "ghost", "2", "").has_value());
}

TEST(RunningOrderModel, MosItemReplaceRejectVocabulary) {
    RunningOrderModel model = modelWithBase();
    const auto replaceMsg = [](const std::string& inner) {
        return parse(
            "<mos><mosID>a</mosID><ncsID>n</ncsID><messageID>9</messageID>"
            "<mosItemReplace>" +
            inner + "</mosItemReplace></mos>");
    };

    // Unknown roID / storyID / itemID are the NotFound (NACK) family.
    EXPECT_EQ(model
                  .apply(replaceMsg("<roID>ghost</roID><storyID>2</storyID>"
                                    "<item><itemID>12</itemID></item>"))
                  .error()
                  .code,
              ErrorCode::NotFound);
    EXPECT_EQ(model
                  .apply(replaceMsg("<roID>5PM</roID><storyID>ghost</storyID>"
                                    "<item><itemID>12</itemID></item>"))
                  .error()
                  .code,
              ErrorCode::NotFound);
    EXPECT_EQ(model
                  .apply(replaceMsg("<roID>5PM</roID><storyID>2</storyID>"
                                    "<item><itemID>ghost</itemID></item>"))
                  .error()
                  .code,
              ErrorCode::NotFound);
    // Shape problems are structural, not resync triggers.
    EXPECT_EQ(model.apply(replaceMsg("<roID>5PM</roID><storyID>2</storyID>")).error().code,
              ErrorCode::ProtocolViolation);
    EXPECT_EQ(model
                  .apply(replaceMsg("<roID>5PM</roID><storyID>2</storyID>"
                                    "<item><objID>M99</objID></item>"))
                  .error()
                  .code,
              ErrorCode::ProtocolViolation);
    // Direct helper misuse: not a mosItemReplace message.
    XmlNode ro = *model.runningOrder("5PM");
    EXPECT_EQ(imos::profiles::applyMosItemReplace(ro, parse(kBaseRo)).error().code,
              ErrorCode::InvalidArgument);
    // Null-value removal of a tag the target never had is a quiet no-op.
    EXPECT_TRUE(model
                    .apply(replaceMsg("<roID>5PM</roID><storyID>2</storyID>"
                                      "<item><itemID>12</itemID><itemChannel/></item>"))
                    .ok());
    EXPECT_EQ(model.item("5PM", "2", "12")->firstChildElement("itemChannel"), nullptr);
}
