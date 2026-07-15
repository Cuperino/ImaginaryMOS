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
// Usage: imos-ws40-wasm-smoke <port>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>

#include <emscripten.h>

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

/// Under node, exiting means letting the event loop drain: close the
/// WebSockets first (live handles would keep node alive forever — the
/// force_exit unwind only records the exit code).
void finish(int code) {
    app->client->stop();
    emscripten_cancel_main_loop();
    emscripten_force_exit(code);
}

void tick() {
    static int ticks = 0;
    ++ticks;
    app->client->pump(*app->session);
    app->session->poll();
    while (auto event = app->session->nextEvent()) {
        if (app->responder->handle(*event)) {
            ++app->answered;
            std::fprintf(stderr, "[smoke] answered reqMachInfo (tick %d)\n", ticks);
        }
    }
    if (ticks % 500 == 0) {
        std::fprintf(stderr, "[smoke] tick %d answered=%d\n", ticks, app->answered);
    }
    if (app->answered > 0 && ++app->graceTicks > 25) {
        std::fprintf(stderr, "[smoke] done, exiting 0\n");
        finish(0);
    }
    if (emscripten_get_now() > app->deadlineMillis) {
        std::fprintf(stderr, "[smoke] deadline hit, exiting 1\n");
        finish(1);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        return 2;
    }
    app = new App;
    app->deadlineMillis = emscripten_get_now() + 20'000;

    imos::transports::ws40::ClientConfig config;
    config.endpointUrl = std::string{"ws://127.0.0.1:"} + argv[1] + "/mos/Communication";
    config.mosID = "vtr7.playout.example.mos";
    config.ncsID = "ncs.example.com";
    config.channels = {imos::Channel::Mom, imos::Channel::Ro, imos::Channel::Aux};
    config.passive = true;
    config.reconnect.initialDelayMillis = 50;
    config.reconnect.maxDelayMillis = 500;
    app->client =
        std::make_unique<imos::transports::ws40::Ws40EmscriptenClient>(config, &app->clock);

    imos::SessionConfig sessionConfig;
    sessionConfig.mosID = config.mosID;
    sessionConfig.ncsID = config.ncsID;
    sessionConfig.keepAliveIntervalMillis = 0;
    app->session = std::make_unique<imos::PeerSession>(
        sessionConfig,
        imos::PeerSession::Dependencies{&app->clock, app->client.get(), &app->store});

    imos::profiles::MachineInfo info;
    info.manufacturer = "Imaginary";
    info.model = "WASM-1";
    info.swRev = "1.0.0";
    info.id = config.mosID;
    info.deviceType = imos::profiles::DeviceType::Mos;
    info.profiles[0] = true;
    app->responder =
        std::make_unique<imos::profiles::Profile0Responder>(info, app->session.get(), &app->clock);

    if (const auto started = app->client->start(); !started.ok()) {
        return 3;
    }
    emscripten_set_main_loop(&tick, 0, /*simulate_infinite_loop=*/1);
    return 0;
}
