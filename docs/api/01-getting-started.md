<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Getting started {#getting_started}

[TOC]

This page takes you from a clean checkout to a running MOS session: build the
libraries, provide the runtime dependencies the sans-I/O core needs, wire a
role to a transport, and drive the pull-mode host loop. The
@ref profiles_overview "profile guides" then build on this foundation.

## 1. Build the libraries {#gs_build}

Dependencies are pinned as git submodules, and the CMake **presets** carry every
flag the build needs (including the CMake-4 policy shim the vendored deps
require) — always configure through a preset.

```sh
git submodule update --init --recursive   # fetch pinned deps (once)

cmake --preset linux                        # configure
cmake --build --preset linux                # build libs + tools + tests
ctest --preset linux                        # run the suite
```

Presets exist for every target: `linux`, `macos`, `windows`, `android`, `ios`,
`wasm`, plus `linux-release`, `linux-asan`, `linux-tsan`, `linux-coverage`,
`linux-tls`, and `linux-fuzz`. Cross-compile presets expect the matching
toolchain (`ANDROID_NDK`, `EMSDK`, Xcode).

### Consuming ImaginaryMOS from your own CMake project

Until the installed package config ships (an M7 deliverable), embed the tree and
link the `imos::` aliases:

```cmake
add_subdirectory(third_party/ImaginaryMOS)

target_link_libraries(my_device PRIVATE
  imos::imos            # device role facade
  imos::transports)     # the MOS 4.0 WebSocket transport

# An NCS/newsroom host links imos::incs instead of (or alongside) imos::imos.
```

Every first-party library emits both a static and a shared target with hidden
visibility; the `imos::<component>` alias resolves to whichever your
`IMOS_BUILD_SHARED` / `IMOS_BUILD_STATIC` settings selected.

### Building these docs

```sh
cmake --preset linux -DIMOS_BUILD_DOCS=ON
cmake --build --preset linux --target docs
# open build/linux/docs/html/index.html
```

## 2. The three things every role needs {#gs_ingredients}

A role facade — @ref imos::Device or @ref incs::Ncs — is constructed from a
**config** plus a bundle of **injected dependencies**, and then attached to a
**transport**. Nothing under `core/` performs I/O itself, so you supply the
side effects.

### Injected dependencies

@ref imos::PeerSession::Dependencies gathers them. Three are required, three are
optional:

```cpp
struct Dependencies {
    IClock*        clock;      // required — monotonic + wall-clock time
    ITransport*    transport;  // required — where framed bytes go out / come in
    IMessageStore* store;      // required — durable-before-send buffer + dedupe
    ILogger*       logger;     // optional — diagnostics
    IRandom*       random;     // optional — retry/reconnect jitter
    ITimerQueue*   timers;     // optional — push-mode wakeups
};
```

The transport is normally one of the shipped ones (see step 4). For the store,
the simplest choice is @ref imos::InMemoryMessageStore; embedded devices that
must survive a restart mid-exchange use the file-backed
@ref imos::store::FileMessageStore instead. `IClock`, `IRandom`, and `ILogger`
are yours to implement over your platform's facilities (a `std::chrono` /
`std::mt19937` pair is typical). Injecting **fakes** here is what makes the core
deterministically testable — see @ref gs_testing.

### Session configuration

@ref imos::SessionConfig describes the one peer relationship: the identities,
the reliability knobs, and the wire encoding.

```cpp
imos::SessionConfig session;
session.mosID = "acme.vtr.example";     // this/the peer device's MOS ID
session.ncsID = "newsroom.example.com"; // this/the peer NCS ID
session.retryTimeoutMillis     = 10'000; // ack timeout (< the 60 s spec ceiling)
session.maxRetries             = 3;      // then the link faults
session.keepAliveIntervalMillis = 30'000; // spec floor; 0 disables
// §3.4 wire encoding — UCS-2 big-endian on binary frames is the 4.0 default.
session.wireEncode.encoding = imos::encoding::WireEncoding::Ucs2Be;
session.wireDecode.encoding = imos::encoding::WireEncoding::Ucs2Be;
```

> **Encoding must agree end to end.** The `SessionConfig` encoding and the
> transport's `wireEncoding` must match, and both must match what the peer
> speaks. UCS-2 rides *binary* WebSocket frames; UTF-8 rides *text* frames. The
> receive side expects the configured encoding rather than sniffing.

### Machine info

@ref imos::profiles::MachineInfo is what the role advertises in `listMachInfo`
(Profile 0). Its `profiles[]` array declares which profiles this endpoint
implements; peers read it during capability negotiation.

```cpp
imos::profiles::MachineInfo info;
info.manufacturer = "Acme Broadcast";
info.model        = "VTR-7";
info.swRev        = "1.0.0";
info.id           = session.mosID;
info.deviceType   = imos::profiles::DeviceType::Mos; // forced to match the role
info.profiles     = {/*0*/true, /*1*/true, /*2*/true, /*3*/false,
                     /*4*/true, /*5*/true, /*6*/false, /*7*/true};
```

## 3. Construct the facade {#gs_construct}

For the **device** side, bundle the session and machine info into a
@ref imos::DeviceConfig "DeviceConfig":

```cpp
imos::DeviceConfig config{session, info};
config.hydrateRunningOrdersOnConnect = false; // Profile 4 option, see its guide

imos::Device device{
    std::move(config),
    imos::PeerSession::Dependencies{&clock, &transport, &store,
                                    /*logger*/ nullptr, &random, /*timers*/ nullptr}};
```

The **NCS** side mirrors this with @ref incs::NcsConfig and @ref incs::Ncs "incs::Ncs":

```cpp
incs::NcsConfig ncsConfig{session, ncsInfo /* deviceType = Ncs */};
incs::Ncs ncs{std::move(ncsConfig),
              imos::PeerSession::Dependencies{&clock, &transport, &store}};
```

Then register the host objects each profile needs — an object catalog, a story
sink, a control sink. Those `attach()` calls are covered in the individual
profile guides; a device serving objects, for example, does
`device.attach(mySource)` where `mySource` implements
@ref imos::profiles::IObjectSource (see @ref profile1).

> **Profile 6 bring-up gate.** If you enable Profile 6 in the config, call the
> static @ref imos::Device::validateConfig (or @ref incs::Ncs::validateConfig)
> *before* constructing — it rejects a config whose IDs are not Fully Qualified.
> See @ref profile6.

## 4. Wire a transport {#gs_transport}

The MOS 4.0 WebSocket transport lives in `imos::transports::ws40`. A **device**
usually dials out with @ref imos::transports::ws40::Ws40Client; an **NCS**
usually listens with @ref imos::transports::ws40::Ws40Server (a browser device
uses the Emscripten client instead).

### Device as a WebSocket client

```cpp
namespace ws40 = imos::transports::ws40;

ws40::ClientConfig clientConfig;
clientConfig.endpointUrl  = "ws://newsroom.example.com:10540/mos/Communication";
clientConfig.mosID        = session.mosID;
clientConfig.ncsID        = session.ncsID;
clientConfig.channels     = {imos::Channel::Mom, imos::Channel::Ro};
clientConfig.wireEncoding = imos::encoding::WireEncoding::Ucs2Be; // match session
// clientConfig.auth = imos::ws40::BasicCredentials{"user", "pass"}; // if required
// clientConfig.tls.enabled = true; // wss:// endpoints

ws40::Ws40Client client{clientConfig, &clock, &random};
imos::Device device{{session, info},
                    imos::PeerSession::Dependencies{&clock, &client, &store}};
device.attach(myObjectSource);
client.start();                       // begins connecting (asynchronous)
```

### NCS as a WebSocket server

```cpp
ws40::ServerConfig serverConfig;
serverConfig.host = "0.0.0.0";
serverConfig.port = 10540;            // 0 → ephemeral; port() reports the choice

ws40::Ws40Server server{serverConfig};
server.addPeer({session.mosID, session.ncsID}); // register the relationship
server.start();

incs::Ncs ncs{{session, ncsInfo},
              imos::PeerSession::Dependencies{
                  &clock, &server.transportFor(session.mosID, session.ncsID), &store}};
```

## 5. Run the host loop {#gs_loop}

Both facades are **pull-mode**: they own no threads. The contract, verbatim from
the headers, is *"push connectivity/frames in, call `poll()`, drain
`nextEvent()`."* Your loop bridges the transport to the facade and back.

### Bridging a client transport

@ref imos::transports::ws40::Ws40Client::pump drains the socket's queued
connectivity changes and inbound frames straight into the session, so a
client-side loop is compact:

```cpp
for (;;) {
    client.pump(device.session());   // socket → session; also drives reconnect
    device.poll();                   // timers + profile dispatch of what arrived

    while (auto event = device.nextEvent()) {
        // acks for this device's own sends, delivery failures, channel faults,
        // protocol/store errors, Profile 6 foreign-reference notices.
        handle(*event);
    }
    // ... drain the profile-specific queues you care about (see each guide) ...

    wait_until(next_deadline(device, client)); // e.g. device.nextDeadline()
}
```

> **Facade-level connect hooks.** `pump()` feeds connectivity to the *session*.
> Facade features keyed on connect — notably Profile 4
> `hydrateRunningOrdersOnConnect` — live in
> @ref imos::Device::onChannelConnected. If you use them, route connectivity
> through the facade (next pattern) rather than letting `pump()` swallow it.

### Bridging a server transport (facade-method style)

@ref imos::transports::ws40::Ws40Server exposes an event queue instead of a
pump, so the host routes each @ref imos::transports::ws40::ServerEvent into the
matching facade method. This is the fully general integration pattern, and the
one to use whenever you need the facade's own connect/disconnect hooks:

```cpp
for (;;) {
    while (auto ev = server.nextEvent()) {
        using Kind = ws40::ServerEvent::Kind;
        switch (ev->kind) {
            case Kind::ChannelConnected:    ncs.onChannelConnected(ev->channel); break;
            case Kind::ChannelDisconnected: ncs.onChannelDisconnected(ev->channel); break;
            case Kind::Frame:               ncs.onFrameReceived(ev->channel, ev->frame); break;
            case Kind::ProtocolError:
            case Kind::Rejected:            log(ev->diagnostic); break;
        }
    }
    ncs.poll();
    while (auto event = ncs.nextEvent()) handle(*event);
    while (auto dir = ncs.nextDirectoryEvent()) handle(*dir); // Profile 1/3
    wait_until(ncs.nextDeadline());
}
```

That is the whole integration surface: **`on…()` in, `poll()`, `nextEvent()` (and
the per-profile drains) out.** Everything else is profile-specific and covered
in the guides that follow.

## 6. Deterministic testing {#gs_testing}

Because the core is sans-I/O, a full device↔NCS conversation can run in-process
with **no sockets, no threads, and a fake clock** — a 60-second ack-timeout test
completes in microseconds. The repository's `tests/support/TwoRoleFixture` wires
a real @ref imos::Device to a real @ref incs::Ncs over a loopback `ITransport`
pair sharing one `FakeClock`; `pump()` moves one hop per direction and
`settle()` drives the link to quiescence. Reuse that pattern for your own
host-side tests: inject fakes for `IClock` / `ITransport` / `IMessageStore`,
enqueue on one role, `pump()`, and assert the mirror on the other.
