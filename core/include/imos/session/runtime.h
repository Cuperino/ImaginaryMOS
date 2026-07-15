// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Injected side-effect interfaces (plan §4.1). The sans-I/O core never reads
// a clock, spawns a thread, or touches a socket/file: hosts inject these.
// Deterministic tests inject fakes — a fake clock makes a 60-second
// ack-timeout test run in microseconds.

#pragma once

#include <cstdint>
#include <string_view>

namespace imos {

/// Milliseconds on a host-defined monotonic timeline. Only differences are
/// meaningful; the epoch is arbitrary (but must not go backwards).
struct MonotonicTime {
    std::int64_t millis = 0;

    friend constexpr bool operator==(MonotonicTime a, MonotonicTime b) noexcept = default;
    friend constexpr auto operator<=>(MonotonicTime a, MonotonicTime b) noexcept = default;
};

constexpr MonotonicTime operator+(MonotonicTime t, std::int64_t deltaMillis) noexcept {
    return MonotonicTime{t.millis + deltaMillis};
}

/// Milliseconds since the Unix epoch, UTC. Used for spec timestamps
/// (heartbeat <time>, store records), never for scheduling.
struct WallClockTime {
    std::int64_t millisSinceEpoch = 0;
};

class IClock {
public:
    virtual ~IClock() = default;
    [[nodiscard]] virtual MonotonicTime now() = 0;
    [[nodiscard]] virtual WallClockTime wallClock() = 0;
};

/// Wakeup scheduling for push-mode hosts: the session announces the earliest
/// deadline it needs a poll() by; pull-mode hosts may ignore this entirely
/// and poll on their own cadence.
class ITimerQueue {
public:
    virtual ~ITimerQueue() = default;
    virtual void requestWakeup(MonotonicTime deadline) = 0;
};

class IRandom {
public:
    virtual ~IRandom() = default;
    /// Uniform bits; used for retry/reconnect jitter, never for secrets.
    [[nodiscard]] virtual std::uint64_t next() = 0;
};

enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warning, Error };

class ILogger {
public:
    virtual ~ILogger() = default;
    virtual void log(LogLevel level, std::string_view message) noexcept = 0;
};

}  // namespace imos
