// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Hand-written storyBody helpers (Profile 4, plan §4.5). <storyBody> is the
// one genuinely irregular content model of the vocabulary — <p> is mixed
// content (character runs interleaved with inline <em>/<tab>/<pi>/… markup),
// which the generated views cannot express — so this small layer is written
// by hand over the retained XmlNode tree. It is strictly read-only: runs
// point into the message's tree, nothing is copied or re-normalized, and the
// tree still re-serializes byte-cleanly after iteration.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <imos/codec/xml.h>
#include <imos/core_export.h>
#include <imos/error.h>

namespace imos::codec {

class Message;

/// Document-order iteration of a <p> element's mixed content. Every child
/// of the paragraph appears as exactly one run — character data as Text,
/// the spec's inline vocabulary as Em/Pi/Tab, and anything else (including
/// the DTD's pkg/b/i/u and vendor tags, comments, and processing
/// instructions) as Raw — so walking the runs loses nothing: the backing
/// nodes re-serialize verbatim.
class IMOS_CORE_API ParagraphRuns {
public:
    enum class Kind : std::uint8_t {
        Text,  ///< a character-data child (whitespace is preserved, not trimmed)
        Em,    ///< <em> emphasis
        Pi,    ///< <pi> presenter instruction
        Tab,   ///< <tab> tabular marker
        Raw,   ///< any other child; re-serialize via `node` for the verbatim form
    };

    struct Run {
        Kind kind = Kind::Text;
        /// Character data of the run: the text itself for Text, the element's
        /// immediate character content for Em/Pi/Tab, and empty for Raw
        /// (opaque content is read through `node`, never flattened here).
        std::string text;
        const XmlNode* node = nullptr;  ///< the backing child, never null
    };

    /// `p` must outlive the runs; they point into its children.
    explicit ParagraphRuns(const XmlNode& p);

    [[nodiscard]] std::vector<Run>::const_iterator begin() const { return runs_.begin(); }
    [[nodiscard]] std::vector<Run>::const_iterator end() const { return runs_.end(); }
    [[nodiscard]] std::size_t size() const noexcept { return runs_.size(); }
    [[nodiscard]] bool empty() const noexcept { return runs_.empty(); }
    [[nodiscard]] const Run& operator[](std::size_t i) const { return runs_[i]; }

private:
    std::vector<Run> runs_;
};

/// A roStorySend body after Read1stMEMasBody resolution (spec §3.5.5): either
/// the literal <storyBody> paragraphs, or — when the storyBody carries
/// Read1stMEMasBody="true" — the first <mosExternalMetadata> block of the
/// roStorySend, whose mosPayload substitutes for the body (agency copy:
/// NewsML, IPTC, …). Both pointers reference the message's own tree; the
/// payload is never a re-serialized copy, preserving the codec's
/// opaque-payload guarantee end to end.
struct ResolvedStoryBody {
    const XmlNode* storyBody = nullptr;       ///< always the <storyBody> element
    const XmlNode* agencyMetadata = nullptr;  ///< first MEM when the attribute is true
    /// True when the body is the agency payload (read it from agencyMetadata's
    /// <mosPayload>, not from storyBody's paragraphs).
    [[nodiscard]] bool bodyIsAgencyPayload() const noexcept { return agencyMetadata != nullptr; }
};

/// Resolve the body of a <roStorySend> element (a message body node, e.g.
/// Message::body()). Fails when the storyBody is missing or the attribute
/// promises an agency payload that is not there.
IMOS_CORE_API Result<ResolvedStoryBody> resolveStoryBody(const XmlNode& roStorySend);

/// Convenience overload over a whole roStorySend message.
IMOS_CORE_API Result<ResolvedStoryBody> resolveStoryBody(const Message& message);

}  // namespace imos::codec
