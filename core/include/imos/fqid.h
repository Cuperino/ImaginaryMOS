// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Fully Qualified MOS ID (FQID) utilities — Profile 6, MOS Redirection
// (plan §3.2 row 6, milestone M5). Profile 6 adds no messages: it is the
// spec's naming convention
//
//     <family>.<machine>.<location>.<enterprise>.mos
//
// (location and enterprise optional) for <mosID>/<ncsID>, which lets a
// device recognize that an object referenced in a running order lives on a
// different MOS and arrange an out-of-band transfer. This header is the
// pure value layer: parse / compose / classify. The enforcement hooks live
// in profiles/profile6.h; the transfer itself is host territory, outside
// the MOS Protocol by definition.
//
// Decisions of record (the spec text gives only the dotted form and four
// examples; these rules are pinned by the fqid unit tables):
//  - Parsing is structural only: dot-separated labels, all non-empty, final
//    label exactly "mos", 2–4 labels before it. The spec imposes no
//    character or length rules on mosID, so neither does the parser.
//  - The 3-label form <family>.<machine>.<x>.mos reads positionally:
//    x is the location (enterprise stays empty). Parse therefore never
//    yields an enterprise without a location.
//  - Comparison is exact (case-sensitive), consistent with mosID matching
//    everywhere else in the protocol; the spec states no case rule. Hosts
//    wanting case-insensitive matching normalize before configuring.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <imos/core_export.h>
#include <imos/error.h>

namespace imos {

/// Parsed Fully Qualified MOS ID (spec Profile 6 convention). Value type;
/// location/enterprise may be empty (the 2- and 3-label forms). An
/// enterprise without a location is not a parseable shape — parse never
/// produces one, and compose() of one is not round-trippable.
struct IMOS_CORE_API Fqid {
    std::string family;
    std::string machine;
    std::string location;    ///< optional scope label
    std::string enterprise;  ///< optional scope label

    /// Canonical string form: labels in order, empty ones skipped, ".mos"
    /// appended. compose(parseFqid(id)) == id for every valid id.
    [[nodiscard]] std::string compose() const;
};

/// Parse `id` as an FQID. Fails (InvalidArgument) on anything that is not
/// the 2/3/4-label ".mos" shape — including empty labels — with a
/// diagnostic naming the offending id.
IMOS_CORE_API Result<Fqid> parseFqid(std::string_view id);

/// Where an object referenced by mosID lives, relative to this device
/// (the spec's "stored locally, or on another machine" question).
enum class Locality : std::uint8_t {
    Self,            ///< same machine as this device: machine + location
                     ///< match within the same enterprise (family may differ
                     ///< — colocated servers share the machine, so nothing
                     ///< to transfer)
    SameEnterprise,  ///< a different machine (or location — the spec extends
                     ///< transfers across buildings and cities) in the same
                     ///< enterprise scope: reachable intra-enterprise
    Foreign,         ///< valid FQID with a different enterprise scope — the
                     ///< out-of-band transfer case Profile 6 exists for
    NotFqid,         ///< legacy/plain mosID — Profile 6 rules don't apply
};

[[nodiscard]] IMOS_CORE_API std::string_view localityName(Locality locality) noexcept;

/// Classify `objectMosID` relative to `self`. Pure function. Tie-breaks for
/// partially-qualified forms (pinned by the unit tables): labels compare
/// exactly and an absent label only matches an absent label, so an
/// enterprise-less FQID is Foreign to any enterprise-scoped one (and vice
/// versa) — the two installations have no shared scope to transfer within.
[[nodiscard]] IMOS_CORE_API Locality classify(const Fqid& self, std::string_view objectMosID);

/// Overload for a pre-parsed object id.
[[nodiscard]] IMOS_CORE_API Locality classify(const Fqid& self, const Fqid& object) noexcept;

}  // namespace imos
