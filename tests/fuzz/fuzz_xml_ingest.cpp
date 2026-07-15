// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Fuzz surface 1 (plan §10): XML message ingest. Seeded from the fixture
// corpus. Also asserts the serializer fixed-point property: once parsed and
// re-serialized, a second parse/serialize round must be byte-identical.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

#include <imos/codec/legacy.h>
#include <imos/codec/message.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input{reinterpret_cast<const char*>(data), size};
    imos::codec::XmlParseLimits limits;
    limits.maxDocumentBytes = 1U << 20;
    limits.maxDepth = 64;

    imos::Result<imos::codec::Message> parsed = imos::codec::parseMessage(input, limits);
    if (!parsed.ok()) {
        return 0;
    }
    const std::string once = imos::codec::serializeMessage(parsed.value());
    imos::Result<imos::codec::Message> reparsed = imos::codec::parseMessage(once, limits);
    if (!reparsed.ok()) {
        std::abort();  // whatever we serialize must parse
    }
    if (imos::codec::serializeMessage(reparsed.value()) != once) {
        std::abort();  // serialization must be a fixed point
    }
    // Exercise the legacy path when applicable; the result only has to be
    // well-formed, not semantically meaningful for fuzz inputs.
    if (parsed.value().info().legacyRemoved) {
        (void)imos::codec::normalizeLegacy(parsed.value());
    }
    return 0;
}
