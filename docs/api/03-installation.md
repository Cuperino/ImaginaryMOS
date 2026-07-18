<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Installation & packaging {#installation}

[TOC]

The @ref getting_started "getting-started guide" builds ImaginaryMOS from the
tree. This page covers the **installed** form: the CMake package, pkg-config
files, and the per-platform bundles produced for release (plan §9).

## Building the package {#inst_build}

Packaging is gated behind one option, wired up by the `*-package` presets:

```sh
cmake --preset linux-package        # -DIMOS_INSTALL_PACKAGE=ON
cmake --build --preset linux-package
ctest --preset linux-package -R Packaging.ConsumerSmoke   # install-and-link gate
cpack --preset linux-package        # tarball + .deb (zip on Windows)
```

`-DIMOS_INSTALL_PACKAGE=ON` turns on the exported CMake package, the pkg-config
files, self-contained static archives, and CPack. macOS and Windows have
matching `macos-package` / `windows-package` presets.

## Consuming with CMake {#inst_cmake}

```cmake
find_package(ImaginaryMOS CONFIG REQUIRED)
target_link_libraries(app PRIVATE imos::imos imos::incs imos::transports)
```

`imos::<lib>` resolves to the shared library when the install has one, else the
static library. The link-style-explicit `imos::<lib>-shared` and
`imos::<lib>-static` targets are exported too. Available libraries: `imos`,
`incs`, `core`, `store_file`, `transports`.

`ImaginaryMOSConfig.cmake` pulls in `ImaginaryMOSDependencies.cmake`, which
resolves whatever the export still references — for a default (vendored) install
that is just `Threads` (and `OpenSSL` when built with TLS).

## Self-contained static archives {#inst_fat}

By default the pinned libxml2 / zlib / IXWebSocket are vendored and are **not**
installed separately. Their objects are folded into the imos static archives at
install time (a "fat archive"): `libimos_core.a` absorbs libxml2 + zlib,
`libimos_transports.a` absorbs IXWebSocket. A static consumer therefore needs no
`find_dependency` on them — only the genuinely-system transitive libraries
(threads, libm, OpenSSL) remain. Configuring with `-DIMOS_USE_SYSTEM_DEPS=ON`
instead skips the fold and emits `find_dependency(LibXml2/ZLIB/ixwebsocket)`.

## pkg-config {#inst_pkgconfig}

One `.pc` per public library lands in `lib/pkgconfig`:

```sh
pkg-config --cflags --libs --static imos_transports
```

`Requires` chains each library to the core engine; `Libs.private` carries the
folded system libraries for static linking.

## Platform bundles {#inst_bundles}

| Target | Producer | Artifact |
|---|---|---|
| Linux / macOS | `cpack --preset …` | `.tar.gz` (Linux also `.deb`) |
| Windows | `cpack --preset windows-package` | `.zip` (DLLs + import libs) |
| iOS / macOS | `packaging/apple/make_xcframework.sh` | `ImaginaryMOS.xcframework` |
| Android | `packaging/android/make_aar.sh` | Prefab `.aar` |
| WebAssembly | `packaging/wasm/bundle_wasm.sh` | `.a` + headers + demo page |

See `packaging/README.md` for details on each bundle.
