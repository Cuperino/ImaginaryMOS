# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Test suite (plan §10). At M0: a GoogleTest smoke test that links every
# first-party library and confirms version/linkage. The profile conformance,
# codec, session, fuzz and soak suites populate this tree from M1 onward.

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
