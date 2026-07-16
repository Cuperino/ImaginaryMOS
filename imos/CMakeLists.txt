# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# imos — the Media Object Server (device) role library (plan §4.6). Thin
# composition over imos_core.

imos_add_library(imos
  OUTPUT_NAME  imos
  EXPORT_BASE  IMOS_IMOS
  SOURCES
    imos.cpp
    device.cpp
  LINK_PUBLIC
    imos::core)
