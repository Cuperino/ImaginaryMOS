# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.24)

project(ImaginaryMOS
  VERSION 1.0.0
  DESCRIPTION "Portable MOS Protocol v4.0 library suite (imos / incs)"
  HOMEPAGE_URL "https://mosprotocol.com/"
  LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(IMOS_IS_TOP_LEVEL ON)
else()
  set(IMOS_IS_TOP_LEVEL OFF)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Hidden by default with explicit export macros (plan §8.2).
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(IMOSOptions)
include(IMOSWarnings)
include(IMOSAddLibrary)
include(IMOSDependencies)

# ---- first-party libraries -------------------------------------------
add_subdirectory(core)
add_subdirectory(store)
add_subdirectory(imos)
add_subdirectory(incs)
add_subdirectory(capi)
add_subdirectory(transports)

# enable_testing() must precede any add_test(), including the fixtures-verify
# gate registered under tools/.
if(IMOS_BUILD_TESTS)
  enable_testing()
endif()

if(IMOS_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(IMOS_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---- documentation (optional, off by default) ------------------------
# Doxygen API reference + narrative guides (plan §15). Self-gates on
# IMOS_BUILD_DOCS and the presence of doxygen.
add_subdirectory(docs)

# ---- install / package config ----------------------------------------
# Component install rules (libs + headers) are emitted by imos_add_library when
# IMOS_INSTALL is on. The CMake package config + exported target set, which need
# a find_dependency story for the vendored static deps, is an M7 deliverable
# (plan §9 packaging) and is gated off until then.
option(IMOS_INSTALL_PACKAGE "Generate the CMake package config + export set (M7)" OFF)
if(IMOS_INSTALL AND IMOS_INSTALL_PACKAGE)
  include(CMakePackageConfigHelpers)
  include(GNUInstallDirs)
  set(IMOS_CMAKE_CONFIG_DEST "${CMAKE_INSTALL_LIBDIR}/cmake/ImaginaryMOS")
  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ImaginaryMOSConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ImaginaryMOSConfig.cmake"
    INSTALL_DESTINATION "${IMOS_CMAKE_CONFIG_DEST}")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ImaginaryMOSConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ImaginaryMOSConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ImaginaryMOSConfigVersion.cmake"
    DESTINATION "${IMOS_CMAKE_CONFIG_DEST}")
endif()

message(STATUS "ImaginaryMOS ${PROJECT_VERSION} — "
  "shared:${IMOS_BUILD_SHARED} static:${IMOS_BUILD_STATIC} "
  "tls:${IMOS_ENABLE_TLS} system-deps:${IMOS_USE_SYSTEM_DEPS} "
  "tests:${IMOS_BUILD_TESTS}")
