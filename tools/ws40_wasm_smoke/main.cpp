// SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
// SPDX-License-Identifier: Apache-2.0
//
// WASM Profile 0 smoke client (plan §13, M2 exit criteria). Runs the real
// Ws40EmscriptenClient as a passive-mode device against a native Ws40Server:
// connects every channel outward with passive=true, waits for the external
// NCS to originate a reqMachInfo back down the held connection, answers it
// with listMachInfo, and exits 0. Exits 1 on timeout. Driven under node
// (which needs WebSocket support) by the Ws40Transport.WasmClientProfile0
// test in tests/transports/ws40_transport_test.cpp.
//
// A second mode exercises the owning-thread contract on a multithreaded
// (-pthread, wasm-mt preset) build: with the "thread" argument the client is
// constructed, started, pumped, and stopped by a dedicated worker pthread —
// the topology a host like a Qt worker thread uses — while the main thread
// only runs an idle loop (which must stay responsive: the JS WebSocket
// object lives there and worker operations proxy to it). Driven by the
// Ws40Transport.WasmClientProfile0SmokeThreaded test.
//
// Usage: imos-ws40-wasm-smoke <port> [thread]

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <emscripten.h>
#if defined(__EMSCRIPTEN_PTHREADS__)
#include <emscripten/threading.h>

#include <atomic>
#include <thread>
#endif

#include <imos/profiles/profile0.h>
#include <imos/session/in_memory_store.h>
#include <imos/session/peer_session.h>
#include <imos/transports/ws40_emscripten.h>

namespace {

class SystemClock : public imos::IClock {
public:
    imos::MonotonicTime now() override {
        const auto steady = std::chrono::steady_clock::now().time_since_epoch();
        return imos::MonotonicTime{
            std::chrono::duration_cast<std::chrono::milliseconds>(steady).count()};
    }
    imos::WallClockTime wallClock() override {
        const auto system = std::chrono::system_clock::now().time_since_epoch();
        return imos::WallClockTime{
            std::chrono::duration_cast<std::chrono::milliseconds>(system).count()};
    }
};

struct App {
    SystemClock clock;
    imos::InMemoryMessageStore store;
    std::unique_ptr<imos::transports::ws40::Ws40EmscriptenClient> client;
    std::unique_ptr<imos::PeerSession> session;
    std::unique_ptr<imos::profiles::Profile0Responder> responder;
    int answered = 0;
    int graceTicks = 0;  ///< let the queued reply flush before exiting
    double deadlineMillis = 0;
};

App* app = nullptr;

/// Build the client/session/responder for the given server port. start() is
/// NOT called here — the owning thread must call it (owning-thread contract).
void configure(App& app, const char* port) {
    imos::transports::ws40::ClientConfig config;
    config.endpointUrl = std::string{"ws://127.0.0.1:"} + port + "/mos/Communication";
    config.mosID = "vtr7.playout.example.mos";
    config.ncsID = "ncs.example.com";
    config.channels = {imos::Channel::Mom, imos::Channel::Ro, imos::Channel::Aux};
    config.passive = true;
    config.reconnect.initialDelayMillis = 50;
    config.reconnect.maxDelayMillis = 500;
    app.client =
        std::make_unique<imos::transports::ws40::Ws40EmscriptenClient>(config, &app.clock);

    imos::SessionConfig sessionConfig;
    sessionConfig.mosID = config.mosID;
    sessionConfig.ncsID = config.ncsID;
    sessionConfig.keepAliveIntervalMillis = 0;
    app.session = std::make_unique<imos::PeerSession>(
        sessionConfig,
        imos::PeerSession::Dependencies{&app.clock, app.client.get(), &app.store});

    imos::profiles::MachineInfo info;
    info.manufacturer = "Imaginary";
    info.model = "WASM-1";
    info.swRev = "1.0.0";
    info.id = config.mosID;
    info.deviceType = imos::profiles::DeviceType::Mos;
    info.profiles[0] = true;
    app.responder =
        std::make_unique<imos::profiles::Profile0Responder>(info, app.session.get(), &app.clock);
}

/// One pump/poll/drain step; returns the exit code once decided, -1 to keep
/// going. `ticks` is the caller's step counter.
int step(App& app, int ticks) {
    app.client->pump(*app.session);
    app.session->poll();
    while (auto event = app.session->nextEvent()) {
        if (app.responder->handle(*event)) {
            ++app.answered;
            std::fprintf(stderr, "[smoke] answered reqMachInfo (tick %d)\n", ticks);
        }
    }
    if (ticks % 500 == 0) {
        std::fprintf(stderr, "[smoke] tick %d answered=%d\n", ticks, app.answered);
    }
    if (app.answered > 0 && ++app.graceTicks > 25) {
        std::fprintf(stderr, "[smoke] done, exiting 0\n");
        return 0;
    }
    if (emscripten_get_now() > app.deadlineMillis) {
        std::fprintf(stderr, "[smoke] deadline hit, exiting 1\n");
        return 1;
    }
    return -1;
}

/// Under node, exiting means letting the event loop drain: close the
/// WebSockets first (live handles would keep node alive forever — the
/// force_exit unwind only records the exit code).
void finish(int code) {
    emscripten_cancel_main_loop();
    emscripten_force_exit(code);
}

void tick() {
    static int ticks = 0;
    ++ticks;
    const int result = step(*app, ticks);
    if (result >= 0) {
        app->client->stop();
        finish(result);
    }
}

#if defined(__EMSCRIPTEN_PTHREADS__)

// Worker-pthread mode: the worker owns the transport end to end. The main
// thread's only jobs are keeping the browser event loop responsive (proxied
// socket operations run there) and reporting the worker's verdict.

std::atomic<int> workerVerdict{-1};

void workerMain(std::string port) {
    App workerApp;
    workerApp.deadlineMillis = emscripten_get_now() + 20'000;
    configure(workerApp, port.c_str());
    if (const auto started = workerApp.client->start(); !started.ok()) {
        std::fprintf(stderr, "[smoke] worker start() failed: %s\n",
                     started.error().message.c_str());
        workerVerdict = 3;
        return;
    }
    int ticks = 0;
    int result = -1;
    while (result < 0) {
        ++ticks;
        // Deliver callbacks proxied to this thread (connectivity, frames)
        // before pumping; a sleep alone is not guaranteed to service them.
        emscripten_current_thread_process_queued_calls();
        result = step(workerApp, ticks);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    workerApp.client->stop();
    // Give the proxied close a moment to reach the main thread.
    emscripten_current_thread_process_queued_calls();
    workerVerdict = result;
}

void threadedTick() {
    const int verdict = workerVerdict.load();
    if (verdict >= 0) {
        finish(verdict);
    }
}

#endif  // defined(__EMSCRIPTEN_PTHREADS__)

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    const bool threaded = argc > 2 && std::strcmp(argv[2], "thread") == 0;
    if (threaded) {
#if defined(__EMSCRIPTEN_PTHREADS__)
        std::thread worker{workerMain, std::string{argv[1]}};
        worker.detach();  // exit happens via finish(); joining would block the loop
        emscripten_set_main_loop(&threadedTick, 0, /*simulate_infinite_loop=*/1);
        return 0;
#else
        std::fprintf(stderr,
                     "[smoke] thread mode needs a -pthread build (wasm-mt preset)\n");
        return 2;
#endif
    }

    app = new App;
    app->deadlineMillis = emscripten_get_now() + 20'000;
    configure(*app, argv[1]);
    if (const auto started = app->client->start(); !started.ok()) {
        return 3;
    }
    emscripten_set_main_loop(&tick, 0, /*simulate_infinite_loop=*/1);
    return 0;
}
