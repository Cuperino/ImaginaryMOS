# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Test suite (plan §10). M1: codec round-trip gate over the spec examples,
# encoding engine, deframer spike, session reliability rules (fake clock/
# transport/store), and store crash recovery. Profile conformance suites
# arrive with M2+; fuzz harnesses live under fuzz/ (clang + libFuzzer only).

include(GoogleTest)

# Enumerate tests at ctest time, not during the build (plan §10). The default
# POST_BUILD mode runs each freshly-linked binary as a build step to list its
# tests; for imos_transport_test — the one suite linking the real IXWebSocket
# + threading stack — that startup occasionally yields an empty list under a
# loaded CI runner, which newer CMake's JSON discovery parser rejects and
# fails the build. PRE_TEST defers discovery to test run time, so a transient
# listing hiccup can never break the build.
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

add_executable(imos_smoke_test smoke/smoke_test.cpp)
target_link_libraries(imos_smoke_test PRIVATE
  GTest::gtest_main
  imos::core
  imos::imos
  imos::incs
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
  codec/story_body_test.cpp
  codec/fqid_test.cpp
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

# ---- transport integration (loopback sockets: ws40 M2, tcp28 M6) ------
if(TARGET imos::transports AND NOT EMSCRIPTEN)
  set(_imos_transport_tests transports/ws40_transport_test.cpp)
  if(IMOS_ENABLE_COMPAT28)
    list(APPEND _imos_transport_tests transports/tcp28_transport_test.cpp)
  endif()
  if(IMOS_ENABLE_WS3)
    list(APPEND _imos_transport_tests transports/ws3_transport_test.cpp)
  endif()
  add_executable(imos_transport_test ${_imos_transport_tests})
  target_link_libraries(imos_transport_test PRIVATE
    GTest::gtest_main
    imos::transports
    imos::core
    imos::imos
    imos::incs
    imos::warnings)
  target_compile_features(imos_transport_test PRIVATE cxx_std_20)
  target_compile_definitions(imos_transport_test PRIVATE
    IMOS_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    IMOS_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/fixtures")
  gtest_discover_tests(imos_transport_test DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 60)
endif()

# ---- profile conformance suites (real imos vs real incs, plan §10) ----
add_executable(imos_profile_test
  profiles/profile1_test.cpp
  profiles/profile2_test.cpp
  profiles/profile3_test.cpp
  profiles/profile4_test.cpp
  profiles/profile5_test.cpp
  profiles/profile6_test.cpp
  profiles/profile7_test.cpp
  profiles/spec_regression_test.cpp)
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

# ---- packaging: install-and-link consumer smoke (plan §9 / M7) --------
# Installs the built tree into a throwaway prefix and configures/builds/runs a
# clean find_package(ImaginaryMOS) consumer against it, in every link style the
# package ships. Opt-in (IMOS_BUILD_PACKAGING_TESTS) because it drives a full
# nested install + configure + build; the `package` CI job turns it on.
if(IMOS_INSTALL_PACKAGE AND IMOS_BUILD_PACKAGING_TESTS)
  set(_imos_smoke_styles "")
  if(IMOS_BUILD_SHARED)
    list(APPEND _imos_smoke_styles shared)
  endif()
  if(IMOS_BUILD_STATIC)
    list(APPEND _imos_smoke_styles static)
  endif()
  add_test(NAME Packaging.ConsumerSmoke
    COMMAND "${CMAKE_COMMAND}"
      "-DIMOS_BUILD_DIR=${CMAKE_BINARY_DIR}"
      "-DIMOS_CONSUMER_SRC=${CMAKE_CURRENT_SOURCE_DIR}/packaging/consumer"
      "-DIMOS_WORK=${CMAKE_BINARY_DIR}/packaging-smoke"
      "-DIMOS_GENERATOR=${CMAKE_GENERATOR}"
      "-DIMOS_BUILD_TYPE=$<CONFIG>"
      "-DIMOS_LINK_STYLES=${_imos_smoke_styles}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/packaging/consumer_smoke.cmake")
  # It installs the whole build, so give it room and keep it off the shared
  # fixture path of the other suites.
  set_tests_properties(Packaging.ConsumerSmoke PROPERTIES TIMEOUT 600 RUN_SERIAL ON)
endif()
