# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Doxygen API reference + narrative guides (plan §15). Off by default and
# entirely optional — the docs are a developer convenience, never a build
# dependency of the libraries. Enable with -DIMOS_BUILD_DOCS=ON, then:
#
#   cmake --build --preset linux --target docs
#
# The generated site lands in <build>/docs/html/index.html.

option(IMOS_BUILD_DOCS "Build the Doxygen API reference (needs doxygen)" OFF)

if(NOT IMOS_BUILD_DOCS)
  return()
endif()

find_package(Doxygen QUIET OPTIONAL_COMPONENTS dot)
if(NOT DOXYGEN_FOUND)
  message(WARNING
    "IMOS_BUILD_DOCS=ON but doxygen was not found; the 'docs' target is unavailable.")
  return()
endif()

# Graphviz drives the class/inheritance diagrams; degrade gracefully without it.
if(TARGET Doxygen::dot)
  set(DOXYGEN_HAVE_DOT YES)
else()
  set(DOXYGEN_HAVE_DOT NO)
  message(STATUS "graphviz 'dot' not found — API docs build without diagrams")
endif()

set(DOXYGEN_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(DOXYGEN_INDEX_FILE "${DOXYGEN_OUTPUT_DIR}/html/index.html")
set(DOXYFILE_IN "${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in")
set(DOXYFILE_OUT "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile")

configure_file("${DOXYFILE_IN}" "${DOXYFILE_OUT}" @ONLY)

# Regenerate when any guide page, public header, or the Doxyfile changes.
file(GLOB_RECURSE IMOS_DOC_PAGES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/api/*.md")
file(GLOB_RECURSE IMOS_PUBLIC_HEADERS CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/core/include/*.h"
  "${PROJECT_SOURCE_DIR}/imos/include/*.h"
  "${PROJECT_SOURCE_DIR}/incs/include/*.h"
  "${PROJECT_SOURCE_DIR}/store/include/*.h"
  "${PROJECT_SOURCE_DIR}/transports/include/*.h"
  "${PROJECT_SOURCE_DIR}/capi/include/*.h")

add_custom_command(
  OUTPUT "${DOXYGEN_INDEX_FILE}"
  COMMAND Doxygen::doxygen "${DOXYFILE_OUT}"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  DEPENDS "${DOXYFILE_OUT}" ${IMOS_DOC_PAGES} ${IMOS_PUBLIC_HEADERS}
  MAIN_DEPENDENCY "${DOXYFILE_OUT}"
  COMMENT "Generating API documentation with Doxygen"
  VERBATIM)

add_custom_target(docs DEPENDS "${DOXYGEN_INDEX_FILE}")

message(STATUS "ImaginaryMOS docs — 'docs' target enabled (dot:${DOXYGEN_HAVE_DOT})")
