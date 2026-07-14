// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <incs/incs.h>

#include <imos/version.h>

namespace incs {

std::string_view libraryName() noexcept {
    return "incs";
}

std::string_view version() noexcept {
    return imos::coreVersion();
}

}  // namespace incs
