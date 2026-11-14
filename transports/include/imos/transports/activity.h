// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// The transports' wake-up hook for host event loops. Every transport that
// receives on threads of its own queues what it receives and leaves the
// draining to the host's pump() call. Without a hook the host can only find
// out that something is queued by polling. An activity handler is called
// after each item is queued, so a host can post one "pump now" to its own
// loop (a Qt queued call, a libuv async handle, an eventfd write) and drop
// the polling timer.
//
// Contract, the same on every transport:
//  - Install the handler before start(); it is read without a lock from the
//    transport's threads, so it must not change while the transport runs.
//  - It is called on a transport thread (on the browser's main thread for the
//    Emscripten client), after the item is queued and outside every
//    transport lock: by the time it runs, pump() will see the item.
//  - It may be called many times for one burst. Coalescing is the host's job;
//    a host that posts a wake-up only when none is pending stays cheap.
//  - It must be fast, must not throw, and must not call back into the
//    transport. All real work belongs in pump(), on the host's thread.
//  - Deadlines (reconnect attempts, redials) are not activity: hosts still
//    arm a timer from nextDeadline().

#pragma once

#include <functional>

namespace imos::transports {

using ActivityHandler = std::function<void()>;

}  // namespace imos::transports
