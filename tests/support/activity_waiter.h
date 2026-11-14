// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// Test support for the transports' activity hook (<imos/transports/
// activity.h>): a host loop that sleeps until a transport says there is
// something to pump, instead of polling on a timer. A test passes only if
// every step of its exchange was woken by the hook — a wait that times out
// fails the drive, so a transport that forgets to signal cannot hide behind
// a polling fallback.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>

#include <imos/transports/activity.h>

namespace imos::test {

class ActivityWaiter {
public:
    /// The handler to install on one or more transports (before start()).
    [[nodiscard]] imos::transports::ActivityHandler handler() {
        return [this] {
            {
                const std::scoped_lock lock{mutex_};
                ++count_;
            }
            wake_.notify_all();
        };
    }

    [[nodiscard]] std::uint64_t count() const {
        const std::scoped_lock lock{mutex_};
        return count_;
    }

    /// Pump, then sleep until the hook fires, until `done` holds. The count
    /// is sampled *before* each pump, so activity that lands while pumping
    /// is not lost. Returns false if any wait lasts longer than kQuiet (the
    /// hook failed to wake the host) or the whole drive exceeds kTotal.
    bool drive(const std::function<void()>& pump, const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + kTotal;
        for (;;) {
            const std::uint64_t seen = count();
            pump();
            if (done()) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::unique_lock lock{mutex_};
            if (!wake_.wait_for(lock, kQuiet, [&] { return count_ != seen; })) {
                return false;
            }
        }
    }

private:
    static constexpr std::chrono::milliseconds kQuiet{3000};
    static constexpr std::chrono::milliseconds kTotal{10000};

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::uint64_t count_ = 0;
};

}  // namespace imos::test
