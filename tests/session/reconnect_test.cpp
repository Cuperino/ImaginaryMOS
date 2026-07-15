// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// ReconnectPolicy (plan §4.4): near-immediate first retry per the spec's
// passive-mode "as quickly as possible", capped exponential backoff with
// jitter on repeated failures, reset on success — all deterministic against
// the fake clock and a scripted IRandom.

#include <cstdint>

#include <gtest/gtest.h>

#include <imos/session/reconnect.h>

using imos::MonotonicTime;
using imos::ReconnectConfig;
using imos::ReconnectPolicy;

namespace {

class FakeClock : public imos::IClock {
public:
    MonotonicTime now() override { return now_; }
    imos::WallClockTime wallClock() override { return {now_.millis}; }
    void advance(std::int64_t millis) { now_.millis += millis; }

private:
    MonotonicTime now_{1000};
};

class ScriptedRandom : public imos::IRandom {
public:
    explicit ScriptedRandom(std::uint64_t value) : value_(value) {}
    std::uint64_t next() override { return value_; }

private:
    std::uint64_t value_;
};

}  // namespace

TEST(ReconnectPolicy, FirstRetryIsNearImmediate) {
    FakeClock clock;
    ReconnectPolicy policy{{}, &clock};

    EXPECT_FALSE(policy.shouldAttempt());  // nothing dropped yet
    policy.onDisconnected();
    EXPECT_FALSE(policy.shouldAttempt());  // initial delay not elapsed
    ASSERT_TRUE(policy.nextAttemptAt().has_value());
    EXPECT_EQ(policy.nextAttemptAt()->millis, 1000 + 100);

    clock.advance(100);
    EXPECT_TRUE(policy.shouldAttempt());
    EXPECT_FALSE(policy.shouldAttempt());  // one attempt per grant
}

TEST(ReconnectPolicy, BacksOffExponentiallyToCap) {
    FakeClock clock;
    ReconnectConfig config;
    config.initialDelayMillis = 100;
    config.maxDelayMillis = 1000;
    config.backoffFactor = 2;
    config.jitterPercent = 0;
    ReconnectPolicy policy{config, &clock};

    std::int64_t expected[] = {100, 200, 400, 800, 1000, 1000};
    policy.onDisconnected();
    for (const std::int64_t delay : expected) {
        ASSERT_TRUE(policy.nextAttemptAt().has_value());
        EXPECT_EQ(policy.nextAttemptAt()->millis - clock.now().millis, delay);
        clock.advance(delay);
        ASSERT_TRUE(policy.shouldAttempt());
        policy.onDisconnected();  // the attempt failed
    }
    EXPECT_EQ(policy.consecutiveFailures(), 6);
}

TEST(ReconnectPolicy, SuccessResetsTheSchedule) {
    FakeClock clock;
    ReconnectConfig config;
    config.jitterPercent = 0;
    ReconnectPolicy policy{config, &clock};

    policy.onDisconnected();
    clock.advance(100);
    ASSERT_TRUE(policy.shouldAttempt());
    policy.onDisconnected();  // failed retry → backed off
    policy.onConnected();     // …but then a connect landed
    EXPECT_EQ(policy.consecutiveFailures(), 0);
    EXPECT_FALSE(policy.nextAttemptAt().has_value());

    policy.onDisconnected();  // fresh drop starts from the initial delay again
    EXPECT_EQ(policy.nextAttemptAt()->millis - clock.now().millis, 100);
}

TEST(ReconnectPolicy, JitterIsBoundedFraction) {
    FakeClock clock;
    ScriptedRandom random{999'999'999};  // large; only the modulo matters
    ReconnectConfig config;
    config.initialDelayMillis = 1000;
    config.jitterPercent = 10;
    ReconnectPolicy policy{config, &clock, &random};

    policy.onDisconnected();
    const std::int64_t delay = policy.nextAttemptAt()->millis - clock.now().millis;
    EXPECT_GE(delay, 1000);
    EXPECT_LE(delay, 1100);
}
