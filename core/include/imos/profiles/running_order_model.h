// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// RunningOrderModel — the device-side shadow running-order store of plan
// §4.6 (Profile 2, milestone M3). It applies the roConstruction family and
// roElementAction mutations transactionally (all-or-nothing per message; on
// failure nothing mutates and the error text carries the NACK reason),
// answers "what is the current playlist" queries, and tracks per-element
// status for roElementStat emission. Stories and items are retained as
// verbatim subtrees, so unknown tags and mosExternalMetadata ride through
// into queries and status reports untouched.
//
// Error vocabulary (drives the Profile2Responder's §3.3 resync heuristic):
// ErrorCode::NotFound marks a reference to an unknown roID/storyID/itemID —
// the spec's "assume a prior communication error" trigger. Structural
// problems (bad operation, mixed sources, duplicate IDs) are
// ProtocolViolation/InvalidArgument and yield a plain NACK, never a resync.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imos/codec/message.h>
#include <imos/core_export.h>
#include <imos/error.h>

namespace imos::profiles {

/// The element attribute vocabulary of roElementStat (RO / STORY / ITEM).
enum class RoElement : std::uint8_t { RunningOrder, Story, Item };

[[nodiscard]] IMOS_CORE_API std::string_view roElementName(RoElement element) noexcept;
[[nodiscard]] IMOS_CORE_API std::optional<RoElement> roElementFromName(
    std::string_view name) noexcept;

/// The spec's metadata merge, shared by mosItemReplace (§3.3.2) and the
/// mosObj "UPDATED" push (§2.2): every child element of `incoming` is folded
/// into `target` — a tag the target already has is replaced in place, a new
/// tag is appended, a tag present with a null value (no text, no child
/// elements) removes the target's, and a <mosExternalMetadata> block
/// replaces the first existing block with a matching <mosSchema> (else it
/// is appended). Tags `incoming` leaves out stay intact. `keyName` is the
/// identifying child (itemID / objID), which is never touched.
IMOS_CORE_API void mergeElementFields(codec::XmlNode& target, const codec::XmlNode& incoming,
                                      std::string_view keyName);

/// Apply a mosItemReplace (Profile 3, spec §3.3.2) to one RO subtree: locate
/// the story and the item keyed by the message's <itemID>, then merge the
/// incoming <item> — tags present in the message replace the target's in
/// place, new tags append, present-but-empty tags remove theirs, and a
/// <mosExternalMetadata> block replaces the first existing block with a
/// matching <mosSchema> (else appends). The item's position is immutable —
/// a replace never moves it. Transactional: on error nothing mutates.
/// Shared by RunningOrderModel::apply and the NCS authoring layer's device
/// projection.
IMOS_CORE_API Result<void> applyMosItemReplace(codec::XmlNode& runningOrder,
                                               const codec::Message& message);

class IMOS_CORE_API RunningOrderModel {
public:
    /// Apply one inbound construction message: roCreate, roReplace,
    /// roMetadataReplace, roDelete, roElementAction, roReadyToAir,
    /// roElementStat (NCS-originated status, 2.8.5+), roList (wholesale
    /// rebuild, the resync completion path), or mosItemReplace (Profile 3
    /// item-data merge, see applyMosItemReplace). Transactional per message.
    ///
    /// Decisions of record: roCreate of an existing roID is WrongState (use
    /// roReplace); roReplace and roList upsert (they carry complete state, so
    /// nothing is missing that a resync could recover); roMetadataReplace
    /// merges into the RO header per spec §3.4.4 (present tags replace in
    /// place, new tags are added, absent tags are left alone, a
    /// mosExternalMetadata block replaces only one with the same mosSchema)
    /// and leaves the story list untouched.
    Result<void> apply(const codec::Message& message);

    /// Drop one RO without ceremony — the resync NACK branch (the NCS no
    /// longer has the RO, so neither should the shadow). No-op if absent.
    void erase(std::string_view roID);

    // ---- playlist queries ----------------------------------------------

    [[nodiscard]] std::size_t size() const noexcept { return orders_.size(); }
    [[nodiscard]] bool has(std::string_view roID) const;
    [[nodiscard]] std::vector<std::string> runningOrderIDs() const;
    /// The stored RO subtree: the verbatim children of the construction
    /// message that defined it (roID, roSlug, …, <story>*), or nullptr.
    [[nodiscard]] const codec::XmlNode* runningOrder(std::string_view roID) const;
    [[nodiscard]] std::vector<std::string> storyIDs(std::string_view roID) const;
    [[nodiscard]] std::vector<std::string> itemIDs(std::string_view roID,
                                                   std::string_view storyID) const;
    [[nodiscard]] const codec::XmlNode* story(std::string_view roID,
                                              std::string_view storyID) const;
    [[nodiscard]] const codec::XmlNode* item(std::string_view roID, std::string_view storyID,
                                             std::string_view itemID) const;
    /// The last roReadyToAir <roAir> value for this RO ("READY"/"NOT READY"),
    /// or nullopt when none was received (or the RO is unknown).
    [[nodiscard]] std::optional<std::string> readyToAir(std::string_view roID) const;

    // ---- per-element status tracking (feeds roElementStat) --------------

    /// Record a status for an element (host playback state or an
    /// NCS-originated roElementStat). Unused ID levels stay empty: a story
    /// status carries an empty itemID, an RO status carries both empty.
    /// Fails NotFound when the referenced element does not exist.
    Result<void> setStatus(RoElement element, std::string_view roID, std::string_view storyID,
                           std::string_view itemID, std::string_view status);
    /// The last recorded status, or nullopt when none (or the element is
    /// gone — statuses are never reported for elements that no longer exist).
    [[nodiscard]] std::optional<std::string> status(RoElement element, std::string_view roID,
                                                    std::string_view storyID,
                                                    std::string_view itemID) const;

private:
    struct StoredRo {
        codec::XmlNode node;                          ///< verbatim RO subtree
        std::optional<std::string> roAir;             ///< last roReadyToAir value
        std::map<std::string, std::string> statuses;  ///< element key → status
    };

    Result<void> applyElementAction(const codec::Message& message);
    Result<void> applyElementStat(const codec::Message& message);
    Result<void> applyMetadataReplace(const codec::Message& message);

    std::map<std::string, StoredRo> orders_;  ///< ordered: deterministic queries
};

}  // namespace imos::profiles
