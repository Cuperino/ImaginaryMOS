#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Build imaginarymos-<version>-wasm.tar.gz — the WebAssembly packaging artifact
# (plan §9, M7): the static archives (`.a`, with the vendored libxml2/zlib
# folded in), the public headers, a BUILDING note describing the Emscripten
# link line, and a demo page. WASM is static-only and client-only (plan §5.4);
# there is no shared library or transport server here.
#
# Requires the Emscripten SDK (EMSDK). Run from the repo root:
#   packaging/wasm/bundle_wasm.sh [output-dir]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${1:-${REPO_ROOT}/build/wasm-bundle}"
VERSION="$(sed -n 's/^ *VERSION \([0-9.]*\).*/\1/p' "${REPO_ROOT}/CMakeLists.txt" | head -1)"
: "${EMSDK:?set EMSDK to your Emscripten 4.0.7 SDK path}"
TOOLCHAIN="${EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake"

BUILD="${OUT_DIR}/build"
PREFIX="${OUT_DIR}/prefix"
STAGE="${OUT_DIR}/imaginarymos-${VERSION}-wasm"

echo "==> configure + build + install (wasm, static)"
cmake -G Ninja -S "${REPO_ROOT}" -B "${BUILD}" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN}" \
  -DIMOS_BUILD_SHARED=OFF \
  -DIMOS_BUILD_TESTS=OFF \
  -DIMOS_BUILD_TOOLS=OFF \
  -DIMOS_INSTALL_PACKAGE=ON
cmake --build "${BUILD}"
cmake --install "${BUILD}" --prefix "${PREFIX}"

echo "==> assembling bundle"
rm -rf "${STAGE}"
mkdir -p "${STAGE}"
cp -R "${PREFIX}/lib" "${PREFIX}/include" "${STAGE}/"
cp "${REPO_ROOT}/packaging/wasm/demo/index.html" "${STAGE}/"
cp "${REPO_ROOT}/LICENSES/Apache-2.0.txt" "${STAGE}/LICENSE"

cat >"${STAGE}/BUILDING.md" <<MD
# ImaginaryMOS for WebAssembly (${VERSION})

Static, client-only build (Emscripten). The device role talks MOS 4.0 over the
browser WebSocket API via \`imos::transports::Ws40EmscriptenClient\`.

## Link line

Link the static archives into your Emscripten app and pull in the browser
WebSocket glue:

    emcc your_app.cpp \\
      -I include \\
      lib/libimos.a lib/libimos_transports.a lib/libimos_core.a \\
      -lwebsocket.js \\
      -sEXPORTED_RUNTIME_METHODS=ccall,cwrap \\
      -o app.js

\`libimos_core.a\` already contains the folded libxml2 + zlib objects, so no
extra dependency archives are required. See index.html for a minimal page.
MD

TARBALL="${OUT_DIR}/imaginarymos-${VERSION}-wasm.tar.gz"
( cd "${OUT_DIR}" && tar czf "${TARBALL}" "imaginarymos-${VERSION}-wasm" )
echo "==> done: ${TARBALL}"
