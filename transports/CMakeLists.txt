# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Transports (plan §5). M2: the full MOS 4.0 WebSocket client/server over
# IXWebSocket (ws40_ixws) on desktop/mobile targets, and the client-only
# ws40_emscripten on WebAssembly. tcp28 (compat28) and ws3_soap land in M6.

if(EMSCRIPTEN)
  # Browser sandbox (plan §5.4): client-only, over the Emscripten WebSocket
  # API. Consuming applications must link with -lwebsocket.js.
  imos_add_library(transports
    OUTPUT_NAME  imos_transports
    EXPORT_BASE  IMOS_TRANSPORTS
    SOURCES
      ws40_emscripten/ws40_emscripten.cpp
    LINK_PUBLIC
      imos::core)
  return()
endif()

imos_add_library(transports
  OUTPUT_NAME  imos_transports
  EXPORT_BASE  IMOS_TRANSPORTS
  SOURCES
    ws40_ixws/probe.cpp
    ws40_ixws/ws40_client.cpp
    ws40_ixws/ws40_server.cpp
  LINK_PUBLIC
    imos::core                      # ITransport/PeerSession/encoding in our API
  LINK_PRIVATE
    ixwebsocket::ixwebsocket)
