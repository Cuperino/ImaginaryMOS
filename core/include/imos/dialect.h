// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Dialect profiles (plan §7, milestone M7): what a peer does differently
// from the letter of the spec, as data, not as `if`s scattered through the
// code. A DialectProfile is a set of declarative settings; the library
// applies each where it bites, and the presets name the peers whose
// behaviour has been observed. Each setting's comment says what was seen,
// in which peer and version, and how it was found - so a setting can be
// dropped when the peer is fixed. dialects/README.md keeps the delta per
// dialect.
//
// Presets so far: "baseline" (the spec as written) and "sofie" (Sofie TV
// Automation's mos-connection, observed against 5.0.0 in Atlantic
// Newsroom's interop lab). iNEWS, ENPS and Octopus follow in M7 proper.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imos/core_export.h>

namespace imos {

struct DialectProfile {
    /// The preset's name ("baseline", "sofie"), for configuration and logs.
    std::string name = "baseline";

    /// Send a <storyBody> whose only child is one <p> with an empty <p/>
    /// after it. mos-connection 5.0.0 reads a lone element as an object, not
    /// a list, and its roStorySend parser then finds no body at all: a
    /// one-paragraph script arrives empty, while two paragraphs, or one
    /// <storyItem>, arrive whole. The empty paragraph changes nothing a
    /// reader sees. (Found driving mos-connection as a Profile 4 device.)
    bool padLoneParagraph = false;

    friend bool operator==(const DialectProfile&, const DialectProfile&) = default;
};

/// The spec as written: every setting off.
[[nodiscard]] IMOS_CORE_API DialectProfile baselineDialect();
/// Sofie TV Automation's mos-connection.
[[nodiscard]] IMOS_CORE_API DialectProfile sofieDialect();
/// A preset by name; nullopt for a name there is no preset for.
[[nodiscard]] IMOS_CORE_API std::optional<DialectProfile> dialectNamed(std::string_view name);
/// Every preset's name, baseline first.
[[nodiscard]] IMOS_CORE_API std::vector<std::string> dialectNames();

}  // namespace imos
