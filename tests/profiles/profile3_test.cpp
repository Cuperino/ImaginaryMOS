// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 3 — Advanced Object Based Workflow conformance (plan §3.2 row 3,
// M5 finale). Pure evaluator units first (the subset-XPath grammar, the
// generalSearch truth table, group logic, paging, deterministic sort), then
// a real imos::Device against a real incs::Ncs: the create flow's
// ack-with-objID-then-push contract, mosReqObjAction against the gated spec
// fixture, the searchable-schema and paged-search exchanges on the aux
// channel, mosItemReplace's position-preserving merge with resync
// escalation on NACK, the resync-preference upgrade, three concurrently
// active channels, and the illustrative-fixture leniency probes.

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/gen/views.h>
#include <imos/codec/message.h>
#include <imos/codec/xml.h>
#include <imos/encoding/encoding.h>
#include <imos/profiles/object_query.h>
#include <imos/profiles/profile1.h>
#include <imos/profiles/profile3.h>

#include "two_role_fixture.h"

using imos::Channel;
using imos::SessionEvent;
using imos::SessionEventType;
using imos::codec::Message;
using imos::codec::MessageType;
using imos::codec::parseMessage;
using imos::codec::serializeXmlNode;
using imos::codec::XmlNode;
using imos::profiles::DirectoryEvent;
using imos::profiles::DirectoryEventType;
using imos::profiles::DirectoryQueryPage;
using imos::profiles::evaluateField;
using imos::profiles::evaluateGeneralSearch;
using imos::profiles::evaluateQuery;
using imos::profiles::FieldExpression;
using imos::profiles::makeMosObjCreate;
using imos::profiles::ObjAction;
using imos::profiles::ObjectQuery;
using imos::profiles::ObjectQueryField;
using imos::profiles::ObjectQueryGroup;
using imos::profiles::ObjectQueryPage;
using imos::profiles::parseFieldXPath;
using imos::profiles::parseObjectQuery;
using imos::test::LoopbackEndpoint;
using imos::test::TwoRoleFixture;
using incs::RunningOrderAuthor;

namespace {

// ---- shared helpers ----------------------------------------------------

std::vector<std::uint8_t> encode(const std::string& xml) {
    auto encoded = imos::encoding::encodeFromUtf8(xml, {});
    EXPECT_TRUE(encoded.ok());
    return std::move(encoded).value();
}

Message parseFrame(const std::vector<std::uint8_t>& frame) {
    auto decoded = imos::encoding::decodeToUtf8(frame, {});
    EXPECT_TRUE(decoded.ok());
    auto parsed = parseMessage(decoded.value());
    EXPECT_TRUE(parsed.ok());
    return std::move(parsed).value();
}

std::string readFixtureText(const std::string& name) {
    const std::string path = std::string{IMOS_FIXTURES_DIR} + "/spec-examples/" + name;
    std::ifstream file(path);
    EXPECT_TRUE(file.is_open()) << path;
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

Message loadFixture(const std::string& name) {
    auto parsed = parseMessage(readFixtureText(name));
    EXPECT_TRUE(parsed.ok()) << (parsed.ok() ? "" : parsed.error().message);
    return std::move(parsed).value();
}

/// Structural canonical form: formatting whitespace dropped and the
/// empty-element spelling normalized, so a programmatically built message
/// compares against the spec's hand-indented fixture.
XmlNode canonNode(XmlNode node) {
    std::vector<XmlNode> kept;
    for (XmlNode& child : node.children) {
        if (child.kind == imos::codec::XmlNodeKind::Text &&
            child.value.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }
        kept.push_back(canonNode(std::move(child)));
    }
    node.children = std::move(kept);
    node.emptyTag = node.kind == imos::codec::XmlNodeKind::Element && node.children.empty();
    return node;
}

std::string structuralForm(const XmlNode& node) {
    return serializeXmlNode(canonNode(node));
}

/// Parse a standalone element (e.g. an <item> subtree) from raw XML.
XmlNode parseSubtree(const std::string& xml) {
    auto parsed = imos::codec::parseXmlDocument(xml);
    EXPECT_TRUE(parsed.ok()) << (parsed.ok() ? "" : parsed.error().message);
    return parsed.ok() ? parsed.value().root : XmlNode{};
}

/// A <mosObj> wrapper message with one mosExternalMetadata payload, for
/// evaluator corpora and device catalogs.
Message makeObjectWithPayload(const std::string& objID, const std::string& slug,
                              const std::string& schema, const std::string& payloadInner) {
    const std::string xml =
        "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>1</messageID>"
        "<mosObj><objID>" +
        objID + "</objID><objSlug>" + slug +
        "</objSlug><objType>VIDEO</objType><status>NEW</status>"
        "<mosExternalMetadata><mosScope>OBJECT</mosScope><mosSchema>" +
        schema + "</mosSchema><mosPayload>" + payloadInner +
        "</mosPayload></mosExternalMetadata></mosObj></mos>";
    auto parsed = parseMessage(xml);
    EXPECT_TRUE(parsed.ok());
    // Re-wrap the body in a fresh skeleton: a stored object must not carry a
    // fixed messageID, or repeated pushes would dedupe-suppress each other.
    Message fresh = imos::codec::makeMessage(MessageType::MosObj);
    *fresh.body() = *parsed.value().body();
    return fresh;
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

const DirectoryEvent* firstOfType(const std::vector<DirectoryEvent>& events,
                                  DirectoryEventType type) {
    for (const DirectoryEvent& event : events) {
        if (event.type == type) {
            return &event;
        }
    }
    return nullptr;
}

/// Prime the NCS with the device's advertised machine info: inject a
/// reqMachInfo at the device; its listMachInfo answer carries the real
/// profile flags, which the NCS's Profile 0 responder captures passively.
void exchangeMachineInfo(TwoRoleFixture& fx) {
    const std::string reqXml = "<mos><mosID>" + std::string{TwoRoleFixture::kMosID} +
                               "</mosID><ncsID>" + std::string{TwoRoleFixture::kNcsID} +
                               "</ncsID><messageID>MI-1</messageID><reqMachInfo/></mos>";
    fx.device->onFrameReceived(Channel::Mom, encode(reqXml));
    fx.settle();
}

/// Object provider implementing the full Profile 3 workflow. Objects are
/// keyed in a std::map for deterministic enumeration; createObject refuses
/// slugs named "refuse" (the NACK branch); queryObjects stays Unsupported so
/// the responder's library-evaluator fallback is what answers searches.
class AdvancedObjectSource : public imos::profiles::IObjectSource {
public:
    std::optional<Message> object(std::string_view objID) override {
        const auto it = objects_.find(std::string{objID});
        if (it == objects_.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    std::vector<std::string> objectIDs() override {
        std::vector<std::string> ids;
        for (const auto& [id, message] : objects_) {
            ids.push_back(id);
        }
        return ids;
    }

    [[nodiscard]] bool supportsProfile3() const noexcept override { return true; }

    imos::Result<std::string> createObject(
        const imos::profiles::ObjectCreateRequest& request) override {
        if (request.objSlug == "refuse") {
            return imos::makeError(imos::ErrorCode::WrongState, "device refuses this slug");
        }
        const std::string objID = "OBJ-" + std::to_string(nextID_++);
        Message stored = imos::codec::makeMessage(MessageType::MosObj);
        imos::codec::view::MosObj view{*stored.body()};
        view.setObjID(objID);
        view.setObjSlug(request.objSlug);
        view.setObjType(request.objType);
        view.setStatus("NEW");
        objects_.insert_or_assign(objID, std::move(stored));
        return objID;
    }

    imos::Result<void> actOnObject(ObjAction op, std::string_view objID,
                                   const Message& body) override {
        const auto it = objects_.find(std::string{objID});
        if (it == objects_.end()) {
            return imos::makeError(imos::ErrorCode::NotFound,
                                   "objID '" + std::string{objID} + "' not found");
        }
        if (op == ObjAction::Delete) {
            objects_.erase(it);
            return {};
        }
        // UPDATE: merge the new slug when one is supplied.
        if (const XmlNode* incoming = body.body()) {
            const std::string slug = incoming->childText("objSlug");
            if (!slug.empty()) {
                imos::codec::view::MosObj{*it->second.body()}.setObjSlug(slug);
            }
        }
        return {};
    }

    imos::Result<imos::profiles::SchemaList> searchableSchemas() override {
        return imos::profiles::SchemaList{"http://device.example/schemas/meta1"};
    }

    void put(const std::string& id, Message message) {
        objects_.insert_or_assign(id, std::move(message));
    }
    void remove(const std::string& id) { objects_.erase(id); }

private:
    std::map<std::string, Message> objects_;
    int nextID_ = 1;
};

/// A P1-only source (no Profile 3 overrides) for the fallback branches.
class PlainObjectSource : public imos::profiles::IObjectSource {
public:
    std::optional<Message> object(std::string_view objID) override {
        const auto it = objects_.find(std::string{objID});
        if (it == objects_.end()) {
            return std::nullopt;
        }
        return it->second;
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
    void remove(const std::string& id) { objects_.erase(id); }

private:
    std::map<std::string, Message> objects_;
};

FieldExpression parseXPathOrDie(std::string_view xpath) {
    auto parsed = parseFieldXPath(xpath);
    EXPECT_TRUE(parsed.ok()) << xpath << ": " << (parsed.ok() ? "" : parsed.error().message);
    return parsed.ok() ? std::move(parsed).value() : FieldExpression{};
}

/// A <mosPayload> context for field evaluation.
XmlNode payload(const std::string& inner) {
    return parseSubtree("<mosPayload>" + inner + "</mosPayload>");
}

bool fieldMatchesPayload(std::string_view xpath, const std::string& payloadInner) {
    return evaluateField(parseXPathOrDie(xpath), payload(payloadInner));
}

}  // namespace

// =========================================================================
// Part 1 — the subset-XPath grammar and evaluator (pure units)
// =========================================================================

// 1. Every searchField XPath in the spec's own example parses, and the
//    location-path/predicate split lands where the grammar says.
TEST(Profile3XPath, ParsesSpecExampleFields) {
    const FieldExpression presenter = parseXPathOrDie("/Presenter [. ='Bob']");
    EXPECT_EQ(presenter.steps, (std::vector<std::string>{"Presenter"}));
    EXPECT_TRUE(presenter.predicate.has_value());

    const FieldExpression slug = parseXPathOrDie("/Slug [.='Dog Abuse']");
    EXPECT_EQ(slug.steps, (std::vector<std::string>{"Slug"}));

    // The spec's own quirk: nested path, uppercase AND, implicit '.' left
    // operand on the second comparison.
    const FieldExpression length = parseXPathOrDie("/Video/Length [.>60 AND <120]");
    EXPECT_EQ(length.steps, (std::vector<std::string>{"Video", "Length"}));

    // Relative form (no leading slash) anchors identically.
    const FieldExpression producer = parseXPathOrDie("Producer [.!='Susan']");
    EXPECT_EQ(producer.steps, (std::vector<std::string>{"Producer"}));

    // No predicate at all = existence.
    const FieldExpression bare = parseXPathOrDie("/Owner");
    EXPECT_FALSE(bare.predicate.has_value());
}

// 2. Out-of-subset and malformed expressions are rejected, never guessed at.
TEST(Profile3XPath, RejectsOutOfSubsetExpressions) {
    EXPECT_FALSE(parseFieldXPath("").ok());
    EXPECT_FALSE(parseFieldXPath("/").ok());
    EXPECT_FALSE(parseFieldXPath("/A [. ='x'").ok());  // missing ]
    EXPECT_FALSE(parseFieldXPath("/A [. ='x] trailing").ok());
    EXPECT_FALSE(parseFieldXPath("/A [. = 'unterminated]").ok());
    EXPECT_FALSE(parseFieldXPath("/A [Producer = 'x']").ok());  // path operand: not in subset
    EXPECT_FALSE(parseFieldXPath("//A [. ='x']").ok());         // descendant axis: not in subset
    EXPECT_FALSE(parseFieldXPath("/A [. ! 'x']").ok());         // '!' without '='
}

// 3. Predicate semantics: case-sensitive string comparison, XPath-style
//    numeric comparison (either side numeric), the boolean functions, and
//    the implicit-context comparison from the spec's example.
TEST(Profile3XPath, PredicateSemantics) {
    const std::string bob = "<Presenter>Bob</Presenter>";
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [. ='Bob']", bob));
    EXPECT_FALSE(fieldMatchesPayload("/Presenter [. ='bob']", bob));  // case-sensitive
    EXPECT_TRUE(fieldMatchesPayload("Presenter [.!='Susan']", bob));

    const std::string length90 = "<Video><Length>90</Length></Video>";
    const std::string length150 = "<Video><Length>150</Length></Video>";
    EXPECT_TRUE(fieldMatchesPayload("/Video/Length [.>60 AND <120]", length90));
    EXPECT_FALSE(fieldMatchesPayload("/Video/Length [.>60 AND <120]", length150));
    EXPECT_TRUE(fieldMatchesPayload("/Video/Length [.>=90 and <=90]", length90));

    // or / not / true() / false() / conversions.
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [. ='Ann' or . ='Bob']", bob));
    EXPECT_FALSE(fieldMatchesPayload("/Presenter [not(. ='Bob')]", bob));
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [true()]", bob));
    EXPECT_FALSE(fieldMatchesPayload("/Presenter [false()]", bob));
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [string(.) ='Bob']", bob));
    EXPECT_TRUE(fieldMatchesPayload("/Video/Length [number(.) = 90]", length90));

    // A non-numeric value never satisfies a numeric relation (NaN).
    EXPECT_FALSE(fieldMatchesPayload("/Presenter [. > 5]", bob));

    // Existence (no predicate) and multi-node any-match.
    EXPECT_TRUE(fieldMatchesPayload("/Presenter", bob));
    EXPECT_FALSE(fieldMatchesPayload("/Producer", bob));
    EXPECT_TRUE(fieldMatchesPayload("/Tag [. ='b']", "<Tag>a</Tag><Tag>b</Tag>"));
}

// 4. The generalSearch grammar: the spec's six examples, operator case
//    insensitivity, and the null expression matching everything.
TEST(Profile3GeneralSearch, SpecTruthTable) {
    const std::string corpus = "A man and his dog went to town";
    EXPECT_TRUE(evaluateGeneralSearch("man", corpus));
    EXPECT_TRUE(evaluateGeneralSearch("man dog", corpus));
    EXPECT_TRUE(evaluateGeneralSearch("man and dog", corpus));
    EXPECT_FALSE(evaluateGeneralSearch("man not dog", corpus));
    EXPECT_TRUE(evaluateGeneralSearch("man or cat", corpus));
    EXPECT_TRUE(evaluateGeneralSearch("man and dog not poodle", corpus));
    EXPECT_FALSE(evaluateGeneralSearch("man and dog not town", corpus));
    EXPECT_FALSE(evaluateGeneralSearch("cat", corpus));
    EXPECT_TRUE(evaluateGeneralSearch("MAN OR CAT", corpus));  // terms case-insensitive
    EXPECT_TRUE(evaluateGeneralSearch("", corpus));            // mandatory-but-null tag
    EXPECT_TRUE(evaluateGeneralSearch("   ", corpus));
}

// 5. evaluateQuery: fields AND within a group, groups OR with each other,
//    generalSearch ANDs with the group result, and the mosSchema scope
//    bounds which metadata blocks fields see.
TEST(Profile3Query, GroupLogicOverCorpus) {
    const std::string schema = "http://s/1";
    const Message a = makeObjectWithPayload("A", "Hotel Fire", schema,
                                            "<Presenter>Bob</Presenter><Length>90</Length>");
    const Message b = makeObjectWithPayload("B", "Big Mice", schema,
                                            "<Presenter>Jennifer</Presenter><Length>90</Length>");
    const Message c = makeObjectWithPayload("C", "Dog Abuse", schema,
                                            "<Presenter>Bob</Presenter><Length>150</Length>");
    const std::vector<const Message*> corpus{&a, &b, &c};

    ObjectQuery query;
    query.queryID = "q1";
    query.mosSchema = schema;
    ObjectQueryGroup bobGroup;
    bobGroup.fields.push_back(
        ObjectQueryField{parseXPathOrDie("/Presenter [. ='Bob']"), -1, false});
    bobGroup.fields.push_back(ObjectQueryField{parseXPathOrDie("/Length [.<120]"), -1, false});
    ObjectQueryGroup jenniferGroup;
    jenniferGroup.fields.push_back(
        ObjectQueryField{parseXPathOrDie("/Presenter [. ='Jennifer']"), -1, false});
    query.groups = {bobGroup, jenniferGroup};

    auto page = evaluateQuery(query, corpus);
    ASSERT_TRUE(page.ok());
    // Bob AND short → A; OR Jennifer → B; C fails the Length AND.
    ASSERT_EQ(page.value().objects.size(), 2U);
    EXPECT_EQ(page.value().listReturnTotal, 2U);

    // generalSearch ANDs with the groups.
    query.generalSearch = "mice";
    page = evaluateQuery(query, corpus);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().objects.size(), 1U);
    EXPECT_EQ(page.value().objects.front().body()->childText("objID"), "B");

    // A schema the objects do not carry blinds the fields: no group matches.
    query.generalSearch.clear();
    query.mosSchema = "http://other/schema";
    page = evaluateQuery(query, corpus);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(page.value().listReturnTotal, 0U);
}

// 6. Paging boundaries: the 1-based inclusive window, an absent end meaning
//    all, the actual-indices reporting, the empty past-the-end page, and the
//    zero-total status message.
TEST(Profile3Query, PagingBoundaries) {
    const std::string schema = "http://s/1";
    std::vector<Message> owned;
    std::vector<const Message*> corpus;
    for (int i = 1; i <= 5; ++i) {
        owned.push_back(makeObjectWithPayload("O" + std::to_string(i), "clip", schema,
                                              "<N>" + std::to_string(i) + "</N>"));
    }
    for (const Message& message : owned) {
        corpus.push_back(&message);
    }

    ObjectQuery query;
    query.queryID = "q";
    query.listReturnStart = 2;
    query.listReturnEnd = 4;
    auto page = evaluateQuery(query, corpus);
    ASSERT_TRUE(page.ok());
    EXPECT_EQ(page.value().listReturnStart, 2U);
    EXPECT_EQ(page.value().listReturnEnd, 4U);
    EXPECT_EQ(page.value().listReturnTotal, 5U);
    ASSERT_EQ(page.value().objects.size(), 3U);
    EXPECT_EQ(page.value().objects.front().body()->childText("objID"), "O2");

    query.listReturnEnd.reset();  // absent end → all results
    auto all = evaluateQuery(query, corpus);
    ASSERT_TRUE(all.ok());
    EXPECT_EQ(all.value().objects.size(), 4U);  // 2..5
    EXPECT_EQ(all.value().listReturnEnd, 5U);

    query.listReturnStart = 9;  // past the end: empty page, zero indices
    query.listReturnEnd = 12;
    auto past = evaluateQuery(query, corpus);
    ASSERT_TRUE(past.ok());
    EXPECT_TRUE(past.value().objects.empty());
    EXPECT_EQ(past.value().listReturnStart, 0U);
    EXPECT_EQ(past.value().listReturnEnd, 0U);
    EXPECT_EQ(past.value().listReturnTotal, 5U);

    query.listReturnStart = 1;  // zero total: status says why
    query.listReturnEnd.reset();
    query.generalSearch = "no-such-term";
    auto none = evaluateQuery(query, corpus);
    ASSERT_TRUE(none.ok());
    EXPECT_EQ(none.value().listReturnTotal, 0U);
    EXPECT_FALSE(none.value().listReturnStatus.empty());

    // Shape validation mirrors the parse-side rules.
    query.generalSearch.clear();
    query.listReturnStart = 0;
    EXPECT_FALSE(evaluateQuery(query, corpus).ok());
}

// 7. Deterministic sort: sortByOrder ranks fields, sortType DESCENDING
//    reverses, values compare numerically when both are numbers, and full
//    ties keep corpus order (stable).
TEST(Profile3Query, DeterministicSort) {
    const std::string schema = "http://s/1";
    const Message a =
        makeObjectWithPayload("A", "clip", schema, "<Show>2</Show><Length>90</Length>");
    const Message b =
        makeObjectWithPayload("B", "clip", schema, "<Show>1</Show><Length>200</Length>");
    const Message c =
        makeObjectWithPayload("C", "clip", schema, "<Show>2</Show><Length>110</Length>");
    const std::vector<const Message*> corpus{&a, &b, &c};

    ObjectQuery query;
    query.queryID = "q";
    query.mosSchema = schema;
    ObjectQueryGroup group;
    group.fields.push_back(ObjectQueryField{parseXPathOrDie("/Show"), 1, false});
    group.fields.push_back(ObjectQueryField{parseXPathOrDie("/Length"), 2, true});  // DESCENDING
    query.groups = {group};

    auto page = evaluateQuery(query, corpus);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().objects.size(), 3U);
    // Show ascending: B(1) first; then Show=2 pair by Length descending:
    // C(110) before A(90). Numeric, not lexicographic (110 < 90 as strings).
    EXPECT_EQ(page.value().objects[0].body()->childText("objID"), "B");
    EXPECT_EQ(page.value().objects[1].body()->childText("objID"), "C");
    EXPECT_EQ(page.value().objects[2].body()->childText("objID"), "A");
}

// 8. parseObjectQuery round-trips a built message and enforces the spec's
//    shape rules (start >= 1, end >= start, sort-attribute consistency, the
//    XPath attribute being present and parseable).
TEST(Profile3Query, ParseValidatesShape) {
    ObjectQuery query;
    query.queryID = "cache-key-1";
    query.username = "jbob";
    query.listReturnStart = 1;
    query.listReturnEnd = 20;
    query.generalSearch = "man bites dog";
    query.mosSchema = "http://s/1";
    ObjectQueryGroup group;
    group.fields.push_back(ObjectQueryField{parseXPathOrDie("/Presenter [. ='Bob']"), 1, false});
    group.fields.push_back(ObjectQueryField{parseXPathOrDie("/Length [.>60 AND <120]"), 2, true});
    query.groups = {group};

    const Message wire = imos::profiles::buildMosReqObjList(query);
    auto reparsed = parseObjectQuery(wire);
    ASSERT_TRUE(reparsed.ok()) << reparsed.error().message;
    EXPECT_EQ(reparsed.value().queryID, "cache-key-1");
    EXPECT_EQ(reparsed.value().username, "jbob");
    EXPECT_EQ(reparsed.value().listReturnStart, 1U);
    EXPECT_EQ(reparsed.value().listReturnEnd, std::optional<std::size_t>{20});
    EXPECT_EQ(reparsed.value().generalSearch, "man bites dog");
    ASSERT_EQ(reparsed.value().groups.size(), 1U);
    ASSERT_EQ(reparsed.value().groups.front().fields.size(), 2U);
    EXPECT_EQ(reparsed.value().groups.front().fields[1].sortByOrder, 2);
    EXPECT_TRUE(reparsed.value().groups.front().fields[1].sortDescending);

    const auto reject = [](const std::string& body) {
        const std::string xml =
            "<mos><mosID>m</mosID><ncsID>n</ncsID><messageID>1</messageID>"
            "<mosReqObjList>" +
            body + "</mosReqObjList></mos>";
        auto parsed = parseMessage(xml);
        ASSERT_TRUE(parsed.ok());
        EXPECT_FALSE(parseObjectQuery(parsed.value()).ok()) << body;
    };
    reject(
        "<queryID>q</queryID><listReturnStart>0</listReturnStart>"
        "<listReturnEnd/><generalSearch/>");
    reject(
        "<queryID>q</queryID><listReturnStart>10</listReturnStart>"
        "<listReturnEnd>5</listReturnEnd><generalSearch/>");
    reject(
        "<queryID>q</queryID><listReturnStart>1</listReturnStart><listReturnEnd/>"
        "<generalSearch/><searchGroup><searchField/></searchGroup>");  // no XPath attr
    // Same field, conflicting sort attributes across groups.
    reject(
        "<queryID>q</queryID><listReturnStart>1</listReturnStart><listReturnEnd/>"
        "<generalSearch/>"
        "<searchGroup><searchField XPath=\"/A\" sortByOrder=\"1\"/></searchGroup>"
        "<searchGroup><searchField XPath=\"/A\" sortByOrder=\"2\"/></searchGroup>");
    // Two unlike fields sharing one sortByOrder.
    reject(
        "<queryID>q</queryID><listReturnStart>1</listReturnStart><listReturnEnd/>"
        "<generalSearch/>"
        "<searchGroup><searchField XPath=\"/A\" sortByOrder=\"1\"/>"
        "<searchField XPath=\"/B\" sortByOrder=\"1\"/></searchGroup>");
}

// =========================================================================
// Part 2 — conformance: real Device vs real Ncs
// =========================================================================

// 9. The placeholder-create flow (spec §3.3.1): the ACK returns the new
//    objID in statusDescription, and the full mosObj push follows on mom —
//    ack first, push after — landing in the directory under the same objID.
TEST(Profile3, CreateFlowAcksObjIdThenPushes) {
    TwoRoleFixture fx;
    AdvancedObjectSource source;
    fx.device->attach(source);
    fx.connectAll();

    ASSERT_TRUE(fx.ncs
                    ->createObject(
                        makeMosObjCreate("Hotel Fire", "VIDEO", "59.94", "1800", "Show 7", "Chris"))
                    .ok());
    fx.settle();

    const std::vector<DirectoryEvent> events = drainDirectory(*fx.ncs);
    const DirectoryEvent* created = firstOfType(events, DirectoryEventType::ObjectCreated);
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(created->objID, "OBJ-1");
    EXPECT_EQ(created->statusDescription, "OBJ-1");  // the spec's field overload

    // The push correlates by objID and arrives after the ack resolution.
    const DirectoryEvent* updated = firstOfType(events, DirectoryEventType::ObjectUpdated);
    ASSERT_NE(updated, nullptr);
    EXPECT_EQ(updated->objID, "OBJ-1");
    EXPECT_LT(created - events.data(), updated - events.data());

    const Message* mirrored = fx.ncs->object("OBJ-1");
    ASSERT_NE(mirrored, nullptr);
    EXPECT_EQ(mirrored->body()->childText("objSlug"), "Hotel Fire");
    EXPECT_EQ(mirrored->body()->childText("status"), "NEW");
}

// 10. The NACK branch: the device refuses, the ack carries the reason, and
//     no phantom entry appears in the directory.
TEST(Profile3, CreateNackLeavesNoPhantomEntry) {
    TwoRoleFixture fx;
    AdvancedObjectSource source;
    fx.device->attach(source);
    fx.connectAll();

    ASSERT_TRUE(fx.ncs->createObject(makeMosObjCreate("refuse", "VIDEO", "59.94")).ok());
    fx.settle();

    const std::vector<DirectoryEvent> events = drainDirectory(*fx.ncs);
    EXPECT_EQ(countType(events, DirectoryEventType::ObjectCreated), 0);
    const DirectoryEvent* failed = firstOfType(events, DirectoryEventType::RequestFailed);
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->statusDescription, "device refuses this slug");
    EXPECT_EQ(fx.ncs->objectIDs().size(), 0U);
}

// 10b. Several creates in flight: an ack names the new objID, not the
//      request, so each resolution carries the channel and Seq its call
//      returned and a host can match placeholders to answers.
TEST(Profile3, CreateResolutionsCarryTheirRequestSeq) {
    TwoRoleFixture fx;
    AdvancedObjectSource source;
    fx.device->attach(source);
    fx.connectAll();

    const auto first = fx.ncs->createObject(makeMosObjCreate("Hotel Fire", "VIDEO", "59.94"));
    const auto refused = fx.ncs->createObject(makeMosObjCreate("refuse", "VIDEO", "59.94"));
    const auto second = fx.ncs->createObject(makeMosObjCreate("Mice in City", "VIDEO", "59.94"));
    ASSERT_TRUE(first.ok() && refused.ok() && second.ok());
    fx.settle();

    const std::vector<DirectoryEvent> events = drainDirectory(*fx.ncs);
    std::map<imos::Seq, const DirectoryEvent*> answers;
    for (const DirectoryEvent& event : events) {
        if (event.type == DirectoryEventType::ObjectCreated ||
            event.type == DirectoryEventType::RequestFailed) {
            EXPECT_EQ(event.channel, Channel::Mom);
            answers[event.seq] = &event;
        } else if (event.type == DirectoryEventType::ObjectUpdated) {
            EXPECT_EQ(event.seq, 0U);  // the device's own push answers nothing
        }
    }
    ASSERT_EQ(answers.size(), 3U);
    ASSERT_TRUE(answers.contains(first.value()) && answers.contains(refused.value()) &&
                answers.contains(second.value()));
    EXPECT_EQ(answers[first.value()]->type, DirectoryEventType::ObjectCreated);
    EXPECT_EQ(answers[refused.value()]->type, DirectoryEventType::RequestFailed);
    EXPECT_EQ(answers[second.value()]->type, DirectoryEventType::ObjectCreated);
    EXPECT_EQ(fx.ncs->object(answers[first.value()]->objID)->body()->childText("objSlug"),
              "Hotel Fire");
    EXPECT_EQ(fx.ncs->object(answers[second.value()]->objID)->body()->childText("objSlug"),
              "Mice in City");
}

// 11. A source without the Profile 3 overrides NACKs creates outright.
TEST(Profile3, PlainSourceNacksCreate) {
    TwoRoleFixture fx;
    PlainObjectSource source;
    fx.device->attach(source);
    fx.connectAll();

    ASSERT_TRUE(fx.ncs->createObject(makeMosObjCreate("Hotel Fire", "VIDEO", "59.94")).ok());
    fx.settle();

    const std::vector<DirectoryEvent> events = drainDirectory(*fx.ncs);
    const DirectoryEvent* failed = firstOfType(events, DirectoryEventType::RequestFailed);
    ASSERT_NE(failed, nullptr);
    EXPECT_EQ(failed->statusDescription, "Profile 3 not supported");
}

// 12. mosReqObjAction UPDATE and DELETE: acked, then the corresponding
//     mosObj push (status UPDATED / DELETED) reaches the directory; DELETE's
//     push is the Profile 1 delete other observers key on.
TEST(Profile3, ActionUpdateAndDeleteRoundTrip) {
    TwoRoleFixture fx;
    AdvancedObjectSource source;
    fx.device->attach(source);
    fx.connectAll();

    ASSERT_TRUE(fx.ncs->createObject(makeMosObjCreate("Hotel Fire", "VIDEO", "59.94")).ok());
    fx.settle();
    (void)drainDirectory(*fx.ncs);

    // UPDATE with a new slug in the action body.
    Message update = imos::codec::makeMessage(MessageType::MosReqObjAction);
    imos::codec::view::MosReqObjAction{*update.body()}.setObjSlug("Bigger Fire");
    ASSERT_TRUE(fx.ncs->requestObjAction(ObjAction::Update, "OBJ-1", std::move(update)).ok());
    fx.settle();

    std::vector<DirectoryEvent> events = drainDirectory(*fx.ncs);
    EXPECT_EQ(countType(events, DirectoryEventType::ActionCompleted), 1);
    const Message* mirrored = fx.ncs->object("OBJ-1");
    ASSERT_NE(mirrored, nullptr);
    EXPECT_EQ(mirrored->body()->childText("objSlug"), "Bigger Fire");
    EXPECT_EQ(mirrored->body()->childText("status"), "UPDATED");

    // DELETE: ActionCompleted plus the status-DELETED push removing the mirror.
    ASSERT_TRUE(fx.ncs->requestObjAction(ObjAction::Delete, "OBJ-1").ok());
    fx.settle();
    events = drainDirectory(*fx.ncs);
    EXPECT_EQ(countType(events, DirectoryEventType::ActionCompleted), 1);
    EXPECT_EQ(countType(events, DirectoryEventType::ObjectDeleted), 1);
    EXPECT_EQ(fx.ncs->object("OBJ-1"), nullptr);

    // UPDATE against a deleted object NACKs.
    ASSERT_TRUE(fx.ncs->requestObjAction(ObjAction::Update, "OBJ-1").ok());
    fx.settle();
    events = drainDirectory(*fx.ncs);
    EXPECT_EQ(countType(events, DirectoryEventType::RequestFailed), 1);
}

// 13. The NCS's built DELETE matches the gated spec fixture's canonical
//     form (mosReqObjAction-3.xml), attributes and all.
TEST(Profile3, DeleteMatchesSpecFixture) {
    TwoRoleFixture fx;
    fx.connectAll();
    auto& momQueue = fx.ncsTransport.outbound[static_cast<std::size_t>(Channel::Mom)];
    momQueue.clear();
    ASSERT_TRUE(fx.ncs->requestObjAction(ObjAction::Delete, "1EFA3009233F8329C1").ok());
    ASSERT_FALSE(momQueue.empty());
    const Message built = parseFrame(momQueue.front());
    ASSERT_EQ(built.type(), MessageType::MosReqObjAction);

    const Message fixture = loadFixture("mosReqObjAction-3.xml");
    EXPECT_EQ(structuralForm(*built.body()), structuralForm(*fixture.body()));
}

// 14. The searchable-schema exchange runs on the aux channel and lands the
//     device's schema pointers in the directory.
TEST(Profile3, SearchableSchemasOnAux) {
    TwoRoleFixture fx;
    AdvancedObjectSource source;
    fx.device->attach(source);
    fx.connectAll();

    auto& auxQueue = fx.ncsTransport.outbound[static_cast<std::size_t>(Channel::Aux)];
    ASSERT_TRUE(fx.ncs->requestSearchableSchemas().ok());
    EXPECT_FALSE(auxQueue.empty()) << "mosReqSearchableSchema must ride the aux channel";
    fx.settle();

    const std::vector<DirectoryEvent> events = drainDirectory(*fx.ncs);
    EXPECT_EQ(countType(events, DirectoryEventType::SchemasListed), 1);
    EXPECT_EQ(fx.ncs->searchableSchemas(),
              (std::vector<std::string>{"http://device.example/schemas/meta1"}));
}

// 15. The search flow: a mosReqObjList with generalSearch, a searchGroup,
//     and paging is answered (via the library evaluator — the test source
//     leaves queryObjects Unsupported) with a page whose objects ingest
//     into the directory mirror.
TEST(Profile3, SearchFlowIngestsPage) {
    TwoRoleFixture fx;
    AdvancedObjectSource source;
    const std::string schema = "http://device.example/schemas/meta1";
    source.put("A", makeObjectWithPayload("A", "Hotel Fire", schema,
                                          "<Presenter>Bob</Presenter><Length>90</Length>"));
    source.put("B", makeObjectWithPayload("B", "Mice in City", schema,
                                          "<Presenter>Jennifer</Presenter><Length>90</Length>"));
    source.put("C", makeObjectWithPayload("C", "Fire Dept Tour", schema,
                                          "<Presenter>Bob</Presenter><Length>150</Length>"));
    fx.device->attach(source);
    fx.connectAll();

    ObjectQuery query;
    query.queryID = "search-1";
    query.mosSchema = schema;
    query.generalSearch = "fire";
    ObjectQueryGroup group;
    group.fields.push_back(ObjectQueryField{parseXPathOrDie("/Presenter [. ='Bob']"), -1, false});
    query.groups = {group};

    const auto sent = fx.ncs->queryObjects(query);
    ASSERT_TRUE(sent.ok());
    fx.settle();

    std::optional<DirectoryQueryPage> page = fx.ncs->nextQueryPage();
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(page->seq, sent.value());
    EXPECT_EQ(page->queryID, "search-1");
    EXPECT_EQ(page->listReturnTotal, 2U);
    EXPECT_EQ(page->objIDs, (std::vector<std::string>{"A", "C"}));
    EXPECT_NE(fx.ncs->object("A"), nullptr);
    EXPECT_NE(fx.ncs->object("C"), nullptr);
    EXPECT_EQ(fx.ncs->object("B"), nullptr);  // never matched, never mirrored
}

// 16. A malformed query (bad subset-XPath) still completes the exchange:
//     the mosObjList answer reports zero results and carries the diagnostic
//     in listReturnStatus.
TEST(Profile3, MalformedQueryAnswersWithStatus) {
    TwoRoleFixture fx;
    AdvancedObjectSource source;
    fx.device->attach(source);
    fx.connectAll();

    const std::string xml =
        "<mos><mosID>" + std::string{TwoRoleFixture::kMosID} + "</mosID><ncsID>" +
        std::string{TwoRoleFixture::kNcsID} +
        "</ncsID><messageID>Q-1</messageID><mosReqObjList>"
        "<queryID>bad-1</queryID><listReturnStart>1</listReturnStart><listReturnEnd/>"
        "<generalSearch/><mosSchema/>"
        "<searchGroup><searchField XPath=\"/Presenter [. ='Bob'\"/></searchGroup>"
        "</mosReqObjList></mos>";
    auto& deviceAux = fx.deviceTransport.outbound[static_cast<std::size_t>(Channel::Aux)];
    deviceAux.clear();
    fx.device->onFrameReceived(Channel::Aux, encode(xml));

    ASSERT_FALSE(deviceAux.empty());
    const Message reply = parseFrame(deviceAux.front());
    ASSERT_EQ(reply.type(), MessageType::MosObjList);
    EXPECT_EQ(reply.body()->childText("queryID"), "bad-1");
    EXPECT_EQ(reply.body()->childText("listReturnTotal"), "0");
    EXPECT_FALSE(reply.body()->childText("listReturnStatus").empty());
    EXPECT_EQ(reply.messageID().value_or(""), "Q-1");  // terminal reply correlation
}

// 17. mosItemReplace: the device-originated replace lands in the NCS
//     authoring store with the spec's merge semantics — position unchanged
//     (middle of three), replaced tags in place, null-value tags removed,
//     new tags added, the MEM block appended, its payload byte-preserved —
//     and the OK ack folds the same merge into the device's shadow model.
TEST(Profile3, ItemReplaceMergesPreservingPosition) {
    TwoRoleFixture fx;
    fx.connectAll();

    ASSERT_TRUE(fx.ncs->author().create("RO1", "Five PM").ok());
    XmlNode story = RunningOrderAuthor::makeStory("s1", "Hotel Fire");
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", story).ok());
    for (const char* id : {"i1", "i2", "i3"}) {
        XmlNode item = RunningOrderAuthor::makeItem(id, std::string{"OBJ-"} + id,
                                                    TwoRoleFixture::kMosID, "slug");
        item.appendElement("itemEdDur").setText("815");
        ASSERT_TRUE(fx.ncs->author().insertItem("RO1", "s1", item).ok());
    }
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();
    ASSERT_EQ(fx.device->runningOrderModel().itemIDs("RO1", "s1"),
              (std::vector<std::string>{"i1", "i2", "i3"}));

    const XmlNode replacement =
        parseSubtree("<item><itemID>i2</itemID><objID>M000627</objID><mosID>" +
                     std::string{TwoRoleFixture::kMosID} +
                     "</mosID><itemEdDur/><itemUserTimingDur>310</itemUserTimingDur>"
                     "<mosExternalMetadata><mosScope>PLAYLIST</mosScope>"
                     "<mosSchema>http://vendor/schema/v1</mosSchema>"
                     "<mosPayload><trigger>837</trigger><key>110</key></mosPayload>"
                     "</mosExternalMetadata></item>");
    ASSERT_TRUE(fx.device->replaceItem("RO1", "s1", replacement).ok());
    fx.settle();

    std::optional<imos::profiles::ItemReplaceResult> result = fx.device->nextItemReplaceResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->ok) << result->roStatus;

    std::optional<incs::ItemReplaceIngest> ingest = fx.ncs->nextItemReplace();
    ASSERT_TRUE(ingest.has_value());
    EXPECT_TRUE(ingest->ok);
    EXPECT_EQ(ingest->roID, "RO1");
    EXPECT_EQ(ingest->storyID, "s1");
    EXPECT_EQ(ingest->itemID, "i2");

    // Position unchanged; merge semantics applied on both stores.
    const std::vector<const imos::profiles::RunningOrderModel*> models{
        &fx.ncs->author().desired(), &fx.device->runningOrderModel()};
    for (const imos::profiles::RunningOrderModel* model : models) {
        EXPECT_EQ(model->itemIDs("RO1", "s1"), (std::vector<std::string>{"i1", "i2", "i3"}));
        const XmlNode* item = model->item("RO1", "s1", "i2");
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(item->childText("objID"), "M000627");            // replaced in place
        EXPECT_EQ(item->firstChildElement("itemEdDur"), nullptr);  // null value removed
        EXPECT_EQ(item->childText("itemUserTimingDur"), "310");    // new tag added
        EXPECT_EQ(item->childText("itemSlug"), "slug");            // untouched tag intact
        const XmlNode* metadata = item->firstChildElement("mosExternalMetadata");
        ASSERT_NE(metadata, nullptr);
        // Byte-preserved payload (the opaque-block guarantee).
        EXPECT_EQ(serializeXmlNode(*metadata->firstChildElement("mosPayload")),
                  "<mosPayload><trigger>837</trigger><key>110</key></mosPayload>");
    }

    // The untouched neighbors kept their content.
    const XmlNode* first = fx.ncs->author().desired().item("RO1", "s1", "i1");
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->childText("itemEdDur"), "815");

    // A committed follow-up does not re-send the merged item (the projection
    // converged): commit reports nothing to do.
    auto seqs = fx.ncs->author().commit("RO1");
    ASSERT_TRUE(seqs.ok());
    EXPECT_TRUE(seqs.value().empty());
}

// 18. A second replace with a matching-schema MEM block replaces that block
//     rather than appending a sibling (first-match-only per the spec).
TEST(Profile3, ItemReplaceMatchingSchemaReplacesBlock) {
    TwoRoleFixture fx;
    fx.connectAll();
    ASSERT_TRUE(fx.ncs->author().create("RO1", "Five PM").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", RunningOrderAuthor::makeStory("s1")).ok());
    ASSERT_TRUE(fx.ncs->author()
                    .insertItem("RO1", "s1",
                                RunningOrderAuthor::makeItem("i1", "O1", TwoRoleFixture::kMosID))
                    .ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();

    const auto replaceWith = [&](const std::string& payload) {
        const XmlNode item = parseSubtree(
            "<item><itemID>i1</itemID><mosExternalMetadata>"
            "<mosSchema>http://vendor/schema/v1</mosSchema><mosPayload>" +
            payload + "</mosPayload></mosExternalMetadata></item>");
        ASSERT_TRUE(fx.device->replaceItem("RO1", "s1", item).ok());
        fx.settle();
    };
    replaceWith("<fade>17</fade>");
    replaceWith("<fade>99</fade>");

    const XmlNode* item = fx.ncs->author().desired().item("RO1", "s1", "i1");
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->countChildElements("mosExternalMetadata"), 1U);
    EXPECT_EQ(item->firstChildElement("mosExternalMetadata")->childText("mosPayload"), "");
    EXPECT_EQ(serializeXmlNode(
                  *item->firstChildElement("mosExternalMetadata")->firstChildElement("mosPayload")),
              "<mosPayload><fade>99</fade></mosPayload>");
}

// 19. The NACK branch (unknown story): the NCS answers the spec's NACK
//     roAck naming the item, and the device escalates the RO to the §3.3
//     resync driver — one ResyncNeeded, then a roReq rebuild converges.
TEST(Profile3, ItemReplaceUnknownStoryNacksIntoResync) {
    TwoRoleFixture fx;
    fx.connectAll();
    ASSERT_TRUE(fx.ncs->author().create("RO1", "Five PM").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", RunningOrderAuthor::makeStory("s1")).ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();

    const XmlNode item = parseSubtree("<item><itemID>iX</itemID><objID>O9</objID></item>");
    ASSERT_TRUE(fx.device->replaceItem("RO1", "ghost-story", item).ok());
    fx.settle();

    std::optional<imos::profiles::ItemReplaceResult> result = fx.device->nextItemReplaceResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->ok);
    EXPECT_NE(result->roStatus.find("NACK"), std::string::npos);

    std::optional<incs::ItemReplaceIngest> ingest = fx.ncs->nextItemReplace();
    ASSERT_TRUE(ingest.has_value());
    EXPECT_FALSE(ingest->ok);

    // The resync driver announced itself and rebuilt the RO.
    bool sawResync = false;
    while (std::optional<SessionEvent> event = fx.device->nextEvent()) {
        sawResync = sawResync || event->type == SessionEventType::ResyncNeeded;
    }
    EXPECT_TRUE(sawResync);
    EXPECT_EQ(fx.device->runningOrderModel().storyIDs("RO1"), (std::vector<std::string>{"s1"}));
}

// 19b. Ownership (spec 3.3.4: a MOS replaces only item references it
//      owns): a replace aimed at another MOS's item, or one handing an item
//      to another MOS, is NACKed and merges nothing.
TEST(Profile3, ItemReplaceOfAnotherMossItemIsRefused) {
    TwoRoleFixture fx;
    fx.connectAll();
    ASSERT_TRUE(fx.ncs->author().create("RO1", "Five PM").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", RunningOrderAuthor::makeStory("s1")).ok());
    ASSERT_TRUE(
        fx.ncs->author()
            .insertItem("RO1", "s1",
                        RunningOrderAuthor::makeItem("mine", "O1", TwoRoleFixture::kMosID, "Mine"))
            .ok());
    ASSERT_TRUE(
        fx.ncs->author()
            .insertItem("RO1", "s1",
                        RunningOrderAuthor::makeItem("theirs", "O2", "cg.example.mos", "Theirs"))
            .ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();

    const auto replace = [&](const std::string& xml) {
        EXPECT_TRUE(fx.device->replaceItem("RO1", "s1", parseSubtree(xml)).ok());
        fx.settle();
        std::optional<incs::ItemReplaceIngest> ingest = fx.ncs->nextItemReplace();
        EXPECT_TRUE(ingest.has_value());
        return ingest.value_or(incs::ItemReplaceIngest{});
    };

    const incs::ItemReplaceIngest foreign =
        replace("<item><itemID>theirs</itemID><objID>HIJACK</objID></item>");
    EXPECT_FALSE(foreign.ok);
    EXPECT_NE(foreign.detail.find("another MOS"), std::string::npos);
    EXPECT_EQ(fx.ncs->author().desired().item("RO1", "s1", "theirs")->childText("objID"), "O2");

    const incs::ItemReplaceIngest handOff =
        replace("<item><itemID>mine</itemID><mosID>cg.example.mos</mosID></item>");
    EXPECT_FALSE(handOff.ok);
    EXPECT_EQ(fx.ncs->author().desired().item("RO1", "s1", "mine")->childText("mosID"),
              TwoRoleFixture::kMosID);

    const incs::ItemReplaceIngest own =
        replace("<item><itemID>mine</itemID><itemSlug>Mine, final</itemSlug></item>");
    EXPECT_TRUE(own.ok) << own.detail;
    EXPECT_EQ(fx.ncs->author().desired().item("RO1", "s1", "mine")->childText("itemSlug"),
              "Mine, final");
}

// 20. Resync preference: with the peer's listMachInfo advertising Profile 3,
//     directory resync issues paged match-all mosReqObjList queries (chained
//     until the total is covered); a P1-only peer falls back to mosReqAll.
TEST(Profile3, ResyncPrefersQueryWhenAdvertised) {
    TwoRoleFixture fx;
    AdvancedObjectSource source;
    const std::string schema = "http://device.example/schemas/meta1";
    for (int i = 1; i <= 5; ++i) {
        const std::string id = "O" + std::to_string(i);
        source.put(id, makeObjectWithPayload(id, "clip", schema, "<N/>"));
    }
    fx.device->attach(source);
    fx.connectAll();
    exchangeMachineInfo(fx);  // the NCS learns the device's Profile 3 flag

    ASSERT_TRUE(fx.ncs->resyncDirectory(2).ok());
    fx.settle();

    int pages = 0;
    std::size_t total = 0;
    while (std::optional<DirectoryQueryPage> page = fx.ncs->nextQueryPage()) {
        ++pages;
        total = page->listReturnTotal;
    }
    EXPECT_EQ(pages, 3);  // 2 + 2 + 1
    EXPECT_EQ(total, 5U);
    EXPECT_EQ(fx.ncs->objectIDs().size(), 5U);
    const std::vector<DirectoryEvent> events = drainDirectory(*fx.ncs);
    EXPECT_EQ(countType(events, DirectoryEventType::SyncAcknowledged), 0);  // no mosReqAll
}

TEST(Profile3, ResyncFallsBackToReqAllWithoutAdvertisement) {
    TwoRoleFixture fx;
    PlainObjectSource source;
    source.put("O1", makeObjectWithPayload("O1", "clip", "http://s/1", "<N/>"));
    source.put("O2", makeObjectWithPayload("O2", "clip", "http://s/1", "<N/>"));
    fx.device->attach(source);
    fx.connectAll();
    exchangeMachineInfo(fx);  // advertises Profile 1 without Profile 3

    ASSERT_TRUE(fx.ncs->resyncDirectory().ok());
    fx.settle();

    const std::vector<DirectoryEvent> events = drainDirectory(*fx.ncs);
    EXPECT_EQ(countType(events, DirectoryEventType::SyncAcknowledged), 1);  // mosReqAll path
    EXPECT_EQ(fx.ncs->objectIDs().size(), 2U);
    EXPECT_FALSE(fx.ncs->nextQueryPage().has_value());
}

// 20b. A resync refills the mirror from scratch as far as the peer is
//      concerned: an object it no longer lists was deleted while the NCS
//      was not listening, and is swept out as ObjectDeleted once the refill
//      is complete - by pages (Profile 3) or one mosListAll (Profile 1).
template <typename Source>
void resyncSweeps(bool profile3, std::size_t pageSize) {
    TwoRoleFixture fx;
    Source source;
    for (int i = 1; i <= 5; ++i) {
        const std::string id = "O" + std::to_string(i);
        source.put(id, makeObjectWithPayload(id, "clip", "http://s/1", "<N/>"));
    }
    fx.device->attach(source);
    fx.connectAll();
    exchangeMachineInfo(fx);
    ASSERT_TRUE(fx.ncs->resyncDirectory(pageSize).ok());
    fx.settle();
    ASSERT_EQ(fx.ncs->objectIDs().size(), 5U);
    (void)drainDirectory(*fx.ncs);
    while (fx.ncs->nextQueryPage()) {
    }

    source.remove("O2");  // gone with no DELETED push heard
    source.remove("O5");
    ASSERT_TRUE(fx.ncs->resyncDirectory(pageSize).ok());
    fx.settle();

    std::vector<std::string> ids = fx.ncs->objectIDs();
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<std::string>{"O1", "O3", "O4"}));
    std::vector<std::string> deleted;
    for (const DirectoryEvent& event : drainDirectory(*fx.ncs)) {
        if (event.type == DirectoryEventType::ObjectDeleted) {
            deleted.push_back(event.objID);
        }
    }
    EXPECT_EQ(deleted, (std::vector<std::string>{"O2", "O5"}));
    (void)profile3;
}

TEST(Profile3, ResyncByPagesSweepsWhatThePeerNoLongerLists) {
    resyncSweeps<AdvancedObjectSource>(true, 2);
}

TEST(Profile3, ResyncByReqAllSweepsWhatThePeerNoLongerLists) {
    resyncSweeps<PlainObjectSource>(false, 0);
}

// 21. Resync-on-miss: a NACKed mosReqObj triggers the preferred mechanism
//     automatically once enabled.
TEST(Profile3, ResyncOnMissRunsPreferredMechanism) {
    TwoRoleFixture fx;
    AdvancedObjectSource source;
    source.put("REAL", makeObjectWithPayload("REAL", "clip", "http://device.example/schemas/meta1",
                                             "<N/>"));
    fx.device->attach(source);
    fx.connectAll();
    exchangeMachineInfo(fx);
    fx.ncs->setDirectoryResyncOnMiss(true);

    ASSERT_TRUE(fx.ncs->requestObject("GONE").ok());
    fx.settle();

    const std::vector<DirectoryEvent> events = drainDirectory(*fx.ncs);
    EXPECT_EQ(countType(events, DirectoryEventType::RequestFailed), 1);
    EXPECT_TRUE(fx.ncs->nextQueryPage().has_value());  // the P3 resync ran
    EXPECT_EQ(fx.ncs->objectIDs(), (std::vector<std::string>{"REAL"}));
}

// 22. Three concurrently active channels (§4.4 finally exercised in full):
//     a paced Profile 1 sync holds the mom channel across fake-clock time
//     while an aux search and an ro construction commit complete
//     immediately — no cross-channel head-of-line blocking. The paced sync
//     then finishes on its own cadence.
TEST(Profile3, ThreeChannelIndependence) {
    TwoRoleFixture fx;
    AdvancedObjectSource source;
    const std::string schema = "http://device.example/schemas/meta1";
    for (int i = 1; i <= 3; ++i) {
        const std::string id = "O" + std::to_string(i);
        source.put(id, makeObjectWithPayload(id, "clip", schema, "<N/>"));
    }
    fx.device->attach(source);
    fx.connectAll();

    // mom: a paced sync (one mosObj per 10 s of fake time; nothing yet).
    ASSERT_TRUE(fx.ncs->requestAll(10).ok());
    fx.settle();
    ASSERT_EQ(fx.ncs->objectIDs().size(), 0U);  // acked, first release pending

    // aux: a query that matches nothing (so the mirror stays untouched)…
    ObjectQuery query;
    query.queryID = "concurrent-1";
    query.generalSearch = "no-such-term";
    ASSERT_TRUE(fx.ncs->queryObjects(query).ok());
    // …and ro: a construction commit, both while mom is mid-sync.
    ASSERT_TRUE(fx.ncs->author().create("RO1", "Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();

    // Both completed although the paced mom sync has released nothing.
    std::optional<DirectoryQueryPage> page = fx.ncs->nextQueryPage();
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(page->listReturnTotal, 0U);
    std::optional<incs::CommitEvent> commit = fx.ncs->author().nextEvent();
    ASSERT_TRUE(commit.has_value());
    EXPECT_TRUE(commit->ok);
    EXPECT_TRUE(fx.device->runningOrderModel().has("RO1"));
    EXPECT_EQ(fx.ncs->objectIDs().size(), 0U);  // mom still pacing

    // The paced sync completes on its own clock.
    for (int i = 0; i < 3; ++i) {
        fx.clock.advanceSeconds(10);
        fx.settle();
    }
    EXPECT_EQ(fx.ncs->objectIDs().size(), 3U);
}

// 23. Leniency probes: the spec's illustrative (deliberately non-well-formed)
//     Profile 3 fragments fail to parse gracefully — an error Result, never
//     a crash — which is exactly why the fixtures gate excludes them.
TEST(Profile3, IllustrativeFixturesAreLeniencyProbes) {
    for (const char* name : {"mosObjCreate.illustrative.xml", "mosReqObjList.illustrative.xml",
                             "mosObjList.illustrative.xml", "mosReqObjAction.illustrative.xml",
                             "mosReqObjAction-2.illustrative.xml"}) {
        SCOPED_TRACE(name);
        const std::string text = readFixtureText(name);
        auto parsed = parseMessage(text);
        if (parsed.ok()) {
            // Tolerable: a fragment that happens to be well-formed must at
            // least round-trip through the retained tree.
            EXPECT_FALSE(imos::codec::serializeMessage(parsed.value()).empty());
        } else {
            EXPECT_FALSE(parsed.error().message.empty());
        }
    }
}

// 24. The gated fixtures for the schema exchange parse into the expected
//     typed views (spec-conformance of the generated registry for the aux
//     family).
TEST(Profile3, GatedFixturesParseTyped) {
    const Message req = loadFixture("mosReqSearchableSchema.xml");
    EXPECT_EQ(req.type(), MessageType::MosReqSearchableSchema);
    EXPECT_EQ(req.body()->attribute("username").value_or(""), "jbob");

    const Message list = loadFixture("mosListSearchableSchema.xml");
    EXPECT_EQ(list.type(), MessageType::MosListSearchableSchema);
    EXPECT_EQ(list.body()->childText("mosSchema"),
              "http://MOSA4.com/mos/supported_schemas/MOSAXML2.08");

    const Message replace = loadFixture("mosItemReplace.xml");
    EXPECT_EQ(replace.type(), MessageType::MosItemReplace);
    imos::codec::view::MosItemReplace view{*loadFixture("mosItemReplace.xml").body()};
    EXPECT_EQ(replace.body()->childText("roID"), "5PM");
}

// 25. The whole advanced workflow also runs over the UTF-8 wire encoding
//     (per-peer §3.4 option): create, search, and item replace unchanged.
TEST(Profile3, WorkflowOverUtf8Wire) {
    TwoRoleFixture::Options options;
    options.wireEncoding = imos::encoding::WireEncoding::Utf8;
    TwoRoleFixture fx{options};
    AdvancedObjectSource source;
    fx.device->attach(source);
    fx.connectAll();

    ASSERT_TRUE(fx.ncs->createObject(makeMosObjCreate("Hotel Fire", "VIDEO", "59.94")).ok());
    fx.settle();
    const std::vector<DirectoryEvent> events = drainDirectory(*fx.ncs);
    const DirectoryEvent* created = firstOfType(events, DirectoryEventType::ObjectCreated);
    ASSERT_NE(created, nullptr);
    ASSERT_NE(fx.ncs->object(created->objID), nullptr);

    ObjectQuery query;
    query.queryID = "q-utf8";
    ASSERT_TRUE(fx.ncs->queryObjects(query).ok());
    fx.settle();
    std::optional<DirectoryQueryPage> page = fx.ncs->nextQueryPage();
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(page->listReturnTotal, 1U);
}

// =========================================================================
// Part 3 — edge and failure paths (the M5 coverage gate's cold branches)
// =========================================================================

// 26. Grammar edges: fractional literals, every comparison operator, the
//     conversion and boolean functions, bare-operand truthiness, and the
//     remaining reject branches.
TEST(Profile3XPath, GrammarEdges) {
    const std::string bob = "<Presenter>Bob</Presenter>";
    const std::string half = "<Ratio>0.5</Ratio>";
    EXPECT_TRUE(fieldMatchesPayload("/Ratio [. = .5]", half));
    EXPECT_TRUE(fieldMatchesPayload("/Ratio [. <= .5 and . >= 0.25]", half));
    EXPECT_TRUE(fieldMatchesPayload("/Ratio [. < 1 OR . > 7]", half));
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [. != 5]", bob));  // NaN: only != holds
    EXPECT_FALSE(fieldMatchesPayload("/Presenter [. >= 5]", bob));
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [. > 'Alice']", bob));  // string relation
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [. <= 'Bob']", bob));
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [string(5) = '5']", bob));
    EXPECT_TRUE(fieldMatchesPayload("/Ratio [number('0.5') = .5]", half));
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [boolean(. = 'Bob')]", bob));
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [.]", bob));    // truthy: non-empty
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [1]", bob));    // truthy: non-zero
    EXPECT_FALSE(fieldMatchesPayload("/Presenter [0]", bob));   // falsy: zero
    EXPECT_TRUE(fieldMatchesPayload("/Presenter ['x']", bob));  // truthy: literal
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [not(false())]", bob));
    EXPECT_TRUE(fieldMatchesPayload("/Presenter [number(.) != number(.)]", bob));  // NaN != NaN

    EXPECT_FALSE(parseFieldXPath("/A [not(true()]").ok());  // ')' missing
    EXPECT_FALSE(parseFieldXPath("/A [boolean(true()]").ok());
    EXPECT_FALSE(parseFieldXPath("/A [true(]").ok());
    EXPECT_FALSE(parseFieldXPath("/A [string(. = 'x']").ok());
    EXPECT_FALSE(parseFieldXPath("/A [. = ]").ok());       // operand expected
    EXPECT_FALSE(parseFieldXPath("/A [. = 'x' @]").ok());  // unexpected character
    EXPECT_FALSE(parseFieldXPath("/A [. !").ok());         // '!' without '='
}

// 27. generalSearch operator edges: bare negation, dangling operators, and
//     alternatives with no terms.
TEST(Profile3GeneralSearch, OperatorEdges) {
    const std::string corpus = "a man walked home";
    EXPECT_TRUE(evaluateGeneralSearch("not dog", corpus));  // only-negated group
    EXPECT_FALSE(evaluateGeneralSearch("not man", corpus));
    EXPECT_TRUE(evaluateGeneralSearch("man or", corpus));      // trailing operator
    EXPECT_TRUE(evaluateGeneralSearch("or man", corpus));      // leading operator
    EXPECT_TRUE(evaluateGeneralSearch("man not", corpus));     // dangling not
    EXPECT_TRUE(evaluateGeneralSearch("and or not", corpus));  // no terms at all
}

// 28. Directory/API validation reject paths and the objAction vocabulary.
TEST(Profile3, ApiValidationRejects) {
    using imos::profiles::objActionFromName;
    using imos::profiles::objActionName;
    EXPECT_EQ(objActionName(ObjAction::New), "NEW");
    EXPECT_EQ(objActionName(ObjAction::Update), "UPDATE");
    EXPECT_EQ(objActionName(ObjAction::Delete), "DELETE");
    EXPECT_FALSE(objActionFromName("new").has_value());  // closed, case-sensitive
    EXPECT_FALSE(objActionFromName("MODIFY").has_value());

    TwoRoleFixture fx;
    fx.connectAll();
    // createObject wants a mosObjCreate.
    EXPECT_FALSE(fx.ncs->createObject(imos::codec::makeMessage(MessageType::MosReqObj)).ok());
    // requestObjAction wants a mosReqObjAction and (for UPDATE/DELETE) an objID.
    EXPECT_FALSE(fx.ncs
                     ->requestObjAction(ObjAction::Update, "X",
                                        imos::codec::makeMessage(MessageType::MosReqObj))
                     .ok());
    EXPECT_FALSE(fx.ncs->requestObjAction(ObjAction::Delete, "").ok());
    // query shape validation happens before anything reaches the wire.
    ObjectQuery bad;
    bad.listReturnStart = 0;
    EXPECT_FALSE(fx.ncs->queryObjects(bad).ok());
    // replaceItem validation: coordinates and a keyed <item> are mandatory.
    const XmlNode item = parseSubtree("<item><itemID>i1</itemID></item>");
    EXPECT_FALSE(fx.device->replaceItem("", "s1", item).ok());
    const XmlNode unkeyed = parseSubtree("<item><objID>o</objID></item>");
    EXPECT_FALSE(fx.device->replaceItem("RO1", "s1", unkeyed).ok());

    // An unknown operation attribute on the wire is a NACK, never a guess.
    const std::string xml = "<mos><mosID>" + std::string{TwoRoleFixture::kMosID} +
                            "</mosID><ncsID>" + std::string{TwoRoleFixture::kNcsID} +
                            "</ncsID><messageID>OP-1</messageID>"
                            "<mosReqObjAction operation=\"MODIFY\" objID=\"X\"/></mos>";
    auto& momOut = fx.deviceTransport.outbound[static_cast<std::size_t>(Channel::Mom)];
    momOut.clear();
    fx.device->onFrameReceived(Channel::Mom, encode(xml));
    ASSERT_FALSE(momOut.empty());
    const Message nack = parseFrame(momOut.front());
    ASSERT_EQ(nack.type(), MessageType::MosAck);
    EXPECT_EQ(nack.body()->childText("status"), "NACK");
}

// 29. Delivery failure on a mosItemReplace: retries lapse (frames dropped),
//     the result surfaces as failed, and the link fault stays visible.
TEST(Profile3, ItemReplaceDeliveryFailureSurfaces) {
    TwoRoleFixture fx;
    fx.connectAll();
    const XmlNode item = parseSubtree("<item><itemID>i1</itemID></item>");
    ASSERT_TRUE(fx.device->replaceItem("RO1", "s1", item).ok());

    auto& roOut = fx.deviceTransport.outbound[static_cast<std::size_t>(Channel::Ro)];
    for (int attempt = 0; attempt < 8; ++attempt) {
        roOut.clear();  // drop every send and retry on the floor
        fx.clock.advanceSeconds(11);
        fx.device->poll();
    }
    std::optional<imos::profiles::ItemReplaceResult> result = fx.device->nextItemReplaceResult();
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->ok);
    EXPECT_EQ(result->roStatus, "delivery failed");
}

// 30. Query answers when no object source is attached: a zero-total page
//     whose status names the problem (the exchange still completes).
TEST(Profile3, QueryWithoutSourceAnswersEmpty) {
    TwoRoleFixture fx;
    fx.connectAll();
    ObjectQuery query;
    query.queryID = "no-source";
    ASSERT_TRUE(fx.ncs->queryObjects(query).ok());
    ASSERT_TRUE(fx.ncs->requestSearchableSchemas().ok());
    fx.settle();
    std::optional<DirectoryQueryPage> page = fx.ncs->nextQueryPage();
    ASSERT_TRUE(page.has_value());
    EXPECT_EQ(page->listReturnTotal, 0U);
    EXPECT_FALSE(page->listReturnStatus.empty());
    EXPECT_TRUE(fx.ncs->searchableSchemas().empty());  // schema reply, no entries
}

// 31. Corpus hygiene in evaluateQuery: null and non-mosObj entries are
//     skipped, and a mixed numeric/string sort key falls back to string
//     comparison rather than misordering.
TEST(Profile3Query, CorpusHygieneAndMixedSortKeys) {
    const std::string schema = "http://s/1";
    const Message a = makeObjectWithPayload("A", "clip", schema, "<K>beta</K>");
    const Message b = makeObjectWithPayload("B", "clip", schema, "<K>42</K>");
    const Message notObj = imos::codec::makeMessage(MessageType::MosReqObj);
    const std::vector<const Message*> corpus{nullptr, &notObj, &a, &b};

    ObjectQuery query;
    query.queryID = "q";
    query.mosSchema = schema;
    ObjectQueryGroup group;
    group.fields.push_back(ObjectQueryField{parseXPathOrDie("/K"), 1, false});
    query.groups = {group};
    auto page = evaluateQuery(query, corpus);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().objects.size(), 2U);
    EXPECT_EQ(page.value().objects[0].body()->childText("objID"), "B");  // "42" < "beta"
}
