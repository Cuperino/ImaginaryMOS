// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/imos.h>

#include <imos/version.h>

namespace imos {

std::string_view libraryName() noexcept {
    return "imos";
}

std::string_view version() noexcept {
    return coreVersion();
}

}  // namespace imos
