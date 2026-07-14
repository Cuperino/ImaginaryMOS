# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Transports (plan §5). At M0 only the ws40_ixws probe is built, to pin the
# IXWebSocket linkage on desktop/mobile targets. The full 4.0 WebSocket client/
# server lands in M2; tcp28 (compat28) and ws3_soap in M6; ws40_emscripten
# swaps in on WebAssembly.

if(EMSCRIPTEN)
  # Browser sandbox: IXWebSocket is replaced by the Emscripten WebSocket API
  # (ws40_emscripten), added in M2. Nothing to build at M0.
  return()
endif()

imos_add_library(transports
  OUTPUT_NAME  imos_transports
  EXPORT_BASE  IMOS_TRANSPORTS
  SOURCES
    ws40_ixws/probe.cpp
  LINK_PRIVATE
    ixwebsocket::ixwebsocket
    imos::core)
