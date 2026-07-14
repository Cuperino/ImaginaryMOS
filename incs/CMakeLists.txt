# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# incs — the Newsroom Computer System (NCS) role library (plan §4.6). Powers
# the NRCS simulator used for M8 interop testing.

imos_add_library(incs
  OUTPUT_NAME   incs
  EXPORT_BASE   IMOS_INCS
  EXPORT_SUBDIR incs
  SOURCES
    incs.cpp
  LINK_PUBLIC
    imos::core)
