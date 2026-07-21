<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# transports/ws40_emscripten

WebAssembly WebSocket client over `emscripten/websocket.h` (plan §5.4), landed
in **M2** as `Ws40EmscriptenClient` (`<imos/transports/ws40_emscripten.h>`).
Client-only by construction — which is exactly what MOS 4.0 passive mode needs,
so a browser-hosted `imos` device is fully functional with `passive=true`.
Browser limits, by design: no Basic-auth header (the WebSocket API cannot set
headers; `start()` rejects `ClientConfig.auth`), TLS trust belongs to the
browser. Consumers link with `-lwebsocket.js`.

Threading: the *owning thread* model (see the header). The thread that calls
`start()` owns the transport — it makes every later call and receives the
callbacks (registered with Emscripten's calling-thread context). On a
single-threaded build that is the main browser thread; on a multithreaded
(`-pthread`, `wasm-mt` preset) build a dedicated worker pthread can own the
transport instead — the JS WebSocket lives on the main thread and operations
proxy to it, so the main thread must stay responsive. Exercised by
`Ws40Transport.WasmClientProfile0SmokeThreaded` (build the `wasm-mt` preset,
then run the linux suite).
