// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Profile 6 — MOS Redirection (plan §3.2 row 6, milestone M5). No new
// messages and no wire behavior: acks, sequencing, and codec are untouched.
// This module is the enforcement layer over the FQID value utilities
// (imos/fqid.h) — an opt-in classifier the Profile 2/4 ingest paths consult
// so that item references naming a *different* MOS surface to the host,
// which then arranges the out-of-band transfer the spec deliberately leaves
// outside the protocol.
//
// Defaults are all-off: with an unconfigured Profile6Config the classifier
// never activates and no behavior changes for non-claiming devices.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imos/codec/message.h>
#include <imos/core_export.h>
#include <imos/error.h>
#include <imos/fqid.h>
#include <imos/session/peer_session.h>

namespace imos::profiles {

/// The Profile 6 enforcement toggles, surfaced on imos::Device (classifying
/// against its mosID) and mirrored on incs::Ncs (identity check on its
/// ncsID; authoring-time validation against the peer device's mosID).
struct Profile6Config {
    /// Claiming Profile 6 requires the device's own ID to be an FQID; with
    /// this set, a non-FQID self ID fails bring-up validation (caught at
    /// configuration time, not on air).
    bool requireFqidSelf = false;
    /// Classify item references in ingested RO content and post a
    /// ForeignObjectReferenced session event for each Foreign one — the
    /// seam a host uses to start its out-of-band transfer.
    bool flagForeignReferences = false;
    /// NACK ingested messages that reference Foreign objects instead of
    /// accepting them (composes with flagForeignReferences).
    bool rejectForeignReferences = false;

    [[nodiscard]] bool enabled() const noexcept {
        return requireFqidSelf || flagForeignReferences || rejectForeignReferences;
    }
};

/// Every toggle needs a parseable self identity — classification is
/// relative to it. Ok when the config is all-off; otherwise `selfID` must
/// parse as an FQID, with the parser's diagnostic surfaced on failure.
[[nodiscard]] IMOS_CORE_API Result<void> validateProfile6Identity(const Profile6Config& config,
                                                                  std::string_view selfID);

/// One classified item reference lifted from an ingested message. IDs are
/// copied out of the message (the reference outlives the callback).
struct ClassifiedReference {
    std::string storyID;
    std::string itemID;
    std::string objID;
    std::string mosID;
    Locality locality = Locality::NotFqid;
};

/// The armed form of a Profile6Config: config + parsed self FQID. Value
/// type owned by the role facade and consulted (by pointer) from the
/// Profile 2/4 responders. Default-constructed it is inert.
class IMOS_CORE_API Profile6Classifier {
public:
    Profile6Classifier() = default;

    /// Validate and arm. Fails exactly when validateProfile6Identity does;
    /// on failure the classifier stays inert.
    Result<void> configure(const Profile6Config& config, std::string_view selfID);

    [[nodiscard]] const Profile6Config& config() const noexcept { return config_; }
    /// True when ingest paths should classify (flag or reject armed).
    [[nodiscard]] bool classifying() const noexcept {
        return self_.has_value() &&
               (config_.flagForeignReferences || config_.rejectForeignReferences);
    }
    [[nodiscard]] bool rejecting() const noexcept {
        return self_.has_value() && config_.rejectForeignReferences;
    }

    /// Classify one mosID relative to the armed self (NotFqid when inert).
    [[nodiscard]] Locality classify(std::string_view mosID) const;

    /// Lift and classify the item references of one ingested message:
    /// roCreate/roReplace story items, roElementAction inserted/replaced
    /// content, and roStorySend <storyItem>s. Agency payloads (Read1stMEMasBody)
    /// stay opaque — mosPayload is never mined for references. Empty for
    /// every other message type or when not classifying.
    [[nodiscard]] std::vector<ClassifiedReference> collectReferences(
        const codec::Message& message) const;

    /// The first Foreign reference, or nullptr — the reject path's NACK
    /// wants exactly one diagnostic.
    [[nodiscard]] static const ClassifiedReference* firstForeign(
        const std::vector<ClassifiedReference>& references);

    /// Post one ForeignObjectReferenced session event per Foreign reference
    /// (no-op unless flagForeignReferences is armed). Called by the ingest
    /// paths after content is accepted.
    void announceForeign(PeerSession& session, Channel channel, std::string_view roID,
                         const std::vector<ClassifiedReference>& references) const;

private:
    Profile6Config config_;
    std::optional<Fqid> self_;
};

}  // namespace imos::profiles
