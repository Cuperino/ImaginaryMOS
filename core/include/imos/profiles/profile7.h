// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 7 — MOS RO / Content List Modification (plan §3.2 row 7, milestone
// M4). The MOS (device) requests story create/update/move/delete on the NCS
// via roReqStoryAction, wrapping a roStorySend payload and requesting an
// optional lease lock. The device never self-applies — it waits for the P2/P4
// echo like all other construction traffic (plan §1.2). This module handles
// the device side (roReqStoryAction origination and roAck resolution);
// Profile7NcsResponder (incs/profile7_ncs.h) is the NCS side.

#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imos/codec/message.h>
#include <imos/codec/xml.h>
#include <imos/core_export.h>
#include <imos/error.h>
#include <imos/session/peer_session.h>

namespace imos::profiles {

/// Story action operation vocabulary (roReqStoryAction operation attribute).
enum class StoryAction : std::uint8_t { New, Update, Delete, Move };

/// One roReqStoryAction to send. Populate the fields the operation needs;
/// others stay default. `story` (NEW) may carry an empty storyID for
/// NCS-assignment. `storyBody`/`agencyMetadata` (UPDATE) mirror Profile 4's
/// two body forms — set at most one.
struct StoryActionRequest {
    StoryAction operation;
    std::string roID;
    std::optional<codec::XmlNode> story;           // NEW: <story> incl. nested storyBody
    std::string beforeStoryID;                     // NEW/MOVE anchor
    std::string storyID;                           // UPDATE/DELETE
    std::string storySlug;                         // UPDATE story header slug
    std::string storyNum;                          // UPDATE story header number
    std::vector<std::string> storyIDs;             // MOVE (one or more)
    std::optional<codec::XmlNode> storyBody;       // UPDATE literal script
    std::optional<codec::XmlNode> agencyMetadata;  // UPDATE Read1stMEMasBody
    std::optional<int> leaseSeconds;               // leaseLock seconds
    std::string username;
};

/// The roAck resolution: ok/NACK, the roStatus text, and — only for a
/// successful NEW — the NCS-assigned storyID.
struct StoryActionResult {
    Seq seq = 0;
    bool ok = false;
    std::string roStatus;
    std::string assignedStoryID;  // NEW only: NCS-assigned storyID when empty was sent
};

/// Device-side roReqStoryAction origination. Builds the wire message per
/// the plan's shape rules; resolves through the session's AckReceived event
/// (never self-applies — the device waits for the ordinary P2/P4 echo).
class IMOS_CORE_API Profile7Requester {
public:
    explicit Profile7Requester(PeerSession* session);

    /// Build and enqueue roReqStoryAction; await roAck via nextResult().
    Result<Seq> requestStoryAction(const StoryActionRequest& request);

    /// Consume one session event: AckReceived roAck for our roReqStoryAction.
    bool handle(const SessionEvent& event);

    /// Next resolved roReqStoryAction (outcome from roAck), in order.
    [[nodiscard]] std::optional<StoryActionResult> nextResult();

private:
    PeerSession* session_;
    std::unordered_map<Seq, StoryAction> pending_;  ///< seq → operation type
    std::deque<StoryActionResult> results_;
};

}  // namespace imos::profiles
