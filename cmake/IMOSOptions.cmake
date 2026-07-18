# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Project-wide build options (plan §9).

include_guard(GLOBAL)

option(IMOS_BUILD_SHARED "Build shared (.so/.dylib/.dll) libraries" ON)
option(IMOS_BUILD_STATIC "Build static (.a/.lib) libraries" ON)

option(IMOS_USE_SYSTEM_DEPS
  "Resolve libxml2/zlib/IXWebSocket with find_package instead of the pinned \
3rdparty submodules" OFF)

# Optional/compat modules. Off at M0; scheduled by later milestones.
option(IMOS_ENABLE_COMPAT28   "Build the MOS 2.8.x TCP transport (M6)"        OFF)
option(IMOS_ENABLE_WS3        "Build the MOS 3.x WebService transport (M6)"   OFF)
option(IMOS_ENABLE_PLUGIN_MSGS "Build the web-control plug-in messages (deferred)" OFF)

# Transport security. TLS/auth/deflate land in M2; off keeps M0 free of an
# OpenSSL dependency while still exercising the IXWebSocket build.
option(IMOS_ENABLE_TLS "Enable TLS (OpenSSL) in the WebSocket transport" OFF)

option(IMOS_BUILD_TESTS "Build the test suite (GoogleTest)" ${IMOS_IS_TOP_LEVEL})
option(IMOS_BUILD_TOOLS "Build the tools (fixtures-verify, imos-cli, …)" ${IMOS_IS_TOP_LEVEL})
option(IMOS_INSTALL     "Generate install/export rules" ${IMOS_IS_TOP_LEVEL})

# Packaging (plan §9, M7). Generates the CMake package config + exported target
# set, the pkg-config files, self-contained static archives (the vendored deps
# are folded in — see cmake/IMOSPackaging.cmake), and CPack metadata. Defined
# here (rather than at the top level) so imos_add_library can route the vendored
# deps out of the install interface while the package build is active.
option(IMOS_INSTALL_PACKAGE
  "Generate the CMake package config + export set, pkg-config, and CPack (M7)" OFF)
option(IMOS_BUILD_PACKAGING_TESTS
  "Register the install-and-link consumer smoke test (needs IMOS_INSTALL_PACKAGE)" OFF)

option(IMOS_BUILD_FUZZERS "Build the libFuzzer harnesses (Clang only, M1+)" OFF)

option(IMOS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(IMOS_ENABLE_ASAN "Build with AddressSanitizer + UBSan" OFF)
option(IMOS_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(IMOS_ENABLE_COVERAGE "Build with coverage instrumentation" OFF)

if(IMOS_ENABLE_ASAN AND IMOS_ENABLE_TSAN)
  message(FATAL_ERROR "IMOS_ENABLE_ASAN and IMOS_ENABLE_TSAN are mutually exclusive")
endif()

# WebAssembly cannot host raw TCP or a SOAP/HTTP server; enforce the support
# matrix (plan §5.4) at configure time.
if(EMSCRIPTEN)
  if(IMOS_ENABLE_COMPAT28 OR IMOS_ENABLE_WS3)
    message(FATAL_ERROR
      "MOS 2.8 (TCP) and MOS 3.x (WebService) are unsupported on WebAssembly")
  endif()
  set(IMOS_BUILD_SHARED OFF CACHE BOOL "" FORCE) # static-only on wasm
endif()
