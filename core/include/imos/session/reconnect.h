// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Reconnect scheduling for dropped channels (plan §4.4). The spec's passive
// mode demands re-establishment "as quickly as possible" when a connection
// drops, so the first attempt is near-immediate; repeated failures back off
// exponentially with jitter (so a fleet of devices behind one firewall does
// not reconnect in lockstep) up to a cap, and a successful connection resets
// the schedule. Sans-I/O: the policy only computes instants against the
// injected clock; the transport glue asks shouldAttempt() from its pump and
// reports outcomes back.

#pragma once

#include <cstdint>
#include <optional>

#include <imos/core_export.h>
#include <imos/session/runtime.h>

namespace imos {

struct ReconnectConfig {
    /// Delay before the first retry after a drop. Near-immediate per the
    /// spec's "as quickly as possible"; non-zero so a flapping listener is
    /// not hammered in a tight loop.
    std::int64_t initialDelayMillis = 100;
    /// Upper bound the exponential backoff saturates at.
    std::int64_t maxDelayMillis = 10'000;
    /// Multiplier applied per consecutive failure (initial, 2×, 4×, …).
    int backoffFactor = 2;
    /// Jitter added on top of each delay, as a fraction of it in percent
    /// (0–100). 10 → up to +10% of the computed delay.
    int jitterPercent = 10;
};

class IMOS_CORE_API ReconnectPolicy {
public:
    ReconnectPolicy(ReconnectConfig config, IClock* clock, IRandom* random = nullptr);

    /// The channel dropped (or an attempt failed before connecting):
    /// schedule the next attempt.
    void onDisconnected();

    /// The channel connected: reset the backoff schedule.
    void onConnected();

    /// True when a reconnect attempt is due now. The caller performs the
    /// attempt; a subsequent onDisconnected()/onConnected() records the
    /// outcome. Returns false while no attempt is pending (connected or
    /// never dropped) or the delay has not elapsed yet.
    [[nodiscard]] bool shouldAttempt();

    /// The instant shouldAttempt() turns true, when an attempt is pending.
    [[nodiscard]] std::optional<MonotonicTime> nextAttemptAt() const;

    [[nodiscard]] int consecutiveFailures() const noexcept { return failures_; }

private:
    [[nodiscard]] std::int64_t delayForFailureCount() const;

    ReconnectConfig config_;
    IClock* clock_;
    IRandom* random_;
    bool attemptPending_ = false;
    bool attemptInFlight_ = false;
    int failures_ = 0;
    MonotonicTime nextAttempt_{};
};

}  // namespace imos
