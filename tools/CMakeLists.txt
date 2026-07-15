# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Command-line tools (plan §4.3). At M0: fixtures-verify, which doubles as the
# "fixtures verified against spec" gate and as a libxml2 integration smoke.
# imos-cli, incs-sim and mos-sim arrive in later milestones.

add_executable(imos-fixtures-verify fixtures_verify/main.cpp)
target_link_libraries(imos-fixtures-verify PRIVATE LibXml2::LibXml2 imos::warnings)
target_compile_features(imos-fixtures-verify PRIVATE cxx_std_20)
set_target_properties(imos-fixtures-verify PROPERTIES OUTPUT_NAME imos-fixtures-verify)

if(IMOS_BUILD_TESTS)
  add_test(NAME fixtures-verify
    COMMAND imos-fixtures-verify "${PROJECT_SOURCE_DIR}/fixtures")

  # Generated-code freshness gate (plan §4.5): regenerating the message
  # registry/views from the DTD + annotations must reproduce the checked-in
  # files exactly. Mirrors the fixtures-freshness gate in CI.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME codegen-verify
      COMMAND ${CMAKE_COMMAND}
        -DPYTHON=${Python3_EXECUTABLE}
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/codegen-verify
        -P ${CMAKE_CURRENT_SOURCE_DIR}/codegen/verify.cmake)
  endif()
endif()
