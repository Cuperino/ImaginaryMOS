// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Fuzz surface 5 (plan §10, M5): the Profile 6 FQID parser — a new, if
// small, input surface: mosIDs arrive from the wire and from host
// configuration. Invariants: no crash on any input; compose ∘ parse is the
// identity on accepted ids (so classification sees exactly what was
// configured); classification itself never crashes on arbitrary object ids.

#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <imos/fqid.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view id{reinterpret_cast<const char*>(data), size};
    const auto parsed = imos::parseFqid(id);
    if (!parsed.ok()) {
        // Still exercise classification against a fixed self: arbitrary
        // object ids must classify (NotFqid) without incident.
        const auto self = imos::parseFqid("aveed.server1.camden.cbs.mos");
        if (imos::classify(self.value(), id) != imos::Locality::NotFqid) {
            std::abort();
        }
        return 0;
    }
    if (!imos::parseFqid(parsed.value().compose()).ok()) {
        std::abort();  // composed output must parse
    }
    const auto reparsed = imos::parseFqid(parsed.value().compose());
    if (reparsed.value().family != parsed.value().family ||
        reparsed.value().machine != parsed.value().machine ||
        reparsed.value().location != parsed.value().location ||
        reparsed.value().enterprise != parsed.value().enterprise) {
        std::abort();  // parse ∘ compose must be the identity
    }
    if (imos::classify(parsed.value(), parsed.value()) != imos::Locality::Self) {
        std::abort();  // every FQID is Self to itself
    }
    return 0;
}
