# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# imos_add_library(<component>
#   SOURCES        <src...>
#   [INCLUDE_DIR   <dir>]            # public headers (default: <cwd>/include)
#   [EXPORT_BASE   <NAME>]           # export-macro base (default IMOS_<COMPONENT>)
#   [OUTPUT_NAME   <name>]           # library file stem (default <component>)
#   [LINK_PUBLIC   <tgt...>]         # deps whose headers appear in our API
#   [LINK_PRIVATE  <tgt...>]         # implementation-only first-party deps
#   [VENDORED      <tgt...>])        # implementation-only *vendored external*
#                                    #   deps (libxml2/zlib/ixwebsocket): linked
#                                    #   privately for the build but kept out of
#                                    #   the install interface. In a package
#                                    #   build their objects are folded into the
#                                    #   static archive (fat-archive) or absorbed
#                                    #   into the shared library. See
#                                    #   cmake/IMOSPackaging.cmake.
#
# Produces both static and shared libraries from a single OBJECT compile (plan
# §9), with hidden visibility + a generated export header (plan §8.2), and an
# `imos::<component>` alias pointing at the shared library (or static if shared
# is disabled). Registers install/export rules when IMOS_INSTALL is on.

include_guard(GLOBAL)
include(GenerateExportHeader)
include(GNUInstallDirs)

function(imos_add_library COMPONENT)
  cmake_parse_arguments(ARG "" "INCLUDE_DIR;EXPORT_BASE;OUTPUT_NAME;EXPORT_SUBDIR"
                            "SOURCES;LINK_PUBLIC;LINK_PRIVATE;VENDORED" ${ARGN})

  if(NOT ARG_INCLUDE_DIR)
    set(ARG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
  endif()
  if(NOT ARG_OUTPUT_NAME)
    set(ARG_OUTPUT_NAME "${COMPONENT}")
  endif()
  if(NOT ARG_EXPORT_SUBDIR)
    set(ARG_EXPORT_SUBDIR "imos")
  endif()
  if(NOT ARG_EXPORT_BASE)
    string(TOUPPER "IMOS_${COMPONENT}" ARG_EXPORT_BASE)
    string(REPLACE "-" "_" ARG_EXPORT_BASE "${ARG_EXPORT_BASE}")
  endif()

  set(objlib  "${COMPONENT}_obj")
  set(gendir  "${PROJECT_BINARY_DIR}/generated")
  set(exphdr  "${gendir}/${ARG_EXPORT_SUBDIR}/${COMPONENT}_export.h")

  # ---- shared object compile ------------------------------------------
  add_library(${objlib} OBJECT ${ARG_SOURCES})
  set_target_properties(${objlib} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions(${objlib} PRIVATE ${objlib}_EXPORTS)
  target_link_libraries(${objlib} PRIVATE imos::warnings)
  generate_export_header(${objlib}
    BASE_NAME          ${ARG_EXPORT_BASE}
    EXPORT_MACRO_NAME  ${ARG_EXPORT_BASE}_API
    NO_EXPORT_MACRO_NAME ${ARG_EXPORT_BASE}_LOCAL
    STATIC_DEFINE      ${ARG_EXPORT_BASE}_STATIC
    EXPORT_FILE_NAME   "${exphdr}")

  # On MSVC a DLL's import library and a static library share the .lib suffix,
  # so emitting both under one OUTPUT_NAME collides on <name>.lib. Suffix the
  # static archive there (only when both libraries are built and the suffixes
  # actually match — MinGW/Unix keep distinct extensions and need no rename).
  set(_static_output_name "${ARG_OUTPUT_NAME}")
  if(IMOS_BUILD_STATIC AND IMOS_BUILD_SHARED AND
     CMAKE_STATIC_LIBRARY_SUFFIX STREQUAL CMAKE_IMPORT_LIBRARY_SUFFIX)
    set(_static_output_name "${ARG_OUTPUT_NAME}_static")
  endif()

  # Usage requirements shared by the object compile and both final libraries.
  set(_targets ${objlib})
  if(IMOS_BUILD_STATIC)
    add_library(${COMPONENT}-static STATIC $<TARGET_OBJECTS:${objlib}>)
    add_library(imos::${COMPONENT}-static ALIAS ${COMPONENT}-static)
    target_compile_definitions(${COMPONENT}-static PUBLIC ${ARG_EXPORT_BASE}_STATIC)
    set_target_properties(${COMPONENT}-static PROPERTIES
      OUTPUT_NAME ${_static_output_name} EXPORT_NAME ${COMPONENT}-static)
    list(APPEND _targets ${COMPONENT}-static)
  endif()
  if(IMOS_BUILD_SHARED)
    add_library(${COMPONENT}-shared SHARED $<TARGET_OBJECTS:${objlib}>)
    add_library(imos::${COMPONENT}-shared ALIAS ${COMPONENT}-shared)
    set_target_properties(${COMPONENT}-shared PROPERTIES
      OUTPUT_NAME ${ARG_OUTPUT_NAME} EXPORT_NAME ${COMPONENT}-shared
      VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
    list(APPEND _targets ${COMPONENT}-shared)
  endif()

  foreach(t IN LISTS _targets)
    target_compile_features(${t} PUBLIC cxx_std_20)
    target_include_directories(${t} PUBLIC
      "$<BUILD_INTERFACE:${ARG_INCLUDE_DIR}>"
      "$<BUILD_INTERFACE:${gendir}>"
      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    if(ARG_LINK_PUBLIC)
      target_link_libraries(${t} PUBLIC ${ARG_LINK_PUBLIC})
    endif()
    if(ARG_LINK_PRIVATE)
      target_link_libraries(${t} PRIVATE ${ARG_LINK_PRIVATE})
    endif()
    # Vendored external deps: always needed to build (the shared library absorbs
    # them; static build-tree consumers link them directly). Their treatment in
    # the *install* interface depends on how the package resolves them:
    #   * fat-archive mode (default packaging): the vendored objects are folded
    #     into the installed static archive, so the vendored target must NOT
    #     leak into the export — $<BUILD_INTERFACE:> confines it to the build.
    #   * system-deps mode: the export references the system LibXml2/ZLIB/
    #     ixwebsocket, resolved via find_dependency, so link it unguarded.
    if(IMOS_INSTALL_PACKAGE AND IMOS_USE_SYSTEM_DEPS)
      set(_vendored_guarded OFF)
    else()
      set(_vendored_guarded ON)
    endif()
    foreach(v IN LISTS ARG_VENDORED)
      if(_vendored_guarded)
        target_link_libraries(${t} PRIVATE "$<BUILD_INTERFACE:${v}>")
      else()
        target_link_libraries(${t} PRIVATE "${v}")
      endif()
    endforeach()
  endforeach()

  # Record the vendored deps on the static target for the fat-archive merge.
  if(ARG_VENDORED AND IMOS_BUILD_STATIC)
    set_property(TARGET ${COMPONENT}-static PROPERTY IMOS_VENDORED_DEPS "${ARG_VENDORED}")
  endif()

  # Register the component so IMOSPackaging can iterate them in creation order
  # (fat-archive folding, pkg-config generation) without a hardcoded list.
  set_property(GLOBAL APPEND PROPERTY IMOS_COMPONENTS "${COMPONENT}")
  set_property(GLOBAL PROPERTY IMOS_COMPONENT_${COMPONENT}_OUTPUT_NAME "${ARG_OUTPUT_NAME}")
  set_property(GLOBAL PROPERTY IMOS_COMPONENT_${COMPONENT}_SUBDIR "${ARG_EXPORT_SUBDIR}")
  set_property(GLOBAL PROPERTY IMOS_COMPONENT_${COMPONENT}_PUBLIC_DEPS "${ARG_LINK_PUBLIC}")

  # Default alias prefers the shared library.
  if(IMOS_BUILD_SHARED)
    add_library(imos::${COMPONENT} ALIAS ${COMPONENT}-shared)
  else()
    add_library(imos::${COMPONENT} ALIAS ${COMPONENT}-static)
  endif()

  # ---- install / export ------------------------------------------------
  if(IMOS_INSTALL)
    set(_installable ${_targets})
    list(REMOVE_ITEM _installable ${objlib})
    # When IMOS_INSTALL_PACKAGE is on (M7 packaging), the libraries join the
    # exported ImaginaryMOSTargets set that CMakePackaging installs; the
    # vendored static deps are made self-contained there (fat-archive) so the
    # export needs no vendored find_dependency story. Otherwise (the plain M0
    # install) we ship self-contained libraries and headers without a config.
    set(_export_args "")
    if(IMOS_INSTALL_PACKAGE)
      set(_export_args EXPORT ImaginaryMOSTargets)
    endif()
    install(TARGETS ${_installable} ${_export_args}
      RUNTIME  DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY  DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE  DESTINATION ${CMAKE_INSTALL_LIBDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY "${ARG_INCLUDE_DIR}/"
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES "${exphdr}"
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${ARG_EXPORT_SUBDIR})
  endif()
endfunction()
