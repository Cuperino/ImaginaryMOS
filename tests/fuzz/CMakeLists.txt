# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# libFuzzer harnesses for the standing parsing surfaces (plan §10): XML
# message ingest, the 2.8 stream deframer, store journal recovery, and (M2)
# the 4.0 connection-URL/parameter parser. Clang-only
# (-fsanitize=fuzzer); the linux-fuzz preset builds them with ASan+UBSan.
# Each harness gets a short deterministic smoke run in CTest so "fuzzers
# running" is continuously true; nightly CI runs them longer.

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "IMOS_BUILD_FUZZERS requires Clang (libFuzzer); use the "
                      "linux-fuzz preset")
endif()

set(IMOS_FUZZ_FLAGS -fsanitize=fuzzer)

function(imos_add_fuzzer NAME)
  cmake_parse_arguments(ARG "" "" "SOURCES;LINK;SEED_CORPUS" ${ARGN})
  add_executable(${NAME} ${ARG_SOURCES})
  target_link_libraries(${NAME} PRIVATE ${ARG_LINK} imos::warnings)
  target_compile_features(${NAME} PRIVATE cxx_std_20)
  target_compile_options(${NAME} PRIVATE ${IMOS_FUZZ_FLAGS})
  target_link_options(${NAME} PRIVATE ${IMOS_FUZZ_FLAGS})
  # libFuzzer writes discoveries into the FIRST corpus directory, so a
  # writable scratch dir always comes first; the seed corpus (e.g. the spec
  # fixtures) is read-only second — never let the fuzzer write into it.
  set(_scratch "${CMAKE_CURRENT_BINARY_DIR}/corpus/${NAME}")
  file(MAKE_DIRECTORY "${_scratch}")
  add_test(NAME fuzz-smoke-${NAME}
    COMMAND ${NAME} -runs=256 -max_len=4096 -seed=20260714
            "${_scratch}" ${ARG_SEED_CORPUS})
endfunction()

# Static link avoids mixing sanitizer runtimes into the shared libraries.
imos_add_fuzzer(imos_fuzz_xml_ingest
  SOURCES fuzz_xml_ingest.cpp
  LINK imos::core-static
  SEED_CORPUS "${PROJECT_SOURCE_DIR}/fixtures/spec-examples")

imos_add_fuzzer(imos_fuzz_deframer
  SOURCES fuzz_deframer.cpp
  LINK imos::core-static)

imos_add_fuzzer(imos_fuzz_store_recovery
  SOURCES fuzz_store_recovery.cpp
  LINK imos::store_file-static imos::core-static)

imos_add_fuzzer(imos_fuzz_ws40_url
  SOURCES fuzz_ws40_url.cpp
  LINK imos::core-static)

imos_add_fuzzer(imos_fuzz_fqid
  SOURCES fuzz_fqid.cpp
  LINK imos::core-static)

# Profile 3 (M5): the subset-XPath / generalSearch / mosReqObjList surface.
# Seeded with the spec fixtures (which include the gated and illustrative
# mosReqObjList documents) plus raw XPath expressions.
imos_add_fuzzer(imos_fuzz_object_query
  SOURCES fuzz_object_query.cpp
  LINK imos::core-static
  SEED_CORPUS "${PROJECT_SOURCE_DIR}/fixtures/spec-examples"
              "${CMAKE_CURRENT_SOURCE_DIR}/seeds/object_query")
