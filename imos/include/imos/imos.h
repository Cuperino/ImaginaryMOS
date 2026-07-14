// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>

#include <imos/imos_export.h>

/// Media Object Server (device) role API. See plan §4.6 / §8.1. At M0 this
/// surface is a placeholder that establishes the library and its linkage; the
/// per-profile handler interfaces and `imos::Device` land from M2 onward.
namespace imos {

/// Role library name, "imos".
[[nodiscard]] IMOS_IMOS_API std::string_view libraryName() noexcept;

/// Version of the underlying core engine.
[[nodiscard]] IMOS_IMOS_API std::string_view version() noexcept;

}  // namespace imos
