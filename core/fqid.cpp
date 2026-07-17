// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/fqid.h>

#include <array>
#include <cstddef>

namespace imos {

namespace {

constexpr std::string_view kMosLabel = "mos";

Error badFqid(std::string_view id, std::string_view reason) {
    return makeError(
        ErrorCode::InvalidArgument,
        "'" + std::string{id} + "' is not a Fully Qualified MOS ID (" + std::string{reason} + ")");
}

}  // namespace

std::string Fqid::compose() const {
    std::string id;
    for (const std::string* label : {&family, &machine, &location, &enterprise}) {
        if (!label->empty()) {
            id += *label;
            id += '.';
        }
    }
    id += kMosLabel;
    return id;
}

Result<Fqid> parseFqid(std::string_view id) {
    // The convention allows at most <family>.<machine>.<location>.<enterprise>
    // before ".mos": five dot-separated labels. Split into a fixed buffer and
    // fail fast at the sixth, so no input — including a hostile run of dots —
    // forces an allocation or an unbounded split (the header's allocation-
    // light promise, and a small hardening of this fuzz surface).
    constexpr std::size_t kMaxLabels = 5;
    std::array<std::string_view, kMaxLabels> labels{};
    std::size_t count = 0;
    std::size_t start = 0;
    while (true) {
        if (count == kMaxLabels) {
            return badFqid(id,
                           "it has more labels than "
                           "<family>.<machine>.<location>.<enterprise>.mos");
        }
        const std::size_t dot = id.find('.', start);
        labels[count++] = id.substr(start, dot - start);
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    if (labels[count - 1] != kMosLabel) {
        return badFqid(id, "it does not end in \".mos\"");
    }
    const std::size_t bodyCount = count - 1;  // labels preceding ".mos"
    if (bodyCount < 2) {
        return badFqid(id, "it needs at least <family>.<machine> before \".mos\"");
    }
    for (std::size_t i = 0; i < bodyCount; ++i) {
        if (labels[i].empty()) {
            return badFqid(id, "it contains an empty label");
        }
    }
    Fqid fqid;
    fqid.family = labels[0];
    fqid.machine = labels[1];
    if (bodyCount > 2) {
        fqid.location = labels[2];  // 3-label form: positionally the location
    }
    if (bodyCount > 3) {
        fqid.enterprise = labels[3];
    }
    return fqid;
}

std::string_view localityName(Locality locality) noexcept {
    switch (locality) {
        case Locality::Self:
            return "Self";
        case Locality::SameEnterprise:
            return "SameEnterprise";
        case Locality::Foreign:
            return "Foreign";
        case Locality::NotFqid:
            break;
    }
    return "NotFqid";
}

Locality classify(const Fqid& self, const Fqid& object) noexcept {
    // Labels compare exactly, absent matching only absent (header tie-break
    // rules). The family never affects locality: the machine, not the
    // product family, is what an object transfer would have to cross. The
    // enterprise alone bounds reachability — the spec extends transfers
    // across buildings and cities, so a different location stays
    // SameEnterprise; only the machine identity (machine + location) is
    // Self.
    if (self.enterprise != object.enterprise) {
        return Locality::Foreign;
    }
    if (self.machine == object.machine && self.location == object.location) {
        return Locality::Self;
    }
    return Locality::SameEnterprise;
}

Locality classify(const Fqid& self, std::string_view objectMosID) {
    const Result<Fqid> object = parseFqid(objectMosID);
    if (!object.ok()) {
        return Locality::NotFqid;
    }
    return classify(self, object.value());
}

}  // namespace imos
