// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 6 — MOS Redirection conformance (plan §3.2 row 6, §10, M5). A
// real imos::Device against a real incs::Ncs over the loopback pair. The
// profile adds no messages, so what is exercised here is the FQID
// convention end to end: ingested storyItem/item references classified
// into the delivered StoryView, exactly one ForeignObjectReferenced event
// per foreign reference, the rejectForeignReferences NACK path, the incs
// authoring-time mirror, bring-up identity enforcement (validateConfig and
// the defensive construction path), listMachInfo advertising Profile 6 —
// and the all-off default changing nothing at all.

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <imos/codec/message.h>
#include <imos/fqid.h>
#include <imos/profiles/profile4.h>
#include <imos/profiles/profile6.h>

#include "two_role_fixture.h"

using imos::Locality;
using imos::codec::parseMessage;
using imos::codec::XmlNode;
using imos::test::TwoRoleFixture;
using incs::RunningOrderAuthor;

namespace {

// A fully qualified installation: this device, a sibling machine, a
// foreign enterprise, and a legacy plain mosID.
constexpr const char* kSelf = "aveed.server1.camden.cbs.mos";
constexpr const char* kSibling = "aveed.server2.camden.cbs.mos";
constexpr const char* kForeign = "tornado.mach2.wjla.allbritton.mos";
constexpr const char* kLegacy = "ARCHIVE-1";
constexpr const char* kFqidNcs = "newsroom.desk1.camden.cbs.mos";

TwoRoleFixture::Options fqidOptions(imos::profiles::Profile6Config deviceProfile6,
                                    imos::profiles::Profile6Config ncsProfile6 = {}) {
    TwoRoleFixture::Options options;
    options.mosID = kSelf;
    options.ncsID = kFqidNcs;
    options.deviceProfile6 = deviceProfile6;
    options.ncsProfile6 = ncsProfile6;
    return options;
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

std::string storyItemXml(const std::string& itemID, const std::string& objID,
                         const std::string& mosID) {
    return "<storyItem><itemID>" + itemID + "</itemID><objID>" + objID + "</objID><mosID>" + mosID +
           "</mosID></storyItem>";
}

/// The four-locality storyBody the classification scenarios ingest.
XmlNode fourLocalityBody() {
    return makeStoryBody("<p>Lead-in</p>" + storyItemXml("i-self", "OBJ-SELF", kSelf) +
                         storyItemXml("i-sib", "OBJ-SIB", kSibling) +
                         storyItemXml("i-for", "OBJ-FOR", kForeign) +
                         storyItemXml("i-old", "OBJ-OLD", kLegacy));
}

/// Story consumer recording each delivered StoryView's references.
class ReferenceRecordingSink : public imos::profiles::IStorySink {
public:
    std::vector<std::vector<imos::profiles::ClassifiedReference>> received;

    imos::Result<void> onStory(const imos::profiles::StoryView& story) override {
        received.push_back(story.references);
        return {};
    }
};

/// Drain the device's app events, returning ForeignObjectReferenced
/// diagnostics (and asserting no ProtocolError slipped through).
std::vector<std::string> drainForeignEvents(imos::Device& device) {
    std::vector<std::string> diagnostics;
    while (std::optional<imos::SessionEvent> event = device.nextEvent()) {
        if (event->type == imos::SessionEventType::ForeignObjectReferenced) {
            diagnostics.push_back(event->diagnostic);
        } else {
            EXPECT_NE(event->type, imos::SessionEventType::ProtocolError) << event->diagnostic;
        }
    }
    return diagnostics;
}

std::vector<incs::CommitEvent> drainCommits(incs::Ncs& ncs) {
    std::vector<incs::CommitEvent> events;
    while (std::optional<incs::CommitEvent> event = ncs.author().nextEvent()) {
        events.push_back(std::move(*event));
    }
    return events;
}

/// Author RO1 with one plain story s1 and converge both roles.
void convergeRo1(TwoRoleFixture& fx) {
    ASSERT_TRUE(fx.ncs->author().create("RO1", "5PM Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", RunningOrderAuthor::makeStory("s1")).ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();
    for (const incs::CommitEvent& event : drainCommits(*fx.ncs)) {
        ASSERT_TRUE(event.ok) << event.roStatus;
    }
    ASSERT_TRUE(fx.device->runningOrderModel().has("RO1"));
}

}  // namespace

// 1. The flagship classification scenario: an RO story references (a) this
//    device, (b) a sibling machine, (c) a foreign enterprise, (d) a legacy
//    plain mosID. The delivered StoryView carries all four classified in
//    document order, and exactly one ForeignObjectReferenced event fires.
TEST(Profile6, StorySendClassifiesReferencesAndFlagsForeign) {
    imos::profiles::Profile6Config config;
    config.flagForeignReferences = true;
    TwoRoleFixture fx{fqidOptions(config)};
    ReferenceRecordingSink sink;
    fx.device->attach(sink);
    fx.connectAll();
    convergeRo1(fx);
    (void)drainForeignEvents(*fx.device);  // discard bring-up noise checks

    ASSERT_TRUE(fx.ncs->author().setStoryBody("RO1", "s1", fourLocalityBody()).ok());
    ASSERT_TRUE(fx.ncs->sendStory("RO1", "s1").ok());
    fx.settle();
    for (const incs::CommitEvent& event : drainCommits(*fx.ncs)) {
        ASSERT_TRUE(event.ok) << event.roStatus;
    }

    ASSERT_EQ(sink.received.size(), 1u);
    const auto& references = sink.received.front();
    ASSERT_EQ(references.size(), 4u);
    EXPECT_EQ(references[0].mosID, kSelf);
    EXPECT_EQ(references[0].locality, Locality::Self);
    EXPECT_EQ(references[1].mosID, kSibling);
    EXPECT_EQ(references[1].locality, Locality::SameEnterprise);
    EXPECT_EQ(references[2].mosID, kForeign);
    EXPECT_EQ(references[2].locality, Locality::Foreign);
    EXPECT_EQ(references[2].objID, "OBJ-FOR");
    EXPECT_EQ(references[2].storyID, "s1");
    EXPECT_EQ(references[3].mosID, kLegacy);
    EXPECT_EQ(references[3].locality, Locality::NotFqid);

    const std::vector<std::string> foreign = drainForeignEvents(*fx.device);
    ASSERT_EQ(foreign.size(), 1u);  // exactly one: only (c) is foreign
    EXPECT_NE(foreign.front().find(kForeign), std::string::npos);
    EXPECT_NE(foreign.front().find("OBJ-FOR"), std::string::npos);

    // The convenience query agrees with what was delivered.
    EXPECT_EQ(fx.device->classifyMosID(kSibling), Locality::SameEnterprise);
    EXPECT_EQ(fx.device->classifyMosID(kForeign), Locality::Foreign);
}

// 2. The Profile 2 ingest path classifies too: a roCreate whose story
//    carries a foreign <item> is accepted (flagging is not an error) and
//    fires exactly one event.
TEST(Profile6, ConstructionFlagsForeignItems) {
    imos::profiles::Profile6Config config;
    config.flagForeignReferences = true;
    TwoRoleFixture fx{fqidOptions(config)};
    fx.connectAll();

    XmlNode story = RunningOrderAuthor::makeStory("s1");
    story.children.push_back(RunningOrderAuthor::makeItem("i1", "OBJ1", kSelf));
    story.children.push_back(RunningOrderAuthor::makeItem("i2", "OBJ2", kForeign));
    ASSERT_TRUE(fx.ncs->author().create("RO1", "5PM Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", story).ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();

    for (const incs::CommitEvent& event : drainCommits(*fx.ncs)) {
        ASSERT_TRUE(event.ok) << event.roStatus;
    }
    EXPECT_TRUE(fx.device->runningOrderModel().has("RO1"));
    const std::vector<std::string> foreign = drainForeignEvents(*fx.device);
    ASSERT_EQ(foreign.size(), 1u);
    EXPECT_NE(foreign.front().find(kForeign), std::string::npos);
}

// 3. rejectForeignReferences turns the foreign reference into the NACK
//    itself: the construction message never reaches the model.
TEST(Profile6, RejectForeignNacksConstruction) {
    imos::profiles::Profile6Config config;
    config.rejectForeignReferences = true;
    TwoRoleFixture fx{fqidOptions(config)};
    fx.connectAll();

    XmlNode story = RunningOrderAuthor::makeStory("s1");
    story.children.push_back(RunningOrderAuthor::makeItem("i1", "OBJ1", kForeign));
    ASSERT_TRUE(fx.ncs->author().create("RO1", "5PM Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", story).ok());
    ASSERT_TRUE(fx.ncs->author().commit("RO1").ok());
    fx.settle();

    const std::vector<incs::CommitEvent> commits = drainCommits(*fx.ncs);
    ASSERT_EQ(commits.size(), 1u);
    EXPECT_FALSE(commits.front().ok);
    EXPECT_NE(commits.front().roStatus.find("foreign object reference"), std::string::npos);
    EXPECT_NE(commits.front().roStatus.find(kForeign), std::string::npos);
    EXPECT_FALSE(fx.device->runningOrderModel().has("RO1"));
    // Rejection is the NACK, not an event (flagForeignReferences is off).
    EXPECT_TRUE(drainForeignEvents(*fx.device).empty());
}

// 4. The same rejection on the Profile 4 path: a roStorySend with a
//    foreign storyItem NACKs before storage or sink delivery.
TEST(Profile6, RejectForeignNacksStorySend) {
    imos::profiles::Profile6Config config;
    config.rejectForeignReferences = true;
    TwoRoleFixture fx{fqidOptions(config)};
    ReferenceRecordingSink sink;
    fx.device->attach(sink);
    fx.connectAll();
    convergeRo1(fx);

    ASSERT_TRUE(fx.ncs->author().setStoryBody("RO1", "s1", fourLocalityBody()).ok());
    ASSERT_TRUE(fx.ncs->sendStory("RO1", "s1").ok());
    fx.settle();

    const std::vector<incs::CommitEvent> commits = drainCommits(*fx.ncs);
    ASSERT_EQ(commits.size(), 1u);
    EXPECT_FALSE(commits.front().ok);
    EXPECT_NE(commits.front().roStatus.find("foreign object reference"), std::string::npos);
    EXPECT_TRUE(sink.received.empty());
    EXPECT_EQ(fx.device->story("RO1", "s1"), nullptr);
}

// 5. All-off defaults: with Profile 6 unconfigured the same foreign-laden
//    traffic is accepted silently — no events, no classifications, no
//    behavior change for non-claiming devices.
TEST(Profile6, DefaultsChangeNothing) {
    TwoRoleFixture fx;  // stock fixture: plain IDs, profile6 all-off
    ReferenceRecordingSink sink;
    fx.device->attach(sink);
    fx.connectAll();
    convergeRo1(fx);

    ASSERT_TRUE(fx.ncs->author().setStoryBody("RO1", "s1", fourLocalityBody()).ok());
    ASSERT_TRUE(fx.ncs->sendStory("RO1", "s1").ok());
    fx.settle();
    for (const incs::CommitEvent& event : drainCommits(*fx.ncs)) {
        ASSERT_TRUE(event.ok) << event.roStatus;
    }

    ASSERT_EQ(sink.received.size(), 1u);
    EXPECT_TRUE(sink.received.front().empty());  // no classifier, no references
    EXPECT_TRUE(drainForeignEvents(*fx.device).empty());
    EXPECT_EQ(fx.device->classifyMosID(kForeign), Locality::NotFqid);
}

// 6. Bring-up enforcement: requireFqidSelf with a plain mosID fails the
//    validateConfig gate; a Device constructed anyway runs with Profile 6
//    disabled and surfaces the diagnostic as one ProtocolError event.
TEST(Profile6, BringUpEnforcementRejectsPlainSelf) {
    imos::profiles::Profile6Config config;
    config.requireFqidSelf = true;

    imos::DeviceConfig bad{TwoRoleFixture::sessionConfig(),
                           TwoRoleFixture::machineInfo(imos::profiles::DeviceType::Mos)};
    bad.session.mosID = "VTR-1";
    bad.profile6 = config;
    const imos::Result<void> invalid = imos::Device::validateConfig(bad);
    ASSERT_FALSE(invalid.ok());
    EXPECT_NE(invalid.error().message.find("Fully Qualified MOS ID"), std::string::npos);
    EXPECT_NE(invalid.error().message.find("VTR-1"), std::string::npos);

    imos::DeviceConfig good = bad;
    good.session.mosID = kSelf;
    EXPECT_TRUE(imos::Device::validateConfig(good).ok());

    // Defensive construction path: the misconfigured device still runs,
    // Profile 6 stays off, and the diagnostic surfaces exactly once.
    TwoRoleFixture::Options options;
    options.mosID = "VTR-1";
    options.deviceProfile6 = config;
    TwoRoleFixture fx{options};
    fx.connectAll();
    fx.settle();
    int protocolErrors = 0;
    while (std::optional<imos::SessionEvent> event = fx.device->nextEvent()) {
        if (event->type == imos::SessionEventType::ProtocolError) {
            ++protocolErrors;
            EXPECT_NE(event->diagnostic.find("Fully Qualified MOS ID"), std::string::npos);
        }
    }
    EXPECT_EQ(protocolErrors, 1);
    EXPECT_EQ(fx.device->classifyMosID(kSelf), Locality::NotFqid);
}

// 7. The incs mirror of the gate: identity on the NCS's own ncsID, and —
//    with classification on — the peer device's mosID as the base.
TEST(Profile6, NcsValidateConfigMirrorsTheGate) {
    imos::profiles::Profile6Config identityOnly;
    identityOnly.requireFqidSelf = true;

    incs::NcsConfig config{TwoRoleFixture::sessionConfig(),
                           TwoRoleFixture::machineInfo(imos::profiles::DeviceType::Ncs)};
    config.profile6 = identityOnly;
    config.session.ncsID = "NCS-1";  // plain: identity check must fail
    config.session.mosID = kSelf;
    EXPECT_FALSE(incs::Ncs::validateConfig(config).ok());

    config.session.ncsID = kFqidNcs;
    EXPECT_TRUE(incs::Ncs::validateConfig(config).ok());
    // Identity-only does not constrain the peer: a legacy device is fine.
    config.session.mosID = "VTR-1";
    EXPECT_TRUE(incs::Ncs::validateConfig(config).ok());

    // Classification needs the peer device's mosID as its base.
    config.profile6.rejectForeignReferences = true;
    EXPECT_FALSE(incs::Ncs::validateConfig(config).ok());
    config.session.mosID = kSelf;
    EXPECT_TRUE(incs::Ncs::validateConfig(config).ok());
}

// 8. Authoring-time validation (the incs mirror in action): with
//    rejectForeignReferences, edits and story bodies referencing an object
//    foreign to the peer device fail at authoring time, before the wire.
TEST(Profile6, NcsRejectsForeignReferencesAtAuthoringTime) {
    imos::profiles::Profile6Config reject;
    reject.rejectForeignReferences = true;
    TwoRoleFixture fx{fqidOptions({}, reject)};
    fx.connectAll();

    ASSERT_TRUE(fx.ncs->author().create("RO1", "5PM Rundown").ok());
    ASSERT_TRUE(fx.ncs->author().insertStory("RO1", RunningOrderAuthor::makeStory("s1")).ok());

    // Reachable references author fine; the foreign one fails with the
    // diagnostic, at the edit — nothing to NACK later.
    ASSERT_TRUE(fx.ncs->author()
                    .insertItem("RO1", "s1", RunningOrderAuthor::makeItem("i1", "O1", kSelf))
                    .ok());
    ASSERT_TRUE(fx.ncs->author()
                    .insertItem("RO1", "s1", RunningOrderAuthor::makeItem("i2", "O2", kSibling))
                    .ok());
    const imos::Result<void> foreignEdit = fx.ncs->author().insertItem(
        "RO1", "s1", RunningOrderAuthor::makeItem("i3", "O3", kForeign));
    ASSERT_FALSE(foreignEdit.ok());
    EXPECT_EQ(foreignEdit.error().code, imos::ErrorCode::InvalidArgument);
    EXPECT_NE(foreignEdit.error().message.find(kForeign), std::string::npos);

    const imos::Result<void> foreignBody =
        fx.ncs->author().setStoryBody("RO1", "s1", fourLocalityBody());
    ASSERT_FALSE(foreignBody.ok());
    EXPECT_NE(foreignBody.error().message.find("foreign object reference"), std::string::npos);
    ASSERT_TRUE(
        fx.ncs->author()
            .setStoryBody("RO1", "s1",
                          makeStoryBody("<p>ok</p>" + storyItemXml("i-sib", "O2", kSibling)))
            .ok());

    EXPECT_EQ(fx.ncs->classifyMosID(kSibling), Locality::SameEnterprise);
    EXPECT_EQ(fx.ncs->classifyMosID(kLegacy), Locality::NotFqid);
}

// 9. A claiming device advertises Profile 6 in listMachInfo; a
//    non-claiming one does not.
TEST(Profile6, ListMachInfoAdvertisesTheClaim) {
    imos::profiles::Profile6Config config;
    config.requireFqidSelf = true;
    TwoRoleFixture fx{fqidOptions(config)};
    fx.connectAll();
    ASSERT_TRUE(fx.ncs->requestMachineInfo().ok());
    fx.settle();
    ASSERT_NE(fx.ncs->peerMachineInfo(), nullptr);
    EXPECT_TRUE(fx.ncs->peerMachineInfo()->info.profiles[6]);

    TwoRoleFixture stock;
    stock.connectAll();
    ASSERT_TRUE(stock.ncs->requestMachineInfo().ok());
    stock.settle();
    ASSERT_NE(stock.ncs->peerMachineInfo(), nullptr);
    EXPECT_FALSE(stock.ncs->peerMachineInfo()->info.profiles[6]);
}
