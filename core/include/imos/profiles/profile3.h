// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 3 — Advanced Object Based Workflow (plan §3.2 row 3, milestone
// M5, the M3–M5 finale). It upgrades the object workflow from "describe and
// enumerate" to "create, modify, and search", across all three channels:
//
//   - mosObjCreate / mosReqObjAction (mom): the NCS creates placeholders on
//     the device and modifies/deletes the objects it created. A successful
//     create acks with the new objID in statusDescription (the spec's
//     deliberate overload of that field), then pushes the full mosObj on
//     mom — ack first, push after.
//   - mosItemReplace (ro): the device replaces item data *it owns* inside
//     an NCS story. Position is immutable — a replace never moves the item.
//   - mosReqSearchableSchema / mosReqObjList (aux): the schema-driven
//     search API, the spec's recommended resync mechanism over mosReqAll.
//     First real aux-channel traffic (legacy query port 10542).
//
// Profile3Responder is the device (imos) side over the same IObjectSource
// Profile 1 uses (its optional Profile 3 methods). The NCS (incs) side
// lives on ObjectDirectory (declared in profile1.h, Profile 3 members
// implemented here) — search pages, create correlation, and the
// resync-preference upgrade.

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <imos/codec/message.h>
#include <imos/core_export.h>
#include <imos/error.h>
#include <imos/profiles/object_query.h>
#include <imos/profiles/profile1.h>
#include <imos/profiles/profile2.h>
#include <imos/session/peer_session.h>

namespace imos::profiles {

/// The roAck resolution of one device-originated mosItemReplace.
struct ItemReplaceResult {
    Seq seq = 0;
    bool ok = false;
    std::string roID;
    std::string storyID;
    std::string roStatus;  ///< the roAck's <roStatus> text
};

/// Device-side Profile 3 service. Shares Profile 1's IObjectSource (the
/// optional Profile 3 methods) and links Profile 1 for the follow-up mosObj
/// pushes and Profile 2 for the §3.3 escalation when the NCS NACKs a
/// mosItemReplace (the shadow RO is presumed stale) — and for folding an
/// acknowledged replace into the shadow model so both sides converge.
class IMOS_CORE_API Profile3Responder {
public:
    Profile3Responder(PeerSession* session, Profile1Responder* profile1,
                      Profile2Responder* profile2);

    /// Set (or replace) the object source — the same source Profile 1 uses.
    /// Until one is attached, handle() NACKs create/action requests and
    /// answers searches empty.
    void setSource(IObjectSource* source) noexcept { source_ = source; }

    /// Consume one session event: inbound mosObjCreate / mosReqObjAction
    /// (mom), the search family (aux), or a roAck resolving one of this
    /// responder's mosItemReplace sends (ro).
    bool handle(const SessionEvent& event);

    /// Replace an item reference this device owns inside an NCS story
    /// (mosItemReplace, ro channel). `item` is a verbatim <item> subtree and
    /// must carry the target <itemID>. The NCS's roAck resolves through
    /// nextItemReplaceResult(); on an OK ack the replace is folded into the
    /// linked Profile 2 shadow model, on a NACK the §3.3 resync driver is
    /// escalated for the RO.
    Result<Seq> replaceItem(std::string_view roID, std::string_view storyID,
                            const codec::XmlNode& item);
    [[nodiscard]] std::optional<ItemReplaceResult> nextItemReplaceResult();

private:
    struct PendingReplace {
        std::string roID;
        std::string storyID;
        codec::Message message;  ///< folded into the shadow model on OK
    };

    void handleCreate(const SessionEvent& event);
    void handleAction(const SessionEvent& event);
    void handleSchemaRequest(const SessionEvent& event);
    void handleQuery(const SessionEvent& event);
    /// The host's queryObjects, or — when it answers Unsupported — the
    /// library evaluator over the Profile 1 enumeration.
    Result<ObjectQueryPage> runQuery(const ObjectQuery& query);
    bool completeItemReplace(const SessionEvent& event);
    /// NACK reply helper (mosAck echoing the request's messageID).
    void nackRequest(const SessionEvent& event, std::string_view objID, std::string_view reason);
    /// Run the create path shared by mosObjCreate and mosReqObjAction NEW.
    void runCreate(const SessionEvent& event);
    /// Announce the post-action mosObj push: fetch the object, stamp its
    /// <status>, and send it through Profile 1 (mom). Delete synthesizes a
    /// minimal DELETED mosObj — the Profile 1 delete push other observers
    /// (and the directory mirror) key on.
    void pushObjectStatus(std::string_view objID, std::string_view status);

    PeerSession* session_;
    Profile1Responder* profile1_;
    Profile2Responder* profile2_;
    IObjectSource* source_ = nullptr;
    std::map<std::pair<Channel, Seq>, PendingReplace> pendingReplace_;
    std::deque<ItemReplaceResult> replaceResults_;
};

}  // namespace imos::profiles
