# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Test suite (plan §10). M1: codec round-trip gate over the spec examples,
# encoding engine, deframer spike, session reliability rules (fake clock/
# transport/store), and store crash recovery. Profile conformance suites
# arrive with M2+; fuzz harnesses live under fuzz/ (clang + libFuzzer only).

include(GoogleTest)

add_executable(imos_smoke_test smoke/smoke_test.cpp)
target_link_libraries(imos_smoke_test PRIVATE
  GTest::gtest_main
  imos::core
  imos::imos
  imos::incs
  imos::imos_capi
  imos::warnings)
target_compile_features(imos_smoke_test PRIVATE cxx_std_20)

if(TARGET imos::transports)
  target_link_libraries(imos_smoke_test PRIVATE imos::transports)
  target_compile_definitions(imos_smoke_test PRIVATE IMOS_HAVE_TRANSPORTS=1)
endif()

gtest_discover_tests(imos_smoke_test)

# ---- core unit suites -------------------------------------------------
add_executable(imos_core_test
  codec/roundtrip_test.cpp
  codec/message_test.cpp
  codec/legacy_test.cpp
  encoding/encoding_test.cpp
  framing/deframer_test.cpp
  session/session_test.cpp)
target_link_libraries(imos_core_test PRIVATE
  GTest::gtest_main
  imos::core
  imos::warnings)
target_compile_features(imos_core_test PRIVATE cxx_std_20)
target_compile_definitions(imos_core_test PRIVATE
  IMOS_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/fixtures")
gtest_discover_tests(imos_core_test DISCOVERY_TIMEOUT 30)

add_executable(imos_store_test store/file_store_test.cpp)
target_link_libraries(imos_store_test PRIVATE
  GTest::gtest_main
  imos::store_file
  imos::core
  imos::warnings)
target_compile_features(imos_store_test PRIVATE cxx_std_20)
gtest_discover_tests(imos_store_test DISCOVERY_TIMEOUT 30)

# ---- fuzz harnesses (plan §10 adversarial layer) ----------------------
if(IMOS_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
