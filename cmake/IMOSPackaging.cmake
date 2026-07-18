# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# M7 packaging (plan §9). Turns the built libraries into consumable artifacts:
#
#   * self-contained static archives — the vendored libxml2/zlib/IXWebSocket
#     objects are folded into the imos static libraries (fat-archive), so a
#     static consumer needs no vendored find_dependency (choice of record);
#   * the exported CMake package (ImaginaryMOSConfig + ImaginaryMOSTargets +
#     ImaginaryMOSDependencies), consumed with find_package(ImaginaryMOS);
#   * pkg-config (.pc) files, one per public library;
#   * CPack metadata for the desktop bundles (tarballs / zip).
#
# The mobile/wasm bundles (XCFramework, AAR, wasm) are assembled from the
# install tree by the scripts and per-toolchain custom targets under
# packaging/ (see packaging/README.md), which this module wires up.
#
# Included from the top-level CMakeLists only when IMOS_INSTALL AND
# IMOS_INSTALL_PACKAGE, after every component add_subdirectory().

include_guard(GLOBAL)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

get_property(_imos_components GLOBAL PROPERTY IMOS_COMPONENTS)
if(NOT _imos_components)
  message(FATAL_ERROR "IMOSPackaging: no components registered (include order?)")
endif()

set(IMOS_CMAKE_CONFIG_DEST "${CMAKE_INSTALL_LIBDIR}/cmake/ImaginaryMOS")

# ---------------------------------------------------------------------------
# 1. Fat static archives + the system transitive libs they still need.
#
# Each vendored dep is folded into the *first* static target that declares it
# (components are registered in add_subdirectory order, so `core` claims
# libxml2+zlib and `transports` claims ixwebsocket; `store_file`'s zlib is
# already covered by the core it publicly depends on and is skipped). The
# system libraries the folded objects need (threads, libm, OpenSSL under TLS)
# cannot be folded — they are surfaced on the static target's INSTALL interface
# and turned into find_dependency() calls below.
# ---------------------------------------------------------------------------
set(_imos_find_deps "")        # package names needing find_dependency()
set(_imos_pc_private_libs "")  # plain -l flags for the pkg-config Libs.private

# The vendored targets whose objects get folded in — never surfaced as deps.
set(_imos_vendored_targets
  LibXml2 LibXml2::LibXml2 xml2
  zlib zlibstatic ZLIB::ZLIB z
  ixwebsocket ixwebsocket::ixwebsocket)

function(_imos_map_system_dep item out_link out_pkg out_pcflag)
  # Classify one transitive link item into (interface link entry, package for
  # find_dependency, pkg-config -l flag). Empty out where not applicable.
  set(${out_link} "" PARENT_SCOPE)
  set(${out_pkg}  "" PARENT_SCOPE)
  set(${out_pcflag} "" PARENT_SCOPE)
  # Strip a $<LINK_ONLY:...> / $<BUILD_INTERFACE:...> wrapper if present.
  string(REGEX REPLACE "^\\$<[A-Z_]+:(.+)>$" "\\1" item "${item}")
  if(item STREQUAL "" OR item MATCHES "^\\$<")
    return()
  endif()
  if(item MATCHES "^Threads::")
    set(${out_link} "Threads::Threads" PARENT_SCOPE)
    set(${out_pkg}  "Threads" PARENT_SCOPE)
    set(${out_pcflag} "-pthread" PARENT_SCOPE)
  elseif(item MATCHES "^OpenSSL::")
    set(${out_link} "${item}" PARENT_SCOPE)
    set(${out_pkg}  "OpenSSL" PARENT_SCOPE)
    if(item MATCHES "SSL")
      set(${out_pcflag} "-lssl" PARENT_SCOPE)
    else()
      set(${out_pcflag} "-lcrypto" PARENT_SCOPE)
    endif()
  elseif(item MATCHES "::")
    # Some other imported target — pass it through and hope find_package covers
    # it; do not invent a find_dependency we cannot name.
    set(${out_link} "${item}" PARENT_SCOPE)
  else()
    # A bare system library name (m, dl, ws2_32, ...) or a -flag.
    set(${out_link} "${item}" PARENT_SCOPE)
    if(item MATCHES "^-")
      set(${out_pcflag} "${item}" PARENT_SCOPE)
    else()
      set(${out_pcflag} "-l${item}" PARENT_SCOPE)
    endif()
  endif()
endfunction()

if(NOT IMOS_USE_SYSTEM_DEPS)
  # Pick the archive-merge tool for this toolchain.
  if(MSVC)
    set(_merge_tool msvc)
  elseif(APPLE)
    set(_merge_tool libtool)
    find_program(IMOS_LIBTOOL libtool REQUIRED)
  else()
    set(_merge_tool gnuar)
  endif()

  set(_folded_deps "")
  foreach(_c IN LISTS _imos_components)
    set(_st "${_c}-static")
    if(NOT TARGET ${_st})
      continue()
    endif()
    get_property(_deps TARGET ${_st} PROPERTY IMOS_VENDORED_DEPS)
    if(NOT _deps)
      continue()
    endif()

    # Fold only the deps not already folded into an earlier archive.
    set(_to_fold "")
    foreach(_d IN LISTS _deps)
      if(NOT _d IN_LIST _folded_deps)
        list(APPEND _to_fold "$<TARGET_FILE:${_d}>")
        list(APPEND _folded_deps "${_d}")
      endif()

      # Collect the system transitive libs this dep needs, regardless of who
      # folds it, so the archive that carries it also carries the interface.
      # Strip any $<LINK_ONLY:...>/$<BUILD_INTERFACE:...> wrapper *before* the
      # vendored check — a vendored dep of a vendored dep (e.g. ixwebsocket's
      # private ZLIB) arrives wrapped and would otherwise slip past into the
      # export as a non-exported target.
      get_target_property(_dll ${_d} INTERFACE_LINK_LIBRARIES)
      if(_dll)
        foreach(_item IN LISTS _dll)
          string(REGEX REPLACE "^\\$<[A-Z_]+:(.+)>$" "\\1" _clean "${_item}")
          if(_clean STREQUAL "" OR _clean MATCHES "^\\$<")
            continue()  # empty or a nested/unresolvable genex
          endif()
          if(_clean IN_LIST _imos_vendored_targets)
            continue()  # folded elsewhere; never surface it
          endif()
          _imos_map_system_dep("${_clean}" _lnk _pkg _pcf)
          if(_lnk)
            target_link_libraries(${_st} INTERFACE "$<INSTALL_INTERFACE:${_lnk}>")
          endif()
          if(_pkg)
            list(APPEND _imos_find_deps "${_pkg}")
          endif()
          if(_pcf)
            list(APPEND _imos_pc_private_libs "${_pcf}")
          endif()
        endforeach()
      endif()
    endforeach()

    if(_to_fold)
      # Fold at install time on the *installed* archive. This sidesteps the
      # same-directory restriction of add_custom_command(TARGET ...) (the
      # component targets live in their own subdirectories) and leaves the
      # build-tree archives — which the tests link against the still-separate
      # vendored libs — untouched. install(CODE) has honored generator
      # expressions since CMake 3.14; the fold runs after the component's own
      # install(TARGETS) because IMOSPackaging is included last.
      set(_inputs_join "$<JOIN:${_to_fold},$<SEMICOLON>>")
      install(CODE
"set(_dst \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/$<TARGET_FILE_NAME:${_st}>\")
message(STATUS \"Folding vendored deps into $<TARGET_FILE_NAME:${_st}>\")
execute_process(
  COMMAND \"${CMAKE_COMMAND}\"
    \"-DIMOS_MERGE_OUTPUT=\${_dst}\"
    \"-DIMOS_MERGE_INPUTS=${_inputs_join}\"
    \"-DIMOS_MERGE_TOOL=${_merge_tool}\"
    \"-DIMOS_MERGE_AR=${CMAKE_AR}\"
    \"-DIMOS_MERGE_RANLIB=${CMAKE_RANLIB}\"
    \"-DIMOS_MERGE_LIBTOOL=${IMOS_LIBTOOL}\"
    -P \"${PROJECT_SOURCE_DIR}/cmake/imos_merge_archives.cmake\"
  RESULT_VARIABLE _fold_rc)
if(NOT _fold_rc EQUAL 0)
  message(FATAL_ERROR \"vendored-dep fold failed for ${_st}\")
endif()")
    endif()
  endforeach()

  # libxml2 needs libm on the ELF/Mach-O targets; guarantee it even if the
  # target interface did not spell it out (deduped against the introspected set).
  if(NOT WIN32)
    list(APPEND _imos_pc_private_libs "-lm")
  endif()
else()
  # System-deps package: the export references the system vendored packages.
  list(APPEND _imos_find_deps LibXml2 ZLIB)
  if(TARGET ixwebsocket::ixwebsocket)
    # Resolved via its own CONFIG package on the consumer.
    set(_imos_find_ixws TRUE)
  endif()
endif()

if(IMOS_ENABLE_TLS)
  list(APPEND _imos_find_deps OpenSSL)
endif()
list(REMOVE_DUPLICATES _imos_find_deps)
list(REMOVE_DUPLICATES _imos_pc_private_libs)

# ---------------------------------------------------------------------------
# 2. ImaginaryMOSDependencies.cmake — re-establish imported targets pre-Targets.
# ---------------------------------------------------------------------------
if(IMOS_USE_SYSTEM_DEPS)
  set(IMOS_DEP_MODE_COMMENT "This install was built with IMOS_USE_SYSTEM_DEPS=ON.")
else()
  set(IMOS_DEP_MODE_COMMENT "This install folded the pinned deps into the libraries (default).")
endif()

set(IMOS_FIND_DEPENDENCY_CALLS "")
foreach(_pkg IN LISTS _imos_find_deps)
  string(APPEND IMOS_FIND_DEPENDENCY_CALLS "find_dependency(${_pkg})\n")
endforeach()
if(_imos_find_ixws)
  string(APPEND IMOS_FIND_DEPENDENCY_CALLS "find_dependency(ixwebsocket)\n")
endif()
if(IMOS_FIND_DEPENDENCY_CALLS STREQUAL "")
  set(IMOS_FIND_DEPENDENCY_CALLS "# (no external dependencies to resolve)\n")
endif()

configure_file(
  "${PROJECT_SOURCE_DIR}/cmake/ImaginaryMOSDependencies.cmake.in"
  "${PROJECT_BINARY_DIR}/ImaginaryMOSDependencies.cmake"
  @ONLY)

# ---------------------------------------------------------------------------
# 3. Package config + version + exported targets.
# ---------------------------------------------------------------------------
# The export only carries the link-style-suffixed targets (imos::imos-shared /
# imos::imos-static); the config recreates the un-suffixed convenience targets
# (imos::imos → the preferred style) from this list.
set(IMOS_PACKAGE_COMPONENTS "${_imos_components}")

configure_package_config_file(
  "${PROJECT_SOURCE_DIR}/cmake/ImaginaryMOSConfig.cmake.in"
  "${PROJECT_BINARY_DIR}/ImaginaryMOSConfig.cmake"
  INSTALL_DESTINATION "${IMOS_CMAKE_CONFIG_DEST}")
write_basic_package_version_file(
  "${PROJECT_BINARY_DIR}/ImaginaryMOSConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${PROJECT_BINARY_DIR}/ImaginaryMOSConfig.cmake"
  "${PROJECT_BINARY_DIR}/ImaginaryMOSConfigVersion.cmake"
  "${PROJECT_BINARY_DIR}/ImaginaryMOSDependencies.cmake"
  DESTINATION "${IMOS_CMAKE_CONFIG_DEST}")

install(EXPORT ImaginaryMOSTargets
  FILE ImaginaryMOSTargets.cmake
  NAMESPACE imos::
  DESTINATION "${IMOS_CMAKE_CONFIG_DEST}")

# License ships with the package (REUSE layout: LICENSES/Apache-2.0.txt).
set(IMOS_LICENSE_FILE "${PROJECT_SOURCE_DIR}/LICENSES/Apache-2.0.txt")
install(FILES "${IMOS_LICENSE_FILE}"
  DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/licenses/ImaginaryMOS"
  RENAME LICENSE)

# ---------------------------------------------------------------------------
# 4. pkg-config (.pc) files, one per public library.
# ---------------------------------------------------------------------------
list(REMOVE_DUPLICATES _imos_pc_private_libs)
string(REPLACE ";" " " _pc_private_libs_str "${_imos_pc_private_libs}")
string(STRIP "${_pc_private_libs_str}" _pc_private_libs_str)

foreach(_c IN LISTS _imos_components)
  get_property(_out GLOBAL PROPERTY IMOS_COMPONENT_${_c}_OUTPUT_NAME)
  get_property(_pub GLOBAL PROPERTY IMOS_COMPONENT_${_c}_PUBLIC_DEPS)

  # Requires: our own public deps, mapped from imos::<x> to the .pc name.
  set(PC_REQUIRES "")
  foreach(_p IN LISTS _pub)
    if(_p MATCHES "^imos::(.+)$")
      get_property(_pout GLOBAL PROPERTY IMOS_COMPONENT_${CMAKE_MATCH_1}_OUTPUT_NAME)
      list(APPEND PC_REQUIRES "${_pout}")
    endif()
  endforeach()
  string(REPLACE ";" " " PC_REQUIRES "${PC_REQUIRES}")

  set(PC_NAME "${_out}")
  set(PC_DESC "ImaginaryMOS ${_c} — MOS Protocol v4.0 library suite")
  set(PC_LIBNAME "${_out}")
  set(PC_REQUIRES_PRIVATE "")
  # Only the base engine (core) and the transport carry the folded system libs.
  if(_c STREQUAL "core" OR _c STREQUAL "transports")
    set(PC_LIBS_PRIVATE "${_pc_private_libs_str}")
  else()
    set(PC_LIBS_PRIVATE "")
  endif()

  configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/imos.pc.in"
    "${PROJECT_BINARY_DIR}/pkgconfig/${_out}.pc"
    @ONLY)
  install(FILES "${PROJECT_BINARY_DIR}/pkgconfig/${_out}.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
endforeach()

# ---------------------------------------------------------------------------
# 5. CPack — desktop bundles (tarball / zip). The mobile/wasm bundles are their
#    own targets (packaging/), not CPack generators.
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "ImaginaryMOS")
set(CPACK_PACKAGE_VENDOR "Javier O. Cordero Pérez")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "${PROJECT_HOMEPAGE_URL}")
set(CPACK_RESOURCE_FILE_LICENSE "${IMOS_LICENSE_FILE}")
set(CPACK_VERBATIM_VARIABLES ON)
set(CPACK_PACKAGE_FILE_NAME
  "imaginarymos-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_STRIP_FILES ON)

if(WIN32)
  set(_imos_cpack_gen "ZIP")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The reference platform (Debian 13) also ships a .deb next to the tarball.
  set(_imos_cpack_gen "TGZ;DEB")
else()
  set(_imos_cpack_gen "TGZ")  # macOS et al.
endif()
set(CPACK_GENERATOR "${_imos_cpack_gen}")

# --- Debian (.deb) generator metadata --------------------------------------
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Javier O. Cordero Pérez <javiercorderoperez@gmail.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "libdevel")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")           # standard <name>_<ver>_<arch>.deb
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)              # derive libc/libstdc++ deps
if(IMOS_ENABLE_TLS)
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libssl3")       # our only external runtime dep
endif()

include(CPack)

if(IMOS_USE_SYSTEM_DEPS)
  set(_imos_pkg_mode "system-deps")
else()
  set(_imos_pkg_mode "fat-archive")
endif()
message(STATUS "ImaginaryMOS packaging: mode=${_imos_pkg_mode} "
  "generator=${_imos_cpack_gen} find_dependency=[${_imos_find_deps}]")
