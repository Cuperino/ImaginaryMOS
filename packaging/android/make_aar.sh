#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Build imaginarymos-<version>.aar — the Android packaging artifact (plan §9,
# M7), a Prefab AAR (schema v2) of the shared role libraries for the pinned
# arm64-v8a ABI (override with IMOS_ANDROID_ABIS="arm64-v8a;x86_64"). Each
# shared library becomes a Prefab module; the vendored deps are absorbed into
# the .so at link time, so nothing external has to travel in the AAR.
#
# Requires the Android NDK (ANDROID_NDK) and `zip`. Run from the repo root:
#   packaging/android/make_aar.sh [output-dir]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${1:-${REPO_ROOT}/build/android-aar}"
VERSION="$(sed -n 's/^ *VERSION \([0-9.]*\).*/\1/p' "${REPO_ROOT}/CMakeLists.txt" | head -1)"
ANDROID_PLATFORM="${IMOS_ANDROID_PLATFORM:-android-28}"
IFS=';' read -r -a ABIS <<<"${IMOS_ANDROID_ABIS:-arm64-v8a}"
: "${ANDROID_NDK:?set ANDROID_NDK to your NDK r26+ path}"

# module | .so basename | intra-package dependency (Prefab ':' reference)
MODULES=(
  "imos_core|libimos_core|"
  "imos|libimos|:imos_core"
  "incs|libincs|:imos_core"
  "imos_store_file|libimos_store_file|:imos_core"
  "imos_transports|libimos_transports|:imos_core"
  "imos_capi|libimos_capi|:imos"
)

STAGE="${OUT_DIR}/aar"
rm -rf "${STAGE}"
mkdir -p "${STAGE}/prefab/modules"

for abi in "${ABIS[@]}"; do
  build="${OUT_DIR}/${abi}/build"
  prefix="${OUT_DIR}/${abi}/prefix"
  echo "==> [${abi}] configure + build + install"
  cmake -G Ninja -S "${REPO_ROOT}" -B "${build}" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="${abi}" \
    -DANDROID_PLATFORM="${ANDROID_PLATFORM}" \
    -DIMOS_BUILD_SHARED=ON \
    -DIMOS_BUILD_STATIC=OFF \
    -DIMOS_BUILD_TESTS=OFF \
    -DIMOS_BUILD_TOOLS=OFF \
    -DIMOS_INSTALL_PACKAGE=ON
  cmake --build "${build}"
  cmake --install "${build}" --prefix "${prefix}"

  # Drop each module's .so into its per-ABI libs dir, alongside its abi.json.
  for entry in "${MODULES[@]}"; do
    IFS='|' read -r mod soname _dep <<<"${entry}"
    abidir="${STAGE}/prefab/modules/${mod}/libs/android.${abi}"
    mkdir -p "${abidir}"
    # Install ships versioned .so + symlinks; ship the real file as lib<mod>.so.
    cp -L "${prefix}/lib/${soname}.so" "${abidir}/${soname}.so"
    cat >"${abidir}/abi.json" <<JSON
{ "abi": "${abi}", "api": ${ANDROID_PLATFORM#android-}, "ndk": 26, "stl": "c++_shared" }
JSON
  done
  LAST_PREFIX="${prefix}"
done

# Headers (ABI-agnostic) + module.json per module. Headers are duplicated into
# each module so a consumer that pulls a single module still compiles.
for entry in "${MODULES[@]}"; do
  IFS='|' read -r mod soname dep <<<"${entry}"
  moddir="${STAGE}/prefab/modules/${mod}"
  mkdir -p "${moddir}/include"
  cp -R "${LAST_PREFIX}/include/." "${moddir}/include/"
  deps="\"-llog\", \"-landroid\""
  [[ -n "${dep}" ]] && deps="\"${dep}\", ${deps}"
  cat >"${moddir}/module.json" <<JSON
{
  "export_libraries": [${deps}],
  "library_name": "${soname}"
}
JSON
done

cat >"${STAGE}/prefab/prefab.json" <<JSON
{
  "schema_version": 2,
  "name": "imaginarymos",
  "version": "${VERSION}",
  "dependencies": []
}
JSON

cat >"${STAGE}/AndroidManifest.xml" <<XML
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
    package="com.mosprotocol.imaginarymos">
    <uses-sdk android:minSdkVersion="28" />
</manifest>
XML

AAR="${OUT_DIR}/imaginarymos-${VERSION}.aar"
rm -f "${AAR}"
( cd "${STAGE}" && zip -qry "${AAR}" . )
echo "==> done: ${AAR} (ABIs: ${ABIS[*]})"
