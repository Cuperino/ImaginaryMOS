# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# imos_capi — the C ABI shim over the imos role library (plan §8.2). Strict
# C-ABI discipline; no C++ type crosses the boundary. At M0 it exposes version
# and status entry points that exercise the export-macro and ABI-version
# scaffolding.

imos_add_library(imos_capi
  OUTPUT_NAME  imos_capi
  EXPORT_BASE  IMOS_CAPI
  SOURCES
    imos_c.cpp
  LINK_PRIVATE
    imos::imos)
