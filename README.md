<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# ImaginaryMOS

A portable C++20 protocol library suite implementing the **MOS Protocol v4.0**
(all Profiles 0–7) with backwards compatibility for the MOS 2.8.x dual-socket
TCP transport and the MOS 3.x SOAP WebService transport.

It ships two role libraries over a shared, **sans-I/O** core:

- **`imos`** — the Media Object Server (device) side. The primary product,
  designed to be embedded in MOS devices (playout servers, graphics engines,
  prompters, publishing endpoints).
- **`incs`** — the Newsroom Computer System (NCS) side, which also powers the
  NRCS simulator used for interoperability testing.

The core implements every state machine, codec, and profile workflow free of
sockets, threads, and clocks — transports, time, and persistence are injected
behind narrow interfaces. That is what makes one codebase honestly portable
across **Linux, macOS, Windows, Android, iOS, and WebAssembly**, supports both
static and dynamic linking, and makes a stable C ABI practical.

See the [implementation plan](reference/imaginarymos-implementation-plan.md) for
the full design, and [docs/coding-standards.md](docs/coding-standards.md) for
contribution rules.

> **Status: Milestone M3 (Profiles 1–2), complete.** The sans-I/O core
> (codec for the full v4.0 vocabulary, encoding engine, session layer, file
> store) and the MOS 4.0 WebSocket transports — IXWebSocket client/server with
> Basic auth, TLS, permessage-deflate, standard & passive topologies, plus the
> WebAssembly client — are implemented and tested, including Profile 0
> end-to-end in both roles. The `imos::Device` and `incs::Ncs` role facades
> compose the session with **Profile 1** (unsolicited `mosObj` push, single-
> object fetch, full-catalog sync in batch and paced modes) and **Profile 2**
> (the roConstruction family into a transactional `RunningOrderModel` with the
> spec's resync-on-unknown-ID heuristic on the device side; a running-order
> authoring/diff API emitting minimal `roElementAction` sequences on the NCS
> side; bidirectional `roElementStat`). Profiles 4–5 (M4) are next. The public
> API is not yet stable.

## Requirements

- **CMake ≥ 3.24** and **Ninja**
- A **C++20** compiler (GCC 14+, Clang, Apple Clang, MSVC v143+, or Emscripten 4.0.7)
- **git** (dependencies are pinned as submodules under `3rdparty/`)
- Network access on first configure (GoogleTest is fetched, pinned, for tests)

Dependencies — IXWebSocket, libxml2, zlib (pinned submodules) and GoogleTest
(FetchContent) — build from source by default. Pass `-DIMOS_USE_SYSTEM_DEPS=ON`
to use system/vcpkg packages instead.

Optional, depending on what you build and test:

- **OpenSSL development headers** (e.g. Debian/Ubuntu `libssl-dev`) for TLS
  (`wss`) support — `-DIMOS_ENABLE_TLS=ON` or the `linux-tls` preset; the TLS
  test also uses the `openssl` CLI to generate its self-signed certificate.
  Builds without it stay `ws`-only and the TLS tests skip themselves. The TLS
  backend is pinned to OpenSSL on Linux, Android, Windows, and macOS
  (Homebrew/MacPorts); iOS uses Secure Transport. Per the spec, self-signed
  peer certificates are accepted by default, with
  `TlsOptions::allowSelfSignedPeers = false` as the disable option.
- **Emscripten 4.0.7** (`EMSDK` set) for the `wasm` preset.
- **node** with WebSocket support for the WASM transport smoke test — node ≥ 22
  out of the box, node 20/21 via `--experimental-websocket` (the test detects
  the flag; the node bundled with emsdk works). The test skips itself when the
  wasm build or node is unavailable.

## Building

```sh
git submodule update --init --recursive     # fetch pinned dependencies

cmake --preset linux                         # configure (see CMakePresets.json)
cmake --build --preset linux                 # build all libraries + tools + tests
ctest --preset linux                         # run the test suite
```

Presets exist for each official target: `linux`, `macos`, `windows`, `android`,
`ios`, `wasm` (plus `linux-release`, `linux-asan`, `linux-tsan`,
`linux-coverage`, `linux-tls`, `linux-fuzz`). Cross-target presets expect the
relevant toolchain (`ANDROID_NDK`, `EMSDK`, Xcode, …).

Key options (see `cmake/IMOSOptions.cmake`): `IMOS_BUILD_SHARED`,
`IMOS_BUILD_STATIC`, `IMOS_USE_SYSTEM_DEPS`, `IMOS_ENABLE_TLS`,
`IMOS_ENABLE_COMPAT28`, `IMOS_ENABLE_WS3`, `IMOS_BUILD_TESTS`,
`IMOS_BUILD_TOOLS`.

## Spec fixtures

The MOS 4.0 DTD, XSD, and every example message are extracted from the project's
copy of the official specification into [`fixtures/`](fixtures/README.md). The
`imos-fixtures-verify` tool (also a CTest gate) checks them with libxml2.
Regenerate with:

```sh
python3 tools/extract_fixtures.py "reference/MOS Protocol 4.0 (Current).html" fixtures
```

## Repository layout

```
core/        sans-I/O engine: codec, session, profiles, encoding
imos/        device-role API              incs/   NCS-role API
capi/        C ABI shims                  transports/ ws40_ixws / emscripten / tcp28 / ws3_soap
store/       IMessageStore + file backend dialects/  sofie / inews / enps / octopus
plugin/      web-control messages         tools/   fixtures-verify, imos-cli, simulators
tests/       unit / conformance / fuzz    fixtures/  spec DTD, XSD, example messages
examples/    worked examples              cmake/   build helpers and presets
3rdparty/    pinned dependencies          reference/ spec + implementation plan
```

## License

Apache-2.0. See [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt). Vendored
dependencies retain their own licenses (IXWebSocket BSD-3, libxml2 MIT, zlib
License, GoogleTest BSD-3).
