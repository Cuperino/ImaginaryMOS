# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
#
# Transports (plan §5). M2: the full MOS 4.0 WebSocket client/server over
# IXWebSocket (ws40_ixws) on desktop/mobile targets, and the client-only
# ws40_emscripten on WebAssembly. M6: the MOS 2.8.x TCP transport (tcp28,
# plan's imos_compat28 module, gated by IMOS_ENABLE_COMPAT28) and the MOS
# 3.x SOAP transport (ws3_soap / imos_ws3, gated by IMOS_ENABLE_WS3).

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

set(_imos_transport_sources
  ws40_ixws/probe.cpp
  ws40_ixws/ws40_client.cpp
  ws40_ixws/ws40_server.cpp)
if(IMOS_ENABLE_COMPAT28)
  list(APPEND _imos_transport_sources
    tcp28/tcp28_socket.cpp
    tcp28/tcp28_endpoint.cpp)
endif()
if(IMOS_ENABLE_WS3)
  list(APPEND _imos_transport_sources
    ws3_soap/ws3_transform.cpp
    ws3_soap/ws3_endpoint.cpp)
endif()

imos_add_library(transports
  OUTPUT_NAME  imos_transports
  EXPORT_BASE  IMOS_TRANSPORTS
  SOURCES
    ${_imos_transport_sources}
  LINK_PUBLIC
    imos::core                      # ITransport/PeerSession/encoding in our API
  VENDORED
    ixwebsocket::ixwebsocket)

if(IMOS_ENABLE_COMPAT28 AND WIN32)
  # WSAPoll and Winsock2 live in ws2_32; the ws40 path gets this via
  # IXWebSocket, the tcp28 socket layer needs it directly.
  foreach(_imos_t transports-static transports-shared)
    if(TARGET ${_imos_t})
      target_link_libraries(${_imos_t} PRIVATE ws2_32)
    endif()
  endforeach()
endif()
