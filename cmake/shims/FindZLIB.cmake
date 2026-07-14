# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# find_package(ZLIB) shim: satisfies dependencies that look for a system zlib
# (notably IXWebSocket) by pointing them at the pinned 3rdparty/zlib build,
# so the vendored copy is used consistently instead of a host library.

if(NOT TARGET ZLIB::ZLIB)
  if(TARGET zlibstatic)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
  elseif(TARGET zlib)
    add_library(ZLIB::ZLIB ALIAS zlib)
  endif()
endif()

if(TARGET ZLIB::ZLIB)
  set(ZLIB_FOUND TRUE)
  set(ZLIB_LIBRARY ZLIB::ZLIB)
  set(ZLIB_LIBRARIES ZLIB::ZLIB)
  set(ZLIB_VERSION_STRING "1.2.13")
else()
  set(ZLIB_FOUND FALSE)
  if(ZLIB_FIND_REQUIRED)
    message(FATAL_ERROR "vendored zlib target not available (build order?)")
  endif()
endif()
