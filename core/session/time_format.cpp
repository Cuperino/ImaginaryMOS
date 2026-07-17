// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/time_format.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace imos {

namespace {

/// Consume exactly `count` digits from `text` at `pos` into `out`.
bool readDigits(std::string_view text, std::size_t& pos, int count, int& out) {
    if (pos + static_cast<std::size_t>(count) > text.size()) {
        return false;
    }
    int value = 0;
    for (int i = 0; i < count; ++i) {
        const char c = text[pos + static_cast<std::size_t>(i)];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    pos += static_cast<std::size_t>(count);
    out = value;
    return true;
}

bool readSeparator(std::string_view text, std::size_t& pos, char separator) {
    if (pos >= text.size() || text[pos] != separator) {
        return false;
    }
    ++pos;
    return true;
}

}  // namespace

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

Result<WallClockTime> parseSpecTime(std::string_view text) {
    using namespace std::chrono;
    const auto fail = [&text]() {
        return makeError(ErrorCode::InvalidArgument,
                         "'" + std::string{text} + "' is not a spec §6 time");
    };

    std::size_t pos = 0;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!readDigits(text, pos, 4, year) || !readSeparator(text, pos, '-') ||
        !readDigits(text, pos, 2, month) || !readSeparator(text, pos, '-') ||
        !readDigits(text, pos, 2, day) || !readSeparator(text, pos, 'T') ||
        !readDigits(text, pos, 2, hour) || !readSeparator(text, pos, ':') ||
        !readDigits(text, pos, 2, minute) || !readSeparator(text, pos, ':') ||
        !readDigits(text, pos, 2, second)) {
        return fail();
    }

    // Fractional seconds: any precision, kept to milliseconds.
    std::int64_t fractionMillis = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        int digits = 0;
        int scale = 100;
        if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') {
            return fail();
        }
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (digits < 3) {
                fractionMillis += static_cast<std::int64_t>(text[pos] - '0') * scale;
                scale /= 10;
            }
            ++digits;
            ++pos;
        }
    }

    // Zone: none (UTC), 'Z', or ±hh[[:]mm].
    std::int64_t offsetMillis = 0;
    if (pos < text.size()) {
        const char zone = text[pos];
        if (zone == 'Z') {
            ++pos;
        } else if (zone == '+' || zone == '-') {
            ++pos;
            int offsetHours = 0;
            int offsetMinutes = 0;
            if (!readDigits(text, pos, 2, offsetHours)) {
                return fail();
            }
            if (pos < text.size()) {
                if (text[pos] == ':') {
                    ++pos;
                }
                if (!readDigits(text, pos, 2, offsetMinutes)) {
                    return fail();
                }
            }
            offsetMillis = (static_cast<std::int64_t>(offsetHours) * 60 + offsetMinutes) * 60'000;
            if (zone == '-') {
                offsetMillis = -offsetMillis;
            }
        }
    }
    if (pos != text.size()) {
        return fail();
    }

    const year_month_day date{std::chrono::year{year}, std::chrono::month{unsigned(month)},
                              std::chrono::day{unsigned(day)}};
    if (!date.ok() || hour > 23 || minute > 59 || second > 60) {
        return fail();
    }
    const sys_days dayPoint{date};
    const std::int64_t millis =
        duration_cast<milliseconds>(dayPoint.time_since_epoch()).count() +
        (static_cast<std::int64_t>(hour) * 3'600 + minute * 60 + second) * 1'000 + fractionMillis -
        offsetMillis;
    return WallClockTime{millis};
}

}  // namespace imos
