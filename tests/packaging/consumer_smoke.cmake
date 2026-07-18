# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Install-and-link smoke test (plan §9 / M7 exit criterion). Installs the built
# tree into a throwaway prefix, then configures, builds, and runs the clean
# consumer project (tests/packaging/consumer) against it — once per requested
# link style. Any failure aborts with a fatal error, which ctest reports.
#
# Invoked from CMake (see tests/CMakeLists.txt) as:
#   cmake -DIMOS_BUILD_DIR=<build> -DIMOS_CONSUMER_SRC=<dir> -DIMOS_WORK=<dir>
#         -DIMOS_GENERATOR=<gen> -DIMOS_LINK_STYLES=shared;static
#         [-DIMOS_BUILD_TYPE=<cfg>] [-DIMOS_TOOLCHAIN=<file>]
#         -P consumer_smoke.cmake

foreach(_req IMOS_BUILD_DIR IMOS_CONSUMER_SRC IMOS_WORK IMOS_GENERATOR IMOS_LINK_STYLES)
  if(NOT DEFINED ${_req})
    message(FATAL_ERROR "consumer_smoke: ${_req} is required")
  endif()
endforeach()
if(NOT DEFINED IMOS_BUILD_TYPE)
  set(IMOS_BUILD_TYPE RelWithDebInfo)
endif()

set(_prefix "${IMOS_WORK}/prefix")
file(REMOVE_RECURSE "${IMOS_WORK}")
file(MAKE_DIRECTORY "${_prefix}")

function(_run_step _desc)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "consumer_smoke: ${_desc} failed (rc=${_rc})")
  endif()
endfunction()

# 1. Install the built tree into the throwaway prefix (folds the vendored deps
#    into the static archives on the way in).
message(STATUS "consumer_smoke: installing into ${_prefix}")
_run_step("install"
  "${CMAKE_COMMAND}" --install "${IMOS_BUILD_DIR}"
  --prefix "${_prefix}" --config "${IMOS_BUILD_TYPE}")

# Runtime loader path for the shared-linked binary (no install rpath is baked
# into the consumer).
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(_ld_env "DYLD_LIBRARY_PATH=${_prefix}/lib")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  set(_ld_env "PATH=${_prefix}/bin;${_prefix}/lib;$ENV{PATH}")
else()
  set(_ld_env "LD_LIBRARY_PATH=${_prefix}/lib")
endif()

# 2. For each link style: configure, build, and run the consumer.
foreach(_style IN LISTS IMOS_LINK_STYLES)
  set(_cbuild "${IMOS_WORK}/consumer-${_style}")
  message(STATUS "consumer_smoke: [${_style}] configuring consumer")

  set(_configure_args
    "${CMAKE_COMMAND}" -G "${IMOS_GENERATOR}"
    -S "${IMOS_CONSUMER_SRC}" -B "${_cbuild}"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DCMAKE_BUILD_TYPE=${IMOS_BUILD_TYPE}"
    "-DIMOS_CONSUMER_LINK=${_style}"
    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5")
  if(IMOS_TOOLCHAIN)
    list(APPEND _configure_args "-DCMAKE_TOOLCHAIN_FILE=${IMOS_TOOLCHAIN}")
  endif()
  _run_step("[${_style}] configure" ${_configure_args})

  message(STATUS "consumer_smoke: [${_style}] building consumer")
  _run_step("[${_style}] build"
    "${CMAKE_COMMAND}" --build "${_cbuild}" --config "${IMOS_BUILD_TYPE}")

  # Locate the built binary (single- and multi-config layouts).
  set(_exe "${_cbuild}/imos_consumer${CMAKE_EXECUTABLE_SUFFIX}")
  if(NOT EXISTS "${_exe}")
    file(GLOB_RECURSE _found "${_cbuild}/imos_consumer*")
    list(FILTER _found EXCLUDE REGEX "\\.(o|obj|ilk|pdb)$")
    if(_found)
      list(GET _found 0 _exe)
    endif()
  endif()
  if(NOT EXISTS "${_exe}")
    message(FATAL_ERROR "consumer_smoke: [${_style}] built binary not found in ${_cbuild}")
  endif()

  message(STATUS "consumer_smoke: [${_style}] running ${_exe}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "${_ld_env}" "${_exe}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  message(STATUS "consumer_smoke: [${_style}] output:\n${_out}")
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "consumer_smoke: [${_style}] run failed (rc=${_rc})\n${_err}")
  endif()
  if(_out MATCHES "MISSING")
    message(FATAL_ERROR "consumer_smoke: [${_style}] a library failed to link:\n${_out}")
  endif()
endforeach()

message(STATUS "consumer_smoke: OK (${IMOS_LINK_STYLES})")
