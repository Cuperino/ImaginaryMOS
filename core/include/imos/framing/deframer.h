// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// MOS 2.8 stream deframer (M1 spike, plan §5.2/§13). The 2.8 TCP transport
// has no length framing: messages are delimited by the XML structure itself,
// so the receiver tokenizes the byte stream into complete <mos>…</mos>
// documents with a resynchronizing scanner and a hard per-message size cap
// enforced before any XML work begins (plan §12). The scanner works on the
// configured wire encoding directly (byte-pattern matching at any offset, so
// odd-length garbage cannot wedge UCS-2 alignment).
//
// Known spike limitation, acceptable for MOS traffic: a literal "</mos>"
// inside a CDATA section or comment would end a frame early; the resulting
// parse failure surfaces as a protocol error upstream.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <imos/core_export.h>
#include <imos/encoding/encoding.h>

namespace imos::framing {

struct DeframerOptions {
    encoding::WireEncoding encoding = encoding::WireEncoding::Ucs2Be;
    /// Hard cap on one message's wire size; larger candidates are discarded
    /// and scanning resynchronizes on the next "<mos" start.
    std::size_t maxMessageBytes = 4U * 1024U * 1024U * 2U;  // wire bytes (UCS-2 doubles)
};

/// One complete <mos> document in wire encoding, plus how many garbage bytes
/// were discarded while scanning for it (0 on a clean stream).
struct Frame {
    std::vector<std::uint8_t> document;
    std::size_t discardedBefore = 0;
};

class IMOS_CORE_API StreamDeframer {
public:
    explicit StreamDeframer(DeframerOptions options = {});

    /// Append received bytes (any split; the scanner keeps its own state).
    void feed(std::span<const std::uint8_t> bytes);

    /// The next complete document, or nullopt until more bytes arrive.
    [[nodiscard]] std::optional<Frame> next();

    [[nodiscard]] std::size_t bufferedBytes() const noexcept { return buffer_.size(); }
    /// Cumulative garbage dropped (resyncs + over-cap discards).
    [[nodiscard]] std::size_t totalDiscarded() const noexcept { return totalDiscarded_; }
    /// Number of over-cap candidates discarded so far.
    [[nodiscard]] std::size_t overflowCount() const noexcept { return overflowCount_; }

private:
    [[nodiscard]] std::size_t unitSize() const noexcept;
    /// The ASCII character encoded at `byteIndex`, or -1 for non-ASCII,
    /// wrong-endian, or out-of-range units.
    [[nodiscard]] int unitAt(std::size_t byteIndex) const noexcept;
    [[nodiscard]] std::size_t findPattern(std::size_t from, std::string_view ascii,
                                          bool requireNameBoundary) const noexcept;
    void dropBytes(std::size_t count) noexcept;

    DeframerOptions options_;
    std::vector<std::uint8_t> buffer_;
    std::size_t totalDiscarded_ = 0;
    std::size_t overflowCount_ = 0;
};

}  // namespace imos::framing
