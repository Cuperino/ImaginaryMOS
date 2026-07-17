// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 5 — Item Control (plan §3.2 row 5, milestone M4). Two messages on
// the ro channel let the newsroom drive playout: roCtrl issues READY /
// EXECUTE / PAUSE / STOP / SIGNAL at running-order, story, or item scope
// (spec §3.7.3 — the scope is selected by leaving the lower-level IDs
// empty), and roItemCue is a timed *notification* of an item event (spec
// §3.7.2 — feedback about the execution point, never a command). Cues are
// often prompter-originated and may be addressed to the item's parent MOS,
// in which case the envelope's <ncsID> slot carries the parent MOS name —
// the spec's deliberately counterintuitive assignment.
//
// Profile5Responder is the device (imos) side: it scope-resolves inbound
// roCtrl into the host's IControlSink (whose outcome drives the roAck),
// validates targets against the linked Profile 2 shadow model — an unknown
// target NACKs and escalates to the §3.3 resync driver — ingests roItemCue
// as an always-acked notification, and originates prompter-style cues.
// Profile5NcsResponder is the NCS (incs) side: thin roCtrl/roItemCue
// origination with roAck resolution, plus ingest of device-originated cues.

#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <imos/codec/message.h>
#include <imos/codec/xml.h>
#include <imos/core_export.h>
#include <imos/error.h>
#include <imos/profiles/profile2.h>
#include <imos/session/peer_session.h>
#include <imos/session/runtime.h>

namespace imos::profiles {

// ---- shared vocabulary --------------------------------------------------

/// The roCtrl command vocabulary — a closed enum per the spec's DTD
/// (<!ELEMENT command (READY|EXECUTE|PAUSE|STOP|SIGNAL)>); anything else
/// is a NACK, never a guess.
enum class CtrlCommand : std::uint8_t { Ready, Execute, Pause, Stop, Signal };

/// The level a roCtrl addresses, selected by leaving lower-level IDs empty:
/// roID alone → the whole RO; roID+storyID → one story; all three → one item.
enum class CtrlScope : std::uint8_t { RunningOrder, Story, Item };

[[nodiscard]] IMOS_CORE_API std::string_view ctrlCommandName(CtrlCommand command) noexcept;
[[nodiscard]] IMOS_CORE_API std::optional<CtrlCommand> ctrlCommandFromName(
    std::string_view name) noexcept;

/// A decoded, scope-resolved roCtrl handed to the device host. Unused ID
/// levels are empty per the scope. `message` references the retained wire
/// message (unknown tags and mosExternalMetadata riders intact); it is only
/// set during the IControlSink callback — queued copies from nextCtrl()
/// carry nullptr.
struct CtrlRequest {
    CtrlScope scope = CtrlScope::RunningOrder;
    CtrlCommand command = CtrlCommand::Ready;
    std::string roID;
    std::string storyID;
    std::string itemID;
    const codec::Message* message = nullptr;
};

/// One roItemCue: received (a notification of an item event at a time) or
/// to send. `mosID` is the *target* MOS from the item reference embedded in
/// the story — not the envelope originator. `roEventTime` is the verbatim
/// field; `eventTime` is its parseSpecTime() reading when parseable (unused
/// on send — the string is what goes to the wire). `metadata` carries
/// verbatim <mosExternalMetadata> blocks in both directions. `message` is
/// set only during the IControlSink callback, like CtrlRequest.
struct ItemCue {
    std::string mosID;
    std::string roID;
    std::string storyID;
    std::string itemID;
    std::string roEventType;
    std::string roEventTime;
    std::optional<WallClockTime> eventTime;
    std::vector<codec::XmlNode> metadata;
    const codec::Message* message = nullptr;
};

/// Parse and scope-resolve a roCtrl. Lenient on shape per the plan: an
/// empty element and an absent element both mean "scope boundary". Fails on
/// a missing roID, an itemID without a storyID (an inconsistent shape is a
/// diagnostic NACK, not a guess), and an unknown command word.
[[nodiscard]] IMOS_CORE_API Result<CtrlRequest> parseRoCtrl(const codec::Message& message);

/// Parse a roItemCue. Lenient — a cue is a notification, so absent fields
/// stay empty rather than failing; only a non-roItemCue message is an error.
[[nodiscard]] IMOS_CORE_API Result<ItemCue> parseRoItemCue(const codec::Message& message);

// ---- device (imos) side -------------------------------------------------

/// Host control consumer (plan §4.6). onCtrl's outcome drives the roAck:
/// success acks "OK", an error NACKs with its message as the detail. A cue
/// is a notification — it is always acked, so onItemCue returns nothing.
class IControlSink {
public:
    virtual Result<void> onCtrl(const CtrlRequest& request) = 0;
    virtual void onItemCue(const ItemCue& cue) = 0;
    virtual ~IControlSink() = default;
};

/// Device-side Profile 5 service. `profile2` (optional) links the shadow
/// model: a roCtrl naming a target the model does not hold is NACKed and
/// escalated to the §3.3 resync driver (the shadow is presumed stale).
/// Without it (or with Profile 2 in raw-sink mode) targets are taken on
/// faith — the host owns RO state.
class IMOS_CORE_API Profile5Responder {
public:
    Profile5Responder(PeerSession* session, Profile2Responder* profile2);

    /// Attach the host's control consumer. Until one is attached, resolved
    /// requests and cues are acked "OK" and queue for nextCtrl()/nextItemCue().
    void setSink(IControlSink* sink) noexcept { sink_ = sink; }

    /// Consume one session event: inbound roCtrl or roItemCue.
    bool handle(const SessionEvent& event);

    /// Originate a cue (prompter style). By default the envelope is
    /// addressed with the session's configured IDs; pass `addresseeNcsID`
    /// to address the item's parent MOS instead — its name then rides the
    /// envelope's <ncsID> slot per spec §3.7.2's counterintuitive
    /// assignment. The peer's roAck surfaces as an ordinary AckReceived.
    Result<Seq> sendItemCue(const ItemCue& cue, std::string_view addresseeNcsID = {});

    /// Sink-less drains, in arrival order (already acked; message == nullptr).
    [[nodiscard]] std::optional<CtrlRequest> nextCtrl();
    [[nodiscard]] std::optional<ItemCue> nextItemCue();

private:
    void handleCtrl(const SessionEvent& event);
    void handleItemCue(const SessionEvent& event);
    /// "" when the target resolves in the linked model (or none is linked);
    /// otherwise the NACK detail for the missing element.
    std::string validateTarget(const CtrlRequest& request) const;

    PeerSession* session_;
    Profile2Responder* profile2_;
    IControlSink* sink_ = nullptr;
    std::deque<CtrlRequest> pendingCtrl_;
    std::deque<ItemCue> cues_;
};

// ---- NCS (incs) side ----------------------------------------------------

/// The roAck resolution of one originated roCtrl or roItemCue.
struct CtrlResult {
    Seq seq = 0;
    bool ok = false;
    std::string roStatus;
};

/// NCS-side Profile 5 service: originates roCtrl (scope-checked against the
/// IDs supplied) and roItemCue, resolves their roAcks through nextResult(),
/// and ingests device-originated cues (always acked, drained via
/// nextItemCue()).
class IMOS_CORE_API Profile5NcsResponder {
public:
    explicit Profile5NcsResponder(PeerSession* session);

    /// Issue one command at `scope`. The IDs must match the scope — story
    /// scope wants roID+storyID, item scope all three, RO scope roID alone;
    /// a mismatch is rejected here rather than guessed at by the device.
    /// All three ID elements are emitted (lower levels empty), the spec's
    /// scope-selection shape.
    Result<Seq> sendCtrl(CtrlScope scope, std::string_view roID, std::string_view storyID,
                         std::string_view itemID, CtrlCommand command);

    /// Originate a cue toward the device (cues flow either way).
    Result<Seq> sendItemCue(const ItemCue& cue);

    /// Consume one session event: a roAck resolving one of this responder's
    /// sends, or an inbound device-originated roItemCue.
    bool handle(const SessionEvent& event);

    [[nodiscard]] std::optional<CtrlResult> nextResult();
    [[nodiscard]] std::optional<ItemCue> nextItemCue();

private:
    PeerSession* session_;
    std::set<Seq> pending_;
    std::deque<CtrlResult> results_;
    std::deque<ItemCue> cues_;
};

/// Build the wire messages (shared by both roles; exposed for tests).
[[nodiscard]] IMOS_CORE_API codec::Message buildRoCtrl(std::string_view roID,
                                                       std::string_view storyID,
                                                       std::string_view itemID,
                                                       CtrlCommand command);
[[nodiscard]] IMOS_CORE_API codec::Message buildRoItemCue(const ItemCue& cue);

}  // namespace imos::profiles
