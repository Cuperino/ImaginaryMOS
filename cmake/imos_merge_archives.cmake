# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Fold one or more input static archives into an output static archive, in
# place, so the result is self-contained (plan §9 packaging: installed static
# libraries carry the vendored deps rather than exporting a find_dependency on
# them). Run as a script from a POST_BUILD step:
#
#   cmake -DIMOS_MERGE_OUTPUT=<archive>
#         -DIMOS_MERGE_INPUTS=<a>;<b>;...
#         -DIMOS_MERGE_TOOL=gnuar|libtool|msvc
#         [-DIMOS_MERGE_AR=<ar>] [-DIMOS_MERGE_LIBTOOL=<libtool>]
#         [-DIMOS_MERGE_RANLIB=<ranlib>]
#         -P cmake/imos_merge_archives.cmake
#
# The three tool families cover every official target: GNU/LLVM ar (Linux,
# Android, WebAssembly via emar), Apple libtool (macOS/iOS), MSVC lib (Windows).

if(NOT IMOS_MERGE_OUTPUT OR NOT IMOS_MERGE_INPUTS OR NOT IMOS_MERGE_TOOL)
  message(FATAL_ERROR "imos_merge_archives: OUTPUT, INPUTS and TOOL are required")
endif()
if(NOT EXISTS "${IMOS_MERGE_OUTPUT}")
  message(FATAL_ERROR "imos_merge_archives: output archive '${IMOS_MERGE_OUTPUT}' does not exist")
endif()

# Preserve the freshly-built archive under .orig and rebuild OUTPUT from it plus
# the inputs. Rebuilding from .orig (rather than editing in place) keeps the
# step idempotent across incremental relinks.
set(_orig "${IMOS_MERGE_OUTPUT}.orig")
file(REMOVE "${_orig}")
file(RENAME "${IMOS_MERGE_OUTPUT}" "${_orig}")

if(IMOS_MERGE_TOOL STREQUAL "gnuar")
  # GNU ar / llvm-ar MRI script. ADDLIB pulls in every member of an archive.
  set(_mri "CREATE ${IMOS_MERGE_OUTPUT}\nADDLIB ${_orig}\n")
  foreach(_in IN LISTS IMOS_MERGE_INPUTS)
    string(APPEND _mri "ADDLIB ${_in}\n")
  endforeach()
  string(APPEND _mri "SAVE\nEND\n")
  set(_script "${IMOS_MERGE_OUTPUT}.mri")
  file(WRITE "${_script}" "${_mri}")
  execute_process(
    COMMAND "${IMOS_MERGE_AR}" -M
    INPUT_FILE "${_script}"
    RESULT_VARIABLE _rc
    ERROR_VARIABLE _err)
  file(REMOVE "${_script}")
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "imos_merge_archives(ar -M) failed: ${_err}")
  endif()
  if(IMOS_MERGE_RANLIB)
    execute_process(COMMAND "${IMOS_MERGE_RANLIB}" "${IMOS_MERGE_OUTPUT}")
  endif()

elseif(IMOS_MERGE_TOOL STREQUAL "libtool")
  execute_process(
    COMMAND "${IMOS_MERGE_LIBTOOL}" -static -o "${IMOS_MERGE_OUTPUT}"
            "${_orig}" ${IMOS_MERGE_INPUTS}
    RESULT_VARIABLE _rc
    ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "imos_merge_archives(libtool) failed: ${_err}")
  endif()

elseif(IMOS_MERGE_TOOL STREQUAL "msvc")
  # lib.exe / llvm-lib: /OUT combines the listed .lib files.
  execute_process(
    COMMAND "${IMOS_MERGE_AR}" /NOLOGO "/OUT:${IMOS_MERGE_OUTPUT}"
            "${_orig}" ${IMOS_MERGE_INPUTS}
    RESULT_VARIABLE _rc
    ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "imos_merge_archives(lib) failed: ${_err}")
  endif()

else()
  file(RENAME "${_orig}" "${IMOS_MERGE_OUTPUT}") # restore before bailing out
  message(FATAL_ERROR "imos_merge_archives: unknown TOOL '${IMOS_MERGE_TOOL}'")
endif()

file(REMOVE "${_orig}")
