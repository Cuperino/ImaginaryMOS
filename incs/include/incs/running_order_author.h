// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// incs::RunningOrderAuthor — the NCS-side running-order authoring and diff
// API (plan §4.6, Profile 2 round of M3). The host edits a *desired* state
// (create ROs, insert/replace/move/swap/delete stories and items); commit()
// diffs desired against the device's projected state and enqueues the
// minimal spec-correct message sequence — roCreate for a new RO,
// roMetadataReplace for header-only changes, batched roElementAction
// INSERT/REPLACE/MOVE/SWAP/DELETE for content and order changes (MOVE is
// always preferred over DELETE+INSERT, mandatory for story-send-linked ROs
// per plan §3.2), roDelete for destruction, and a wholesale roReplace only
// when the delta exceeds a configurable threshold.
//
// The desired state lives in a RunningOrderModel of its own, so edits share
// the exact mutation and validation semantics the device applies — an edit
// the device would NACK is rejected at authoring time. The author also
// implements IRunningOrderSource: a device resyncing via roReq is answered
// with the desired state, which then becomes the device's projected state.

#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <imos/codec/message.h>
#include <imos/error.h>
#include <imos/profiles/profile2.h>
#include <imos/profiles/running_order_model.h>
#include <imos/session/peer_session.h>
#include <incs/incs_export.h>

namespace incs {

/// The resolution of one committed message: its roAck (ok or NACK with the
/// per-element statuses), or a delivery failure after retries lapsed.
struct CommitEvent {
    std::string roID;
    imos::Seq seq = 0;
    bool ok = false;
    bool deliveryFailed = false;  ///< retries exhausted; no roAck at all
    std::string roStatus;         ///< the roAck's <roStatus> text
    std::vector<imos::profiles::ElementStatus> statuses;
};

class IMOS_INCS_API RunningOrderAuthor final : public imos::profiles::IRunningOrderSource {
public:
    struct Config {
        /// Diff size (messages per RO) beyond which commit() sends one
        /// wholesale roReplace instead of the incremental sequence.
        std::size_t replaceThreshold = 6;
    };

    explicit RunningOrderAuthor(imos::PeerSession* session);
    RunningOrderAuthor(imos::PeerSession* session, Config config);

    // ---- desired-state editing (validated like the device would) --------

    imos::Result<void> create(std::string_view roID, std::string_view roSlug);
    imos::Result<void> destroy(std::string_view roID);  ///< roDelete on commit
    /// Set one RO header field (roSlug, roEdStart, roEdDur, roChannel, …) in
    /// its content-model position. Never a story — use the story edits.
    imos::Result<void> setHeaderField(std::string_view roID, std::string_view field,
                                      std::string_view value);
    /// Request roReadyToAir with this <roAir> value ("READY" / "NOT READY").
    imos::Result<void> setReadyToAir(std::string_view roID, std::string_view roAir);

    /// `story` is a <story> subtree (see makeStory); an empty beforeStoryID
    /// appends at the tail.
    imos::Result<void> insertStory(std::string_view roID, const imos::codec::XmlNode& story,
                                   std::string_view beforeStoryID = {});
    imos::Result<void> replaceStory(std::string_view roID, std::string_view storyID,
                                    const imos::codec::XmlNode& story);
    imos::Result<void> removeStory(std::string_view roID, std::string_view storyID);
    imos::Result<void> moveStory(std::string_view roID, std::string_view storyID,
                                 std::string_view beforeStoryID = {});
    imos::Result<void> swapStories(std::string_view roID, std::string_view storyA,
                                   std::string_view storyB);

    imos::Result<void> insertItem(std::string_view roID, std::string_view storyID,
                                  const imos::codec::XmlNode& item,
                                  std::string_view beforeItemID = {});
    imos::Result<void> replaceItem(std::string_view roID, std::string_view storyID,
                                   std::string_view itemID, const imos::codec::XmlNode& item);
    imos::Result<void> removeItem(std::string_view roID, std::string_view storyID,
                                  std::string_view itemID);
    imos::Result<void> moveItem(std::string_view roID, std::string_view storyID,
                                std::string_view itemID, std::string_view beforeItemID = {});
    imos::Result<void> swapItems(std::string_view roID, std::string_view storyID,
                                 std::string_view itemA, std::string_view itemB);

    /// Convenience subtree builders for hosts and tests.
    [[nodiscard]] static imos::codec::XmlNode makeStory(std::string_view storyID,
                                                        std::string_view storySlug = {},
                                                        std::string_view storyNum = {});
    [[nodiscard]] static imos::codec::XmlNode makeItem(std::string_view itemID,
                                                       std::string_view objID,
                                                       std::string_view mosID,
                                                       std::string_view itemSlug = {});

    // ---- queries ---------------------------------------------------------

    /// The desired state (shared RunningOrderModel semantics and queries).
    [[nodiscard]] const imos::profiles::RunningOrderModel& desired() const noexcept {
        return desired_;
    }
    /// Messages committed but not yet resolved by a roAck.
    [[nodiscard]] std::size_t pendingCount() const noexcept { return pending_.size(); }

    // ---- commit / session plumbing ---------------------------------------

    /// Diff desired vs the device projection for one RO and enqueue the
    /// resulting messages in order on the ro channel. Returns their seqs
    /// (empty when nothing changed). In-flight-of-one serializes delivery;
    /// commits are never coalesced — a commit while previous messages are
    /// unacked diffs against the projection those messages will produce.
    imos::Result<std::vector<imos::Seq>> commit(std::string_view roID);
    /// commit() for every RO the author or the device knows.
    imos::Result<std::vector<imos::Seq>> commitAll();

    /// Consume one session event; returns true when it resolved a committed
    /// message (queues a CommitEvent). A NACK marks the device projection
    /// unknown, so the next commit() falls back to a wholesale roReplace.
    bool handle(const imos::SessionEvent& event);
    [[nodiscard]] std::optional<CommitEvent> nextEvent();

    /// IRunningOrderSource: answer a resyncing device from the desired
    /// state; the snapshot becomes the device's projected state.
    std::optional<imos::codec::Message> runningOrder(std::string_view roID) override;

private:
    /// What the device is believed to hold for one RO.
    struct DeviceRo {
        imos::codec::XmlNode node;  ///< projected RO subtree
        bool valid = true;          ///< false after a NACK → full roReplace
        std::optional<std::string> roAir;
    };

    imos::Result<void> applyEdit(const imos::codec::Message& message);

    imos::PeerSession* session_;
    Config config_;
    imos::profiles::RunningOrderModel desired_;
    std::unordered_map<std::string, DeviceRo> device_;
    std::unordered_map<imos::Seq, std::string> pending_;  ///< seq → roID
    std::deque<CommitEvent> events_;
};

}  // namespace incs
