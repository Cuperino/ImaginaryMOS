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
browser, callbacks arrive on the main thread (host pumps them via `pump()`).
Consumers link with `-lwebsocket.js`.
