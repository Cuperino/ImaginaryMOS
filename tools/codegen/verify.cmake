# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# codegen-verify test script: regenerate the message registry/views into a
# scratch tree and diff against the checked-in files under core/.

set(_gen "${SOURCE_DIR}/tools/codegen/gen_messages.py")
set(_dtd "${SOURCE_DIR}/fixtures/schema/mos4.dtd")
set(_ann "${SOURCE_DIR}/tools/codegen/annotations.json")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

execute_process(
  COMMAND "${PYTHON}" "${_gen}" "${_dtd}" "${_ann}" "${WORK_DIR}"
  RESULT_VARIABLE _result OUTPUT_QUIET)
if(NOT _result EQUAL 0)
  message(FATAL_ERROR "gen_messages.py failed (${_result})")
endif()

foreach(_file
    include/imos/codec/gen/message_types.h
    include/imos/codec/gen/views.h
    codec/gen/message_types.cpp)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E compare_files
      "${WORK_DIR}/${_file}" "${SOURCE_DIR}/core/${_file}"
    RESULT_VARIABLE _diff)
  if(NOT _diff EQUAL 0)
    message(FATAL_ERROR
      "generated file is stale: core/${_file}\n"
      "regenerate with: python3 tools/codegen/gen_messages.py "
      "fixtures/schema/mos4.dtd tools/codegen/annotations.json core")
  endif()
endforeach()

message(STATUS "codegen-verify: generated code is in sync")
