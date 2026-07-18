#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Build ImaginaryMOS.xcframework — the Apple packaging artifact (plan §9, M7).
# Produces a static XCFramework with one self-contained library per slice
# (device iOS-arm64, simulator arm64 + x86_64) plus the public headers. Each
# slice is a configure → build → install of the static, packaging-enabled tree
# (so the vendored libxml2/zlib/IXWebSocket are folded into the archives), whose
# per-component archives are then merged into a single libImaginaryMOS.a with
# Apple's libtool.
#
# Requires macOS with Xcode. Run from the repository root:
#   packaging/apple/make_xcframework.sh [output-dir]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${1:-${REPO_ROOT}/build/apple}"
VERSION="$(sed -n 's/^ *VERSION \([0-9.]*\).*/\1/p' "${REPO_ROOT}/CMakeLists.txt" | head -1)"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "error: the XCFramework can only be built on macOS with Xcode" >&2
  exit 1
fi

# Public libraries, base engine last so libtool sees the fat core once.
COMPONENTS=(imos incs imos_transports imos_store_file imos_core)

# slice name | CMAKE_OSX_SYSROOT | architectures
SLICES=(
  "ios-arm64|iphoneos|arm64"
  "ios-sim|iphonesimulator|arm64;x86_64"
)

build_slice() {
  local name="$1" sysroot="$2" arches="$3"
  local build="${OUT_DIR}/${name}/build"
  local prefix="${OUT_DIR}/${name}/prefix"
  echo "==> [${name}] configure + build + install (sysroot=${sysroot}, arch=${arches})"
  cmake -G Ninja -S "${REPO_ROOT}" -B "${build}" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="${sysroot}" \
    -DCMAKE_OSX_ARCHITECTURES="${arches}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 \
    -DIMOS_BUILD_SHARED=OFF \
    -DIMOS_BUILD_TESTS=OFF \
    -DIMOS_BUILD_TOOLS=OFF \
    -DIMOS_INSTALL_PACKAGE=ON
  cmake --build "${build}"
  cmake --install "${build}" --prefix "${prefix}"

  # Merge the per-component archives into one self-contained static library.
  local libs=()
  local c
  for c in "${COMPONENTS[@]}"; do
    [[ -f "${prefix}/lib/lib${c}.a" ]] && libs+=("${prefix}/lib/lib${c}.a")
  done
  libtool -static -o "${prefix}/libImaginaryMOS.a" "${libs[@]}"
  echo "    merged $(printf '%s ' "${COMPONENTS[@]}")-> libImaginaryMOS.a"
}

XCARGS=()
for slice in "${SLICES[@]}"; do
  IFS='|' read -r name sysroot arches <<<"${slice}"
  build_slice "${name}" "${sysroot}" "${arches}"
  XCARGS+=(-library "${OUT_DIR}/${name}/prefix/libImaginaryMOS.a"
           -headers "${OUT_DIR}/${name}/prefix/include")
done

FRAMEWORK="${OUT_DIR}/ImaginaryMOS.xcframework"
rm -rf "${FRAMEWORK}"
echo "==> creating ${FRAMEWORK}"
xcodebuild -create-xcframework "${XCARGS[@]}" -output "${FRAMEWORK}"

# Zip it for distribution.
( cd "${OUT_DIR}" && zip -qry "ImaginaryMOS-${VERSION}.xcframework.zip" \
    "ImaginaryMOS.xcframework" )
echo "==> done: ${OUT_DIR}/ImaginaryMOS-${VERSION}.xcframework.zip"
