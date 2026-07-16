// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 4 — Advanced RO / Content List Workflow (plan §3.2 row 4,
// milestone M4). Two capabilities on top of Profile 2's running-order
// machinery, all on the ro channel: RO enumeration (roReqAll → roListAll,
// headers only; per-RO detail still flows through Profile 2's roReq) and
// full story bodies (roStorySend → roAck), the prompter/publishing feed
// whose Read1stMEMasBody form carries agency payloads (NewsML, …) that must
// reach the host byte-verbatim.
//
// Profile4Responder is the device (imos) side: it ingests roStorySend into a
// prompter-shaped story store (keyed roID/storyID, replace-on-resend),
// surfaces each accepted story to an optional IStorySink, escalates unknown
// roIDs to Profile 2's §3.3 resync driver, and drives cold-start hydration
// (roReqAll → header index → roReq per RO → model). Profile4NcsResponder is
// the NCS (incs) side: it answers roReqAll from the authoring store with a
// headers-only roListAll. Story origination (building roStorySend from
// authored content) lives with the authoring API, incs::RunningOrderAuthor.

#pragma once

#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imos/codec/message.h>
#include <imos/codec/story_body.h>
#include <imos/core_export.h>
#include <imos/error.h>
#include <imos/profiles/profile2.h>
#include <imos/profiles/running_order_model.h>
#include <imos/session/peer_session.h>

namespace imos::profiles {

// ---- device (imos) side ----------------------------------------------

/// One accepted roStorySend, resolved for the host. Pointers reference the
/// retained message tree (valid for the duration of the callback; the same
/// verbatim message is queryable afterwards through Profile4Responder::
/// story()). When body.bodyIsAgencyPayload(), the script is the first MEM's
/// mosPayload — verbatim, never re-normalized.
struct StoryView {
    std::string roID;
    std::string storyID;
    std::string storySlug;
    std::string storyNum;
    codec::ResolvedStoryBody body;
    const codec::Message* message = nullptr;  ///< the full roStorySend
};

/// Host story consumer (prompter, publishing endpoint, archive…). Invoked
/// once per accepted roStorySend; a resend of the same roID/storyID replaces
/// the previous content. Return success to roAck "OK", or an error whose
/// message becomes the NACK detail.
class IStorySink {
public:
    virtual Result<void> onStory(const StoryView& story) = 0;
    virtual ~IStorySink() = default;
};

/// Device-side Profile 4 service. `profile2` (optional) links the two
/// profiles: with it, a roStorySend naming an RO the shadow model does not
/// know is NACKed and escalated to the §3.3 resync driver, and hydration
/// fans the roListAll headers out into per-RO roReq rebuilds. Without it
/// (or with Profile 2 in raw-sink mode) stories are accepted as-is — the
/// host owns RO state.
class IMOS_CORE_API Profile4Responder {
public:
    Profile4Responder(PeerSession* session, Profile2Responder* profile2);

    /// Attach the host's story consumer. Until one is attached, accepted
    /// stories are only stored (still acked, still queryable).
    void setSink(IStorySink* sink) noexcept { sink_ = sink; }

    /// Consume one session event: inbound roStorySend, or the roListAll
    /// completing this responder's roReqAll.
    bool handle(const SessionEvent& event);

    /// Enumerate every MOS-active RO on the NCS (roReqAll). The roListAll
    /// reply replaces the header index wholesale; with `hydrate` (and a
    /// linked Profile 2 in model mode) each listed RO is then fetched via
    /// roReq so a cold-started device converges on the full state.
    Result<Seq> requestAllRunningOrders(bool hydrate);

    // ---- RO header index (populated by the roListAll reply) -------------

    [[nodiscard]] std::vector<std::string> headerIDs() const;
    /// The verbatim <ro> header subtree from the last roListAll, or nullptr.
    [[nodiscard]] const codec::XmlNode* header(std::string_view roID) const;

    // ---- story store (prompter-shaped, replace-on-resend) ---------------

    /// The last accepted roStorySend for this story, verbatim, or nullptr.
    [[nodiscard]] const codec::Message* story(std::string_view roID,
                                              std::string_view storyID) const;
    [[nodiscard]] std::vector<std::string> storyIDs(std::string_view roID) const;
    /// Drop stored stories (all for an RO, or one story). No-op if absent.
    void eraseStories(std::string_view roID);
    void eraseStory(std::string_view roID, std::string_view storyID);

private:
    void handleStorySend(const SessionEvent& event);
    void completeListAll(const SessionEvent& event);

    PeerSession* session_;
    Profile2Responder* profile2_;
    IStorySink* sink_ = nullptr;
    std::unordered_map<Seq, bool> pendingListAll_;   ///< roReqAll seq → hydrate?
    std::map<std::string, codec::XmlNode> headers_;  ///< roID → verbatim <ro>
    std::map<std::string, std::map<std::string, codec::Message>> stories_;
};

// ---- NCS (incs) side --------------------------------------------------

/// NCS-side Profile 4 service: answers the device's roReqAll from the
/// authoring store with a headers-only roListAll (terminal reply, echoing
/// the request's messageID). Story and item detail never rides roListAll —
/// the device asks per RO with Profile 2's roReq.
class IMOS_CORE_API Profile4NcsResponder {
public:
    explicit Profile4NcsResponder(PeerSession* session);

    /// The authoring store to enumerate (incs wires the author's desired
    /// state). Until set, roReqAll is answered with an empty roListAll.
    void setStore(const RunningOrderModel* store) noexcept { store_ = store; }

    bool handle(const SessionEvent& event);

private:
    PeerSession* session_;
    const RunningOrderModel* store_ = nullptr;
};

}  // namespace imos::profiles
