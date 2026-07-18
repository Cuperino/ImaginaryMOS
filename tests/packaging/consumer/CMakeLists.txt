# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# A clean, standalone consumer of the installed ImaginaryMOS package. It is not
# part of the main build tree — the packaging smoke test (tests/packaging/
# consumer_smoke.cmake) configures it against an install prefix to prove that
# find_package(ImaginaryMOS) + link works from scratch, in both link styles
# (plan §9 / M7 exit criterion: "install-and-link smoke tests from a clean
# consumer project").

cmake_minimum_required(VERSION 3.24)
project(imos_consumer LANGUAGES CXX)

# IMOS_CONSUMER_LINK selects the link style to exercise: "shared" (default) or
# "static". Static must pull the folded vendored deps + system libs purely from
# the exported interface.
set(IMOS_CONSUMER_LINK "shared" CACHE STRING "shared|static")

find_package(ImaginaryMOS CONFIG REQUIRED)

add_executable(imos_consumer main.cpp)

if(IMOS_CONSUMER_LINK STREQUAL "static")
  target_link_libraries(imos_consumer PRIVATE
    imos::imos-static imos::incs-static imos::transports-static
    imos::store_file-static imos::imos_capi-static)
else()
  target_link_libraries(imos_consumer PRIVATE
    imos::imos imos::incs imos::transports imos::store_file imos::imos_capi)
endif()
