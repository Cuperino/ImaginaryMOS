<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# ImaginaryMOS coding standards

These standards keep one codebase honestly portable across the six official
targets (Linux, macOS, Windows, Android, iOS, WebAssembly) and safe to embed
inside third-party MOS devices. They are derived from the architecture in the
[implementation plan](../reference/imaginarymos-implementation-plan.md) §4 and §8.

## Language and toolchain

- **C++20**, no compiler extensions (`CMAKE_CXX_EXTENSIONS OFF`).
- Supported toolchains: GCC 14+, Clang/Apple Clang, MSVC v143+, Emscripten
  (pinned 4.0.7). Code must compile warning-clean under `-Wall -Wextra
  -Wpedantic` / `/W4` (see `cmake/IMOSWarnings.cmake`).
- CMake ≥ 3.24 with presets. Never hard-code paths; drive everything through
  targets and usage requirements.

## Architecture rules (non-negotiable)

- **Sans-I/O core.** Nothing under `core/` may open a socket, spawn a thread,
  read a clock, or touch the filesystem. Side effects arrive through injected
  interfaces (`ITransport`, `IClock`, `ITimerQueue`, `IRandom`, `ILogger`,
  `IMessageStore`). This is what lets GoogleTest drive every profile
  deterministically and what makes the WebAssembly build possible.
- **No exceptions across the public C++ API.** Fallible operations return
  `Result<T, Error>`; exceptions and RTTI stay confined to module internals.
- **Lossless unknown-content retention.** The codec preserves unknown
  elements/attributes and re-emits them (spec's unknown-tag rule + dialect
  passthrough). Opaque `mosPayload` is never re-normalized by default.
- **Embedded-first.** Default consumption is a pull-mode event pump; the
  library creates no threads of its own in that mode. Mind footprint on the
  static targets (WASM/iOS/Android).

## Symbol visibility and ABI

- Hidden by default (`-fvisibility=hidden`) with explicit export macros from
  the generated `<component>_export.h` (e.g. `IMOS_CORE_API`). Add new public
  symbols with the export macro; never widen visibility globally.
- The C++ API is source-stable within a major version. There is no binary
  (ABI) stability guarantee — consumers embed the C++ libraries and rebuild
  against a matching toolchain.

## Formatting and naming

- Formatting is enforced by **`.clang-format`** (Google base, 4-space indent,
  100-column). Run `clang-format -i` before committing; CI checks it.
- Static analysis via **`.clang-tidy`** (informational at M0).
- Namespaces `imos::` and `incs::`. Types `PascalCase`, functions/variables
  `camelCase`, constants `kPascalCase`, macros `IMOS_UPPER_SNAKE`.
- Include order: the module's own header first, then a blank line, then C/C++
  standard headers, then third-party, then project headers (`IncludeBlocks:
  Preserve` keeps deliberate grouping).

## Licensing and headers

- Apache-2.0. Every source file starts with an SPDX header:
  `SPDX-FileCopyrightText` + `SPDX-License-Identifier: Apache-2.0`.
- Vendored code under `3rdparty/` keeps its own license and conventions and is
  exempt from these rules.

## Testing

- GoogleTest / GoogleMock. Unit tests are deterministic (fake clock/transport/
  store). Codec tests round-trip the spec examples in `fixtures/`. Profile
  conformance suites run both roles over every applicable transport and emit
  the machine-readable conformance matrix. See plan §10.
- New public behavior ships with tests in the same change.
