// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Fuzz surface 2 (plan §10): the 2.8 stream deframer. The first input byte
// picks the encoding and chunking pattern; the rest is the hostile stream.
// Invariants: no crash, bounded buffering, emitted frames respect the cap.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include <imos/framing/deframer.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::uint8_t selector = data[0];
    data += 1;
    size -= 1;

    imos::framing::DeframerOptions options;
    switch (selector % 3) {
        case 0:
            options.encoding = imos::encoding::WireEncoding::Ucs2Be;
            break;
        case 1:
            options.encoding = imos::encoding::WireEncoding::Ucs2Le;
            break;
        default:
            options.encoding = imos::encoding::WireEncoding::Utf8;
            break;
    }
    options.maxMessageBytes = 4096;
    imos::framing::StreamDeframer deframer{options};

    const std::size_t chunk = 1 + (selector >> 2);
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t len = chunk < size - offset ? chunk : size - offset;
        deframer.feed(std::span<const std::uint8_t>{data + offset, len});
        offset += len;
        while (const auto frame = deframer.next()) {
            if (frame->document.size() > options.maxMessageBytes) {
                std::abort();  // cap must hold for emitted frames
            }
        }
    }
    // The buffer must stay bounded: at most cap + a small resync tail.
    if (deframer.bufferedBytes() > options.maxMessageBytes + 64) {
        std::abort();
    }
    return 0;
}
