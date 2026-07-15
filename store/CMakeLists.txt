# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# imos_store_file — the default file-backed IMessageStore (plan §6).
# Deliberately outside the sans-I/O core: this module owns real files.

imos_add_library(store_file
  OUTPUT_NAME  imos_store_file
  EXPORT_BASE  IMOS_STORE_FILE
  SOURCES
    file_store.cpp
  LINK_PUBLIC
    imos::core
  LINK_PRIVATE
    ZLIB::ZLIB)
