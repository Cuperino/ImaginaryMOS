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
endif()
