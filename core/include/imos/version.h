// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string_view>

#include <imos/core_export.h>

#define IMOS_VERSION_MAJOR 1
#define IMOS_VERSION_MINOR 0
#define IMOS_VERSION_PATCH 0
#define IMOS_VERSION_STRING "1.0.0"

namespace imos {

/// Semantic version of the ImaginaryMOS core engine, e.g. "1.0.0".
[[nodiscard]] IMOS_CORE_API std::string_view coreVersion() noexcept;

/// Versions of the third-party libraries the core is linked against. Useful
/// for diagnostics and for confirming the pinned build wired up correctly.
struct DependencyVersions {
    std::string_view libxml2;
    std::string_view zlib;
};

[[nodiscard]] IMOS_CORE_API DependencyVersions dependencyVersions() noexcept;

}  // namespace imos
