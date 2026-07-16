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
  target_compile_definitions(imos_smoke_test PRIVATE
    IMOS_HAVE_TRANSPORTS=1
    IMOS_TLS_EXPECTED=$<BOOL:${IMOS_ENABLE_TLS}>)
endif()

gtest_discover_tests(imos_smoke_test)

# ---- core unit suites -------------------------------------------------
add_executable(imos_core_test
  codec/roundtrip_test.cpp
  codec/message_test.cpp
  codec/legacy_test.cpp
  encoding/encoding_test.cpp
  framing/deframer_test.cpp
  profiles/profile0_test.cpp
  profiles/running_order_model_test.cpp
  session/reconnect_test.cpp
  session/session_test.cpp
  ws40/url_test.cpp)
target_link_libraries(imos_core_test PRIVATE
  GTest::gtest_main
  imos::core
  imos::warnings)
target_compile_features(imos_core_test PRIVATE cxx_std_20)
target_compile_definitions(imos_core_test PRIVATE
  IMOS_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/fixtures")
gtest_discover_tests(imos_core_test DISCOVERY_TIMEOUT 30)

# ---- ws40 transport integration (loopback WebSocket, plan §13 M2) -----
if(TARGET imos::transports AND NOT EMSCRIPTEN)
  add_executable(imos_transport_test transports/ws40_transport_test.cpp)
  target_link_libraries(imos_transport_test PRIVATE
    GTest::gtest_main
    imos::transports
    imos::core
    imos::imos
    imos::incs
    ixwebsocket::ixwebsocket   # ix::getFreePort in the test rig
    imos::warnings)
  target_compile_features(imos_transport_test PRIVATE cxx_std_20)
  target_compile_definitions(imos_transport_test PRIVATE
    IMOS_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  gtest_discover_tests(imos_transport_test DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 60)
endif()

# ---- profile conformance suites (real imos vs real incs, plan §10) ----
add_executable(imos_profile_test
  profiles/profile1_test.cpp
  profiles/profile2_test.cpp)
target_link_libraries(imos_profile_test PRIVATE
  GTest::gtest_main
  imos::core
  imos::imos
  imos::incs
  imos::store_file
  imos::warnings)
target_compile_features(imos_profile_test PRIVATE cxx_std_20)
target_include_directories(imos_profile_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_definitions(imos_profile_test PRIVATE
  IMOS_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/fixtures")
gtest_discover_tests(imos_profile_test DISCOVERY_TIMEOUT 30)

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
