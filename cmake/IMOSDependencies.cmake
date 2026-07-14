# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Dependency acquisition. Default: build the pinned 3rdparty submodules
# (libxml2 minimal feature set, zlib, IXWebSocket). Escape hatch:
# IMOS_USE_SYSTEM_DEPS resolves them with find_package (plan §9).

include_guard(GLOBAL)

set(IMOS_3RDPARTY "${PROJECT_SOURCE_DIR}/3rdparty")

# Some pinned dependencies (zlib 1.2.x) register their own CTest cases via
# add_subdirectory (example / example64 / minigzip). Filter those out of our
# test set while forwarding every first-party add_test() call unchanged.
macro(add_test)
  if("${ARGV0}" MATCHES "^(example|example64|minigzip|minigzip64)$")
    # vendored dependency self-test: skip
  else()
    _add_test(${ARGV})
  endif()
endmacro()

# The pinned submodules declare cmake_minimum_required below CMake 3.5, which
# CMake 4.x rejects unless we raise the floor for them.
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

function(_imos_require_submodule dir name)
  if(NOT EXISTS "${dir}/CMakeLists.txt")
    message(FATAL_ERROR
      "Pinned dependency '${name}' is missing at ${dir}.\n"
      "Run:  git submodule update --init --recursive\n"
      "or configure with -DIMOS_USE_SYSTEM_DEPS=ON to use system packages.")
  endif()
endfunction()

if(IMOS_USE_SYSTEM_DEPS)
  find_package(LibXml2 REQUIRED)
  find_package(ZLIB REQUIRED)
  find_package(ixwebsocket CONFIG QUIET)
  if(NOT ixwebsocket_FOUND AND NOT TARGET ixwebsocket::ixwebsocket)
    message(WARNING "System IXWebSocket not found; transports will be limited")
  endif()
else()
  # Build dependencies as static libraries regardless of IMOS_BUILD_SHARED.
  set(_imos_saved_shared ${BUILD_SHARED_LIBS})
  set(BUILD_SHARED_LIBS OFF)

  # -- zlib -------------------------------------------------------------
  _imos_require_submodule("${IMOS_3RDPARTY}/zlib" zlib)
  set(SKIP_INSTALL_ALL ON) # keep vendored zlib out of our install tree
  add_subdirectory("${IMOS_3RDPARTY}/zlib" EXCLUDE_FROM_ALL)
  # The 1.2.x CMake build does not attach usage requirements to zlibstatic.
  target_include_directories(zlibstatic PUBLIC
    "$<BUILD_INTERFACE:${IMOS_3RDPARTY}/zlib>"
    "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/3rdparty/zlib>")
  # Expose ZLIB::ZLIB so IXWebSocket's find_package(ZLIB) resolves to the pin.
  list(PREPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/shims")

  # -- libxml2 (minimal footprint, plan §4.1) ---------------------------
  _imos_require_submodule("${IMOS_3RDPARTY}/libxml2" libxml2)
  set(LIBXML2_WITH_HTTP    OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_CATALOG OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_MODULES OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_PYTHON  OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_TESTS   OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_DOCS    OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_ICU     OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_ICONV   OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_LZMA    OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_ZLIB    OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_LEGACY  OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_READLINE OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_TLS     OFF CACHE BOOL "" FORCE)
  set(LIBXML2_WITH_XINCLUDE OFF CACHE BOOL "" FORCE)
  # Kept ON: OUTPUT, PATTERN, PUSH, REGEXPS, SAX1, THREADS, VALID (DTD),
  # XPATH (needed for mosReqObjList translation, plan §3.2).
  add_subdirectory("${IMOS_3RDPARTY}/libxml2" EXCLUDE_FROM_ALL)

  # -- IXWebSocket ------------------------------------------------------
  _imos_require_submodule("${IMOS_3RDPARTY}/IXWebSocket" IXWebSocket)
  set(USE_TLS  ${IMOS_ENABLE_TLS} CACHE BOOL "" FORCE)
  set(USE_ZLIB ON  CACHE BOOL "" FORCE)
  set(USE_WS   OFF CACHE BOOL "" FORCE)  # the `ws` CLI tool
  set(USE_TEST OFF CACHE BOOL "" FORCE)
  set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory("${IMOS_3RDPARTY}/IXWebSocket" EXCLUDE_FROM_ALL)

  set(BUILD_SHARED_LIBS ${_imos_saved_shared})
endif()

# Normalize target names so first-party code links the same names either way.
if(NOT TARGET LibXml2::LibXml2 AND TARGET LibXml2)
  add_library(LibXml2::LibXml2 ALIAS LibXml2)
endif()
if(NOT TARGET ZLIB::ZLIB AND TARGET zlibstatic)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()

# -- GoogleTest (pinned via FetchContent, plan §9/§10) ------------------
if(IMOS_BUILD_TESTS)
  if(IMOS_USE_SYSTEM_DEPS)
    find_package(GTest CONFIG QUIET)
  endif()
  if(NOT TARGET GTest::gtest)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.15.2         # pinned
      GIT_SHALLOW    TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
endif()
