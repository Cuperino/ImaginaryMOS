// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/dialect.h>

namespace imos {

DialectProfile baselineDialect() {
    return {};
}

DialectProfile sofieDialect() {
    DialectProfile dialect;
    dialect.name = "sofie";
    dialect.padLoneParagraph = true;
    return dialect;
}

std::optional<DialectProfile> dialectNamed(std::string_view name) {
    if (name == "baseline") {
        return baselineDialect();
    }
    if (name == "sofie") {
        return sofieDialect();
    }
    return std::nullopt;
}

std::vector<std::string> dialectNames() {
    return {"baseline", "sofie"};
}

}  // namespace imos
