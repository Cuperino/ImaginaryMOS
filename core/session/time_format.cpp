// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/time_format.h>

#include <chrono>
#include <cstdio>

namespace imos {

std::string formatSpecTime(WallClockTime time) {
    using namespace std::chrono;
    const sys_time<milliseconds> point{milliseconds{time.millisSinceEpoch}};
    const auto day = floor<days>(point);
    const year_month_day date{day};
    const hh_mm_ss<milliseconds> clock{point - day};
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02uT%02lld:%02lld:%02lld",
                  static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                  static_cast<unsigned>(date.day()), static_cast<long long>(clock.hours().count()),
                  static_cast<long long>(clock.minutes().count()),
                  static_cast<long long>(clock.seconds().count()));
    return buffer;
}

}  // namespace imos
