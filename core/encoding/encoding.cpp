// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/encoding/encoding.h>

#include <string>

namespace imos::encoding {

namespace {

constexpr char32_t kReplacementChar = 0xFFFD;
constexpr char32_t kBom = 0xFEFF;
constexpr char32_t kSwappedBom = 0xFFFE;

constexpr bool isHighSurrogate(char32_t unit) noexcept {
    return unit >= 0xD800 && unit <= 0xDBFF;
}

constexpr bool isLowSurrogate(char32_t unit) noexcept {
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

void appendUtf8(std::string& out, char32_t codePoint) {
    if (codePoint < 0x80) {
        out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        out += static_cast<char>(0xC0 | (codePoint >> 6));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codePoint >> 12));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codePoint >> 18));
        out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

/// Decode one UTF-8 code point at `pos`; returns npos-style failure via
/// `ok=false`. On success advances pos.
char32_t nextUtf8(std::string_view utf8, std::size_t& pos, bool& ok) noexcept {
    ok = true;
    const auto byte = static_cast<unsigned char>(utf8[pos]);
    if (byte < 0x80) {
        ++pos;
        return byte;
    }
    int length = 0;
    char32_t codePoint = 0;
    char32_t minimum = 0;
    if ((byte & 0xE0) == 0xC0) {
        length = 2;
        codePoint = byte & 0x1FU;
        minimum = 0x80;
    } else if ((byte & 0xF0) == 0xE0) {
        length = 3;
        codePoint = byte & 0x0FU;
        minimum = 0x800;
    } else if ((byte & 0xF8) == 0xF0) {
        length = 4;
        codePoint = byte & 0x07U;
        minimum = 0x10000;
    } else {
        ok = false;
        return 0;
    }
    if (pos + static_cast<std::size_t>(length) > utf8.size()) {
        ok = false;
        return 0;
    }
    for (std::size_t i = 1; i < static_cast<std::size_t>(length); ++i) {
        const auto continuation = static_cast<unsigned char>(utf8[pos + i]);
        if ((continuation & 0xC0) != 0x80) {
            ok = false;
            return 0;
        }
        codePoint = (codePoint << 6) | (continuation & 0x3FU);
    }
    if (codePoint < minimum || codePoint > 0x10FFFF ||
        (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
        ok = false;
        return 0;
    }
    pos += static_cast<std::size_t>(length);
    return codePoint;
}

void appendUnit(std::vector<std::uint8_t>& out, char32_t unit, bool bigEndian) {
    if (bigEndian) {
        out.push_back(static_cast<std::uint8_t>(unit >> 8));
        out.push_back(static_cast<std::uint8_t>(unit & 0xFF));
    } else {
        out.push_back(static_cast<std::uint8_t>(unit & 0xFF));
        out.push_back(static_cast<std::uint8_t>(unit >> 8));
    }
}

}  // namespace

std::string_view wireEncodingName(WireEncoding encoding) noexcept {
    switch (encoding) {
        case WireEncoding::Ucs2Be:
            return "UCS-2 BE";
        case WireEncoding::Ucs2Le:
            return "UCS-2 LE";
        case WireEncoding::Utf8:
            return "UTF-8";
    }
    return "";
}

std::size_t findInvalidUtf8(std::string_view utf8) noexcept {
    std::size_t pos = 0;
    while (pos < utf8.size()) {
        const std::size_t at = pos;
        bool ok = false;
        (void)nextUtf8(utf8, pos, ok);
        if (!ok) {
            return at;
        }
    }
    return std::string_view::npos;
}

Result<std::string> decodeToUtf8(std::span<const std::uint8_t> wire, const DecodeOptions& options,
                                 DecodeStats* stats) {
    DecodeStats localStats;
    DecodeStats& tally = stats != nullptr ? *stats : localStats;
    tally = DecodeStats{};

    if (options.encoding == WireEncoding::Utf8) {
        std::string_view text{reinterpret_cast<const char*>(wire.data()), wire.size()};
        if (options.acceptBom && text.starts_with("\xEF\xBB\xBF")) {
            text.remove_prefix(3);
            tally.sawBom = true;
        }
        const std::size_t bad = findInvalidUtf8(text);
        if (bad != std::string_view::npos) {
            return makeError(ErrorCode::EncodingError,
                             "invalid UTF-8 at byte " + std::to_string(bad) +
                                 " — peer configured as UTF-8; check whether it actually "
                                 "sends UCS-2");
        }
        return std::string{text};
    }

    if (wire.size() % 2 != 0) {
        return makeError(ErrorCode::EncodingError,
                         "odd byte count (" + std::to_string(wire.size()) + ") for " +
                             std::string{wireEncodingName(options.encoding)} +
                             " — peer encoding is likely misconfigured");
    }
    const bool bigEndian = options.encoding == WireEncoding::Ucs2Be;
    std::string out;
    out.reserve(wire.size() / 2);
    for (std::size_t i = 0; i + 2 <= wire.size(); i += 2) {
        const char32_t unit =
            bigEndian ? static_cast<char32_t>((static_cast<char32_t>(wire[i]) << 8) | wire[i + 1])
                      : static_cast<char32_t>((static_cast<char32_t>(wire[i + 1]) << 8) | wire[i]);
        if (i == 0 && (unit == kBom || unit == kSwappedBom)) {
            if (unit == kSwappedBom) {
                return makeError(ErrorCode::EncodingError,
                                 "byte-order mark contradicts the configured " +
                                     std::string{wireEncodingName(options.encoding)} +
                                     " byte order — the peer is sending the opposite endianness");
            }
            if (options.acceptBom) {
                tally.sawBom = true;
                continue;
            }
        }
        if (isHighSurrogate(unit)) {
            if (options.acceptSurrogatePairs && i + 4 <= wire.size()) {
                const char32_t low =
                    bigEndian ? static_cast<char32_t>((static_cast<char32_t>(wire[i + 2]) << 8) |
                                                      wire[i + 3])
                              : static_cast<char32_t>((static_cast<char32_t>(wire[i + 3]) << 8) |
                                                      wire[i + 2]);
                if (isLowSurrogate(low)) {
                    appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                    ++tally.surrogatePairs;
                    i += 2;
                    continue;
                }
            }
            return makeError(ErrorCode::EncodingError,
                             "lone high surrogate at byte " + std::to_string(i));
        }
        if (isLowSurrogate(unit)) {
            return makeError(ErrorCode::EncodingError,
                             "lone low surrogate at byte " + std::to_string(i));
        }
        appendUtf8(out, unit);
    }
    return out;
}

Result<std::vector<std::uint8_t>> encodeFromUtf8(std::string_view utf8,
                                                 const EncodeOptions& options, EncodeStats* stats) {
    EncodeStats localStats;
    EncodeStats& tally = stats != nullptr ? *stats : localStats;
    tally = EncodeStats{};

    if (options.encoding == WireEncoding::Utf8) {
        const std::size_t bad = findInvalidUtf8(utf8);
        if (bad != std::string_view::npos) {
            return makeError(ErrorCode::EncodingError,
                             "invalid UTF-8 input at byte " + std::to_string(bad));
        }
        std::vector<std::uint8_t> out;
        out.reserve(utf8.size() + (options.writeBom ? 3 : 0));
        if (options.writeBom) {
            out.insert(out.end(), {0xEF, 0xBB, 0xBF});
        }
        out.insert(out.end(), utf8.begin(), utf8.end());
        return out;
    }

    const bool bigEndian = options.encoding == WireEncoding::Ucs2Be;
    std::vector<std::uint8_t> out;
    out.reserve(utf8.size() * 2 + 2);
    if (options.writeBom) {
        appendUnit(out, kBom, bigEndian);
    }
    std::size_t pos = 0;
    while (pos < utf8.size()) {
        const std::size_t at = pos;
        bool ok = false;
        const char32_t codePoint = nextUtf8(utf8, pos, ok);
        if (!ok) {
            return makeError(ErrorCode::EncodingError,
                             "invalid UTF-8 input at byte " + std::to_string(at));
        }
        if (codePoint > 0xFFFF) {
            switch (options.nonBmp) {
                case NonBmpPolicy::Replace:
                    appendUnit(out, kReplacementChar, bigEndian);
                    ++tally.replacedNonBmp;
                    break;
                case NonBmpPolicy::Reject:
                    return makeError(ErrorCode::EncodingError,
                                     "code point at byte " + std::to_string(at) +
                                         " is outside the BMP and cannot be represented "
                                         "in UCS-2 (policy: reject)");
                case NonBmpPolicy::SurrogatePair: {
                    const char32_t offset = codePoint - 0x10000;
                    appendUnit(out, 0xD800 + (offset >> 10), bigEndian);
                    appendUnit(out, 0xDC00 + (offset & 0x3FF), bigEndian);
                    break;
                }
            }
            continue;
        }
        appendUnit(out, codePoint, bigEndian);
    }
    return out;
}

}  // namespace imos::encoding
