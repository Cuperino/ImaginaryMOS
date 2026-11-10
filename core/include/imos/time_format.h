// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Spec §6 time field formatting. Times look like "2009-04-11T17:20:42" with
// optional fractional seconds and optional zone; M1 emits the plain UTC
// form, and parsing (M4, Profile 5's roEventTime) tolerates the dialect
// variants: fractional seconds of any precision after either separator the
// spec itself uses (§6 defines ",ddd"; its roItemCue examples write ".00"),
// an optional 'Z', an optional ±hh[:mm] offset, and zone-less times (read
// as UTC).

#pragma once

#include <string>
#include <string_view>

#include <imos/core_export.h>
#include <imos/error.h>
#include <imos/session/runtime.h>

namespace imos {

/// "YYYY-MM-DDTHH:MM:SS" in UTC.
[[nodiscard]] IMOS_CORE_API std::string formatSpecTime(WallClockTime time);

/// Parse a spec time field ("YYYY-MM-DDTHH:MM:SS" plus the tolerated
/// variants above) to UTC. Fractional seconds are kept to millisecond
/// precision; a missing zone means UTC.
[[nodiscard]] IMOS_CORE_API Result<WallClockTime> parseSpecTime(std::string_view text);

}  // namespace imos
