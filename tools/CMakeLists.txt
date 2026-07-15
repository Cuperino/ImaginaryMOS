# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Command-line tools (plan §4.3). At M0: fixtures-verify, which doubles as the
# "fixtures verified against spec" gate and as a libxml2 integration smoke.
# imos-cli, incs-sim and mos-sim arrive in later milestones.

# WASM Profile 0 smoke client (plan §13 M2 exit: "P0 conformance … + WASM
# client"). Built only on the wasm preset; the linux test suite runs it under
# node (with WebSocket support) against a native Ws40Server.
if(EMSCRIPTEN)
  add_executable(imos-ws40-wasm-smoke ws40_wasm_smoke/main.cpp)
  target_link_libraries(imos-ws40-wasm-smoke PRIVATE
    imos::transports imos::core imos::warnings)
  target_compile_features(imos-ws40-wasm-smoke PRIVATE cxx_std_20)
  target_link_options(imos-ws40-wasm-smoke PRIVATE
    "-lwebsocket.js" "-sEXIT_RUNTIME=1" "-sALLOW_MEMORY_GROWTH=1")
endif()

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
