// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Shared sans-I/O test doubles reused by the profile conformance suites. A
// fake clock turns the spec's second-scale timers into microsecond-fast
// deterministic tests; a fake random makes retry/pace jitter reproducible; a
// loopback endpoint queues outbound frames so a harness can control delivery
// (drop / hold / duplicate) for negative branches.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

#include <imos/protocol.h>
#include <imos/session/runtime.h>
#include <imos/session/transport.h>

namespace imos::test {

class FakeClock : public imos::IClock {
public:
    explicit FakeClock(std::int64_t startMillis = 1000) : now_{startMillis} {}

    imos::MonotonicTime now() override { return now_; }
    imos::WallClockTime wallClock() override {
        return imos::WallClockTime{kEpochBase + now_.millis};
    }

    void advance(std::int64_t millis) { now_.millis += millis; }
    void advanceSeconds(std::int64_t seconds) { advance(seconds * 1000); }

private:
    static constexpr std::int64_t kEpochBase = 1'750'000'000'000;
    imos::MonotonicTime now_;
};

class FakeRandom : public imos::IRandom {
public:
    std::uint64_t next() override { return value_; }
    void set(std::uint64_t value) { value_ = value; }

private:
    std::uint64_t value_ = 0;
};

/// ITransport that queues outbound frames per channel instead of sending them;
/// a harness delivers them to the peer session via its pump(). The queues are
/// public so negative tests can drop, hold, reorder, or duplicate frames.
class LoopbackEndpoint : public imos::ITransport {
public:
    imos::Result<void> send(imos::Channel channel, std::span<const std::uint8_t> frame) override {
        outbound[static_cast<std::size_t>(channel)].emplace_back(frame.begin(), frame.end());
        return {};
    }
    [[nodiscard]] bool isConnected(imos::Channel channel) const override {
        return connected[static_cast<std::size_t>(channel)];
    }

    std::array<std::deque<std::vector<std::uint8_t>>, imos::kChannelCount> outbound;
    std::array<bool, imos::kChannelCount> connected{};
};

}  // namespace imos::test
