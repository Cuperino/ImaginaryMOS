// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>

#include <incs/incs_export.h>

/// Newsroom Computer System (NCS) role API. See plan §4.6. At M0 this surface
/// is a placeholder; the RO authoring API and object directory land from M3.
namespace incs {

/// Role library name, "incs".
[[nodiscard]] IMOS_INCS_API std::string_view libraryName() noexcept;

/// Version of the underlying core engine.
[[nodiscard]] IMOS_INCS_API std::string_view version() noexcept;

}  // namespace incs
