# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# imos_core — the sans-I/O protocol engine (plan §4.1). At M0 this is a minimal
# translation unit that pins the libxml2 + zlib linkage; codec/, session/,
# profiles/ and encoding/ fill in from M1 onward.

imos_add_library(core
  OUTPUT_NAME  imos_core
  EXPORT_BASE  IMOS_CORE
  SOURCES
    version.cpp
  LINK_PRIVATE
    LibXml2::LibXml2
    ZLIB::ZLIB)
