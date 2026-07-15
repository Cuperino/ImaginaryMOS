// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/framing/deframer.h>

#include <cstddef>
#include <string_view>

namespace imos::framing {

namespace {

constexpr std::string_view kStartPattern = "<mos";
constexpr std::string_view kEndPattern = "</mos";
constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

constexpr bool isWhitespaceChar(int c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// A unit that may legally follow the element name in "<mos…" / "</mos…":
/// whitespace, '>', or '/' (empty-element form). Anything else means we
/// matched a longer name such as <mosID> or </mosAck>.
constexpr bool isNameBoundary(int c) noexcept {
    return c == '>' || c == '/' || isWhitespaceChar(c);
}

}  // namespace

StreamDeframer::StreamDeframer(DeframerOptions options) : options_(options) {}

void StreamDeframer::feed(std::span<const std::uint8_t> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

std::size_t StreamDeframer::unitSize() const noexcept {
    return options_.encoding == encoding::WireEncoding::Utf8 ? 1 : 2;
}

int StreamDeframer::unitAt(std::size_t byteIndex) const noexcept {
    if (byteIndex + unitSize() > buffer_.size()) {
        return -1;
    }
    switch (options_.encoding) {
        case encoding::WireEncoding::Utf8:
            return buffer_[byteIndex] < 0x80 ? buffer_[byteIndex] : -1;
        case encoding::WireEncoding::Ucs2Be:
            return buffer_[byteIndex] == 0 ? buffer_[byteIndex + 1] : -1;
        case encoding::WireEncoding::Ucs2Le:
            return buffer_[byteIndex + 1] == 0 ? buffer_[byteIndex] : -1;
    }
    return -1;
}

std::size_t StreamDeframer::findPattern(std::size_t from, std::string_view ascii,
                                        bool requireNameBoundary) const noexcept {
    const std::size_t unit = unitSize();
    const std::size_t patternBytes = ascii.size() * unit;
    if (buffer_.size() < patternBytes) {
        return kNpos;
    }
    // Byte-granular scan (not unit-granular): odd-length garbage must not be
    // able to wedge UCS-2 alignment (plan §5.2 resynchronizing scanner).
    for (std::size_t at = from; at + patternBytes <= buffer_.size(); ++at) {
        bool match = true;
        for (std::size_t i = 0; i < ascii.size(); ++i) {
            if (unitAt(at + i * unit) != ascii[i]) {
                match = false;
                break;
            }
        }
        if (!match) {
            continue;
        }
        if (requireNameBoundary) {
            const int following = unitAt(at + patternBytes);
            if (following < 0 && at + patternBytes + unit > buffer_.size()) {
                return kNpos;  // boundary unit not received yet — wait
            }
            if (!isNameBoundary(following)) {
                continue;
            }
        }
        return at;
    }
    return kNpos;
}

void StreamDeframer::dropBytes(std::size_t count) noexcept {
    buffer_.erase(buffer_.begin(),
                  buffer_.begin() +
                      static_cast<std::ptrdiff_t>(count < buffer_.size() ? count : buffer_.size()));
}

std::optional<Frame> StreamDeframer::next() {
    const std::size_t unit = unitSize();
    std::size_t discarded = 0;

    // Inter-message whitespace is legal on a 2.8 stream; consume it quietly.
    while (isWhitespaceChar(unitAt(0))) {
        dropBytes(unit);
    }

    for (;;) {
        const std::size_t start = findPattern(0, kStartPattern, true);
        if (start == kNpos) {
            // No start in sight: keep only a tail that could still complete a
            // split "<mos" match; everything before it is garbage.
            const std::size_t keep = (kStartPattern.size() + 1) * unit;
            if (buffer_.size() > keep) {
                const std::size_t drop = buffer_.size() - keep;
                dropBytes(drop);
                discarded += drop;
                totalDiscarded_ += drop;
            }
            return std::nullopt;
        }
        if (start > 0) {
            dropBytes(start);
            discarded += start;
            totalDiscarded_ += start;
        }

        // Locate "</mos" then its terminating '>', skipping optional
        // whitespace inside the close tag.
        std::size_t searchFrom = kStartPattern.size() * unit;
        std::size_t frameEnd = kNpos;
        for (;;) {
            const std::size_t end = findPattern(searchFrom, kEndPattern, true);
            if (end == kNpos) {
                break;
            }
            std::size_t cursor = end + kEndPattern.size() * unit;
            while (isWhitespaceChar(unitAt(cursor))) {
                cursor += unit;
            }
            const int closing = unitAt(cursor);
            if (closing == '>') {
                frameEnd = cursor + unit;
                break;
            }
            if (cursor + unit > buffer_.size()) {
                break;  // tag still incomplete — wait for more bytes
            }
            searchFrom = end + unit;  // false positive; keep scanning
        }

        if (frameEnd == kNpos) {
            if (buffer_.size() > options_.maxMessageBytes) {
                // Over-cap candidate: drop its start pattern and resync.
                ++overflowCount_;
                const std::size_t drop = kStartPattern.size() * unit;
                dropBytes(drop);
                discarded += drop;
                totalDiscarded_ += drop;
                continue;
            }
            return std::nullopt;
        }
        if (frameEnd > options_.maxMessageBytes) {
            ++overflowCount_;
            totalDiscarded_ += frameEnd;
            discarded += frameEnd;
            dropBytes(frameEnd);
            continue;
        }

        Frame frame;
        frame.document.assign(buffer_.begin(),
                              buffer_.begin() + static_cast<std::ptrdiff_t>(frameEnd));
        frame.discardedBefore = discarded;
        dropBytes(frameEnd);
        return frame;
    }
}

}  // namespace imos::framing
