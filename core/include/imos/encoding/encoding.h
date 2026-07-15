// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Wire-encoding engine (plan §3.4). The spec mandates ISO 10646 UCS-2 big
// endian for message content; UTF-8 is a per-peer option for peers that
// adopt the web-native reading, and little-endian UCS-2 variants have been
// observed in the 2.8 field. Internally everything is UTF-8; this module
// converts at the wire boundary without pulling in ICU. The receive side
// expects the configured encoding rather than sniffing — mismatches surface
// as errors whose diagnostics name the likely misconfiguration.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <imos/core_export.h>
#include <imos/error.h>

namespace imos::encoding {

enum class WireEncoding : std::uint8_t { Ucs2Be, Ucs2Le, Utf8 };

[[nodiscard]] IMOS_CORE_API std::string_view wireEncodingName(WireEncoding encoding) noexcept;

/// UCS-2 cannot represent characters outside the Basic Multilingual Plane
/// (emoji in slugs will happen — plan R-4). Default: replace with U+FFFD and
/// report; Reject fails the encode; SurrogatePair emits UTF-16 pairs for
/// dialects known to accept them.
enum class NonBmpPolicy : std::uint8_t { Replace, Reject, SurrogatePair };

struct EncodeOptions {
    WireEncoding encoding = WireEncoding::Ucs2Be;
    NonBmpPolicy nonBmp = NonBmpPolicy::Replace;
    bool writeBom = false;  ///< spec messages carry no BOM; dialect option
};

struct EncodeStats {
    std::size_t replacedNonBmp = 0;  ///< code points replaced with U+FFFD
};

struct DecodeOptions {
    WireEncoding encoding = WireEncoding::Ucs2Be;
    /// Strip a leading BOM matching the configured byte order. A BOM of the
    /// *opposite* order is always an error naming the misconfiguration.
    bool acceptBom = true;
    /// Postel leniency: some field implementations emit UTF-16 rather than
    /// strict UCS-2; well-formed surrogate pairs decode (and are counted)
    /// instead of failing. Lone surrogates always fail.
    bool acceptSurrogatePairs = true;
};

struct DecodeStats {
    bool sawBom = false;
    std::size_t surrogatePairs = 0;
};

/// Decode wire bytes to UTF-8 per the configured encoding.
IMOS_CORE_API Result<std::string> decodeToUtf8(std::span<const std::uint8_t> wire,
                                               const DecodeOptions& options = {},
                                               DecodeStats* stats = nullptr);

/// Encode UTF-8 text to wire bytes. Invalid UTF-8 input is an error (the
/// internal representation is trusted, but boundaries verify).
IMOS_CORE_API Result<std::vector<std::uint8_t>> encodeFromUtf8(std::string_view utf8,
                                                               const EncodeOptions& options = {},
                                                               EncodeStats* stats = nullptr);

/// Validate UTF-8 (rejects overlong forms, surrogates, and > U+10FFFF).
/// Returns the byte offset of the first invalid sequence, or npos if valid.
[[nodiscard]] IMOS_CORE_API std::size_t findInvalidUtf8(std::string_view utf8) noexcept;

}  // namespace imos::encoding
