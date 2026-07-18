<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Packaging (plan §9, M7)

Artifacts for every official target. The **desktop** bundles come straight out
of CPack; the **mobile/wasm** bundles are assembled by the scripts here because
they need multiple builds or a platform-specific container format.

Everything is gated behind `-DIMOS_INSTALL_PACKAGE=ON`, which turns on the
exported CMake package, the pkg-config files, the self-contained static
archives, and CPack. See `cmake/IMOSPackaging.cmake` for the machinery.

## What ships

| Target | Command | Artifact |
|---|---|---|
| Linux   | `cmake --preset linux-package && cmake --build --preset linux-package && cpack --preset linux-package` | `imaginarymos-<v>-Linux-<arch>.tar.gz` + `imaginarymos_<v>_<arch>.deb` |
| macOS   | `cmake --preset macos-package && … && cpack --preset macos-package` | `imaginarymos-<v>-Darwin-<arch>.tar.gz` (universal) |
| Windows | `cmake --preset windows-package && … && cpack --preset windows-package` | `imaginarymos-<v>-Windows-<arch>.zip` (DLLs + import libs) |
| iOS/macOS | `packaging/apple/make_xcframework.sh` | `ImaginaryMOS.xcframework(.zip)` |
| Android | `packaging/android/make_aar.sh` | `imaginarymos-<v>.aar` (Prefab) |
| WebAssembly | `packaging/wasm/bundle_wasm.sh` | `imaginarymos-<v>-wasm.tar.gz` (`.a` + headers + demo) |

## The self-contained static archives

By default the pinned libxml2 / zlib / IXWebSocket are **vendored** (not system
packages) and are not installed on their own. So that a static consumer needs
no `find_dependency` on them, their objects are **folded into the imos static
archives at install time** (a "fat archive"): `libimos_core.a` absorbs libxml2 +
zlib, `libimos_transports.a` absorbs IXWebSocket. Only genuinely-system
transitive libraries remain (threads, libm, and OpenSSL under TLS), resolved by
`ImaginaryMOSDependencies.cmake`. Building with `-DIMOS_USE_SYSTEM_DEPS=ON`
instead skips the fold and emits `find_dependency(LibXml2/ZLIB/ixwebsocket)`.

The merge is portable across toolchains via `cmake/imos_merge_archives.cmake`
(GNU/LLVM `ar` MRI · Apple `libtool` · MSVC `lib`).

## Consuming the CMake package

```cmake
find_package(ImaginaryMOS CONFIG REQUIRED)
target_link_libraries(app PRIVATE imos::imos imos::incs imos::transports)
```

`imos::<lib>` resolves to the shared library when present, else static; the
explicit `imos::<lib>-shared` / `imos::<lib>-static` targets are also exported.
`tests/packaging/consumer/` is the clean project the install-and-link smoke test
(`ctest -R Packaging.ConsumerSmoke`) drives against a throwaway install prefix.

## pkg-config

One `.pc` per public library in `lib/pkgconfig` (`imos_core`, `imos`, `incs`,
`imos_store_file`, `imos_transports`), with `Requires` chained to
the base engine and `Libs.private` carrying the folded system libraries for the
static case.
