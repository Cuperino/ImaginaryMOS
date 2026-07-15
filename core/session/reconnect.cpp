// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0

#include <imos/session/reconnect.h>

#include <algorithm>

namespace imos {

ReconnectPolicy::ReconnectPolicy(ReconnectConfig config, IClock* clock, IRandom* random)
    : config_(config), clock_(clock), random_(random) {
    config_.initialDelayMillis = std::max<std::int64_t>(config_.initialDelayMillis, 0);
    config_.maxDelayMillis = std::max(config_.maxDelayMillis, config_.initialDelayMillis);
    config_.backoffFactor = std::max(config_.backoffFactor, 1);
    config_.jitterPercent = std::clamp(config_.jitterPercent, 0, 100);
}

std::int64_t ReconnectPolicy::delayForFailureCount() const {
    std::int64_t delay = config_.initialDelayMillis;
    for (int i = 0; i < failures_; ++i) {
        if (delay >= config_.maxDelayMillis / config_.backoffFactor) {
            return config_.maxDelayMillis;  // saturate without overflow
        }
        delay *= config_.backoffFactor;
    }
    return std::min(delay, config_.maxDelayMillis);
}

void ReconnectPolicy::onDisconnected() {
    if (attemptInFlight_) {
        ++failures_;  // the retry itself failed: back off further
    } else {
        failures_ = 0;  // a working connection dropped: start fresh
    }
    attemptInFlight_ = false;
    attemptPending_ = true;

    std::int64_t delay = delayForFailureCount();
    if (random_ != nullptr && config_.jitterPercent > 0 && delay > 0) {
        const auto bound = static_cast<std::uint64_t>(delay) *
                           static_cast<std::uint64_t>(config_.jitterPercent) / 100U;
        if (bound > 0) {
            delay += static_cast<std::int64_t>(random_->next() % (bound + 1));
        }
    }
    nextAttempt_ = clock_->now() + delay;
}

void ReconnectPolicy::onConnected() {
    attemptPending_ = false;
    attemptInFlight_ = false;
    failures_ = 0;
}

bool ReconnectPolicy::shouldAttempt() {
    if (!attemptPending_ || clock_->now() < nextAttempt_) {
        return false;
    }
    attemptPending_ = false;
    attemptInFlight_ = true;
    return true;
}

std::optional<MonotonicTime> ReconnectPolicy::nextAttemptAt() const {
    if (!attemptPending_) {
        return std::nullopt;
    }
    return nextAttempt_;
}

}  // namespace imos
