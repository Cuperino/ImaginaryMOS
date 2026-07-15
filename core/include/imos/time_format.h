// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Spec §6 time field formatting. Times look like "2009-04-11T17:20:42" with
// optional fractional seconds and optional zone; M1 emits the plain UTC
// form. Parsing tolerance for dialect variants (missing 'Z', fractional
// seconds, timezone-less local times) lands with the field-format work.

#pragma once

#include <string>

#include <imos/core_export.h>
#include <imos/session/runtime.h>

namespace imos {

/// "YYYY-MM-DDTHH:MM:SS" in UTC.
[[nodiscard]] IMOS_CORE_API std::string formatSpecTime(WallClockTime time);

}  // namespace imos
