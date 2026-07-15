// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Fuzz surface 4 (plan §10): the MOS 4.0 connection-URL/parameter parser —
// the first thing hostile bytes reach on a server upgrade. Invariants: no
// crash on any input; accepted parses re-compose and re-parse to the same
// MOS parameters (extras ride verbatim); rejected inputs cost nothing more
// than the size cap.

#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <imos/ws40/url.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view uri{reinterpret_cast<const char*>(data), size};
    const auto parsed = imos::ws40::parseConnectionParams(uri);
    if (!parsed.ok()) {
        return 0;
    }
    // Round-trip stability for everything the parser accepted.
    const std::string url = imos::ws40::composeUrl("wss://host/mos", parsed.value());
    const auto reparsed = imos::ws40::parseConnectionParams(url);
    if (!reparsed.ok()) {
        std::abort();  // composed output must parse
    }
    if (reparsed.value().mosID != parsed.value().mosID ||
        reparsed.value().ncsID != parsed.value().ncsID ||
        reparsed.value().channel != parsed.value().channel ||
        reparsed.value().passive != parsed.value().passive ||
        reparsed.value().extras != parsed.value().extras) {
        std::abort();  // parse ∘ compose must be the identity on MOS params
    }
    return 0;
}
