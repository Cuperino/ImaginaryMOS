# ImaginaryMOS — Implementation Plan

**Components:** `imos` (Media Object Server role) and `incs` (Newsroom Computer System role)
**Language / toolkit:** C++20 · IXWebSocket · libxml2 · zlib · GoogleTest/GoogleMock
**Spec baseline:** MOS Protocol v4.0.1 (project copy: `MOS-Protocol-Version-4_0_1_.pdf`), with MOS 2.8.5 and MOS 3.8.4 referenced for compatibility layers
**License:** Apache-2.0
**Status:** Rev. 3 — incorporates plan-review decisions and manual changes · 2026-07-13

---

## 1. Summary

ImaginaryMOS is a portable protocol library suite implementing the full MOS Protocol v4.0 (all Profiles 0–7) with backwards compatibility for the MOS 2.8.x dual-socket TCP transport and the MOS 3.x SOAP WebService transport. It ships as two role libraries built on a shared core: `imos`, the Media Object Server side, which is the primary product and is designed to be embedded inside MOS devices (playout servers, graphics engines, prompters, publishing endpoints); and `incs`, the NCS side, which exists to serve NCS integrators and — critically for this project — to power the NRCS simulator that substitutes for commercial newsroom systems we cannot access during interoperability testing.

The defining architectural decision is a **sans-I/O protocol core**: every state machine, codec, and profile workflow is implemented free of sockets, threads, and clocks, with transports, time, and persistence injected behind narrow interfaces. This is what makes one codebase honestly portable across Linux, macOS, Windows, Android, iOS, and WebAssembly (where the Emscripten WebSocket API replaces IXWebSocket), what makes both static and dynamic linking straightforward, what makes a stable C ABI practical, and what lets GoogleTest exercise every profile deterministically without a network. Reliability behaviors that the spec asks for — strictly sequential per-channel delivery, retry on timeout, and durable buffering of unacknowledged messages across restarts — are first-class in the design via the `IMessageStore` interface and its file-backed implementation.

The plan below is grounded in the v4.0.1 specification text: the profile and message inventory in §3 was extracted from the project's spec copy, and the details that the spec leaves ambiguous (notably message encoding over WebSocket) are called out explicitly and resolved at plan review rather than silently assumed; the encoding decision is recorded in §3.4, and all review resolutions are logged in §14.

## 2. Requirements traceability

| Requirement (as stated) | Where addressed |
|---|---|
| MOS v4.0 library, all profiles (0–7) | §3.2 inventory; §4 architecture; per-profile milestones M2–M5; conformance suites §10 |
| Backwards compatibility with 2.8.x TCP | §5.2 transport; legacy-message acceptance §3.2; M6 |
| MOS 3.x WebService support | §5.3; M6 |
| Project/component naming (ImaginaryMOS, `imos`, `incs`) | §4.3 deliverables |
| Primary role: `imos` embedded in MOS devices | §4.1 principles (footprint, pull-mode event pump, no forced threads); §8 API |
| C++20, IXWebSocket, libxml2, zlib | §4, §5, §9 (zlib: WebSocket permessage-deflate, store CRC/compression, HTTP gzip) |
| GoogleTest/GoogleMock, full coverage of each profile | §10 test strategy; conformance matrix gate |
| Static and dynamic linking | §9 |
| Six official targets incl. WASM via Emscripten API | §5.4, §9 |
| C ABI in scope | §8.2 |
| `IMessageStore` with file-backed persistence | §6 |
| Dialects: Sofie, iNEWS, ENPS, Octopus | §7 |
| No NRCS access → simulation for M8 interop | §11; milestone M8 |
| Use of Sofie TV Automation, mosromgr, Superdesk | §7 (quirk sourcing), §11 (interop lab) |

## 3. What the protocol requires — spec grounding

### 3.1 Three transport generations, one message vocabulary

The spec's own framing is that "the message flow and message structure of MOS 4.0 is fundamentally the same as earlier versions," and that an NCS is expected to speak v4.0 *in addition to* MOS 2.x (sockets) and MOS 3.x (WebService). ImaginaryMOS mirrors that: a single message model and session logic, with three interchangeable transports.

**MOS 2.8.x (TCP).** XML messages over long-lived raw TCP connections on up to three ports: the *lower* port 10540 (media-object metadata traffic, e.g. `mosObj`), the *upper* port 10541 (running-order traffic), and the rarely used *query* port 10542. Encoding is ISO 10646 UCS-2, big-endian. There is no length framing; messages are delimited by the XML structure itself (`<mos>…</mos>` roots on a byte stream), so the receiver needs a streaming deframer. Sockets are meant to be held open continuously; message flow is strictly sequential per port; and each message definition in the spec carries a "Port" annotation that our code generation will capture so the session layer routes every message to the correct channel automatically.

**MOS 3.x (WebService).** The same message vocabulary carried as SOAP WebService operations over HTTP(S), defined by WSDLs published by the MOS Group; the compatibility target is MOS 3.8.4. This generation is request/response rather than long-lived streams; §5.3 scopes what we implement.

**MOS 4.0 (WebSocket).** Both the MOS and the NCS expose a web server interface — typically on 80/443 but configurable — and behind it a WebSocket endpoint (e.g. `wss://<host>/mos/Communication`). Connection URLs carry query parameters identifying the parties and the logical channel: `mosID`, `ncsID`, and `channel`, where `channel=mom` corresponds to the legacy lower port 10540, `channel=ro` to the upper port 10541, and `channel=aux` to the query port 10542. The two-port *logic* of 2.x is deliberately preserved; only the pipe changes, and each MOS message is bounded by WebSocket framing (one message per WebSocket frame/message, no stream deframing needed). Two connection topologies exist. In **standard mode**, each side opens WebSocket connections to the other side's endpoint for the channels on which it originates traffic. In **passive mode** (`passive=true` on the URL), the party inside a firewall opens one client connection per channel outward to the externally reachable party and keeps them alive; the external party then originates its messages back down those same connections, so no inbound firewall ports are needed. If a passive connection drops, the inner party must re-establish it as quickly as possible. Security is layered on web standards: `wss` with CA-issued certificates always accepted, self-signed certificates accepted or acceptable-by-option, and HTTP Basic authentication headers on the upgrade request recommended in preference to credentials in URLs.

### 3.2 Profiles and message inventory (v4.0.1)

Support for Profile 0 plus at least one other profile is the minimum bar for claiming MOS compatibility; a vendor claiming a profile must implement *all* of its messages and its workflow. ImaginaryMOS targets all eight. The inventory below was taken from the spec copy in this project and is the authoritative checklist for the codec, the profile services, and the conformance suites.

| Profile | Name | Messages required (beyond lower profiles) | Notes for implementation |
|---|---|---|---|
| 0 | Basic Communication | `keepAlive`, `heartbeat`, `reqMachInfo`, `listMachInfo` | `keepAlive` is new in 4.0: no `messageID`, no reply, discard on receipt, send no faster than every 30 s (or on 30 s idle), valid on all channels bidirectionally. `heartbeat` is echoed with a timestamp (loop-avoidance required). `listMachInfo` declares manufacturer/model/revisions, device type (NCS or MOS), and the supported-profiles list — our version/dialect negotiation reads this. |
| 1 | Basic Object Based Workflow | `mosAck`, `mosObj`, `mosReqObj`, `mosReqAll`, `mosListAll` | `mosReqAll` has two synchronization methods keyed on `<pause>`: `0` → batch reply inside one `mosListAll`; `>0` → individual `mosObj` messages spaced by *pause* seconds, interleavable with live traffic. `mosReqObj` must be supported as a diagnostic even though rare. |
| 2 | Basic Running Order / Content List Workflow | `roAck`, `roCreate`, `roReplace`, `roMetadataReplace`, `roDelete`, `roReq`, `roList`, `roReadyToAir`, `roElementAction`, `roElementStat` | The "roConstruction" family. `roElementAction` performs INSERT/REPLACE/MOVE/SWAP/DELETE on stories and items and is the only sanctioned sequence-modification message in 4.0. `roElementStat` (element = RO, STORY or ITEM) is bidirectional as of 2.8.5 and supersedes `roStat`/`roItemStat`. |
| 3 | Advanced Object Based Workflow | `mosObjCreate`, `mosItemReplace`, `mosReqObjAction`, and the search family `mosReqSearchableSchema`, `mosListSearchableSchema`, `mosReqObjList`, `mosObjList` | `mosObjCreate`: NCS asks the MOS to create an object/placeholder; ACK returns the new `objID` in `statusDescription`, and the MOS then emits a full `mosObj` on the *mom* channel. `mosReqObjAction` lets the NCS modify/delete such objects. `mosReqObjList` supports an XPath-subset query (searchFields ANDed within a group, groups ORed) and is the spec-recommended resync mechanism over `mosReqAll`. `mosItemReplace` lets the MOS replace item data it owns inside an NCS story (position immutable). |
| 4 | Advanced RO / Content List Workflow | `roReqAll`, `roListAll`, `roStorySend` | `roReqAll`/`roListAll` enumerate all MOS-active ROs (headers only; details via `roReq`). `roStorySend` carries full story bodies (mixed content: `p`, `pi`, `em`, `tab`, presenter fields, embedded `storyItem` references) to prompters/publishers; supports `Read1stMEMasBody="true"` where the first `mosExternalMetadata` block substitutes for the body (agency/NewsML payloads arrive this way — payloads must be preserved verbatim). Sequence moves on story-send-linked ROs must use `roElementAction` MOVE, never delete+insert. |
| 5 | Item Control | `roItemCue`, `roCtrl` | `roItemCue` is a timed *notification* (`roEventType`, `roEventTime`), often prompter-originated, addressable to the parent MOS (note the spec's deliberately counterintuitive `ncsID` assignment in that case). `roCtrl` issues READY / EXECUTE / PAUSE / STOP / SIGNAL at RO, story, or item scope, selected by leaving lower-level IDs empty. |
| 6 | MOS Redirection | *(no new messages)* | Requires the Fully Qualified MOS ID convention `<family>.<machine>.<location>.<enterprise>.mos` (location/enterprise optional) so devices can recognize non-local objects and arrange out-of-band transfer. Library work: FQID parse/compose/compare utilities, locality classification hooks, and enforcement toggles; the transfer itself is explicitly outside MOS. |
| 7 | MOS RO / Content List Modification | `roReqStoryAction` | The MOS requests story create/update/move/delete on the NCS, wrapping `roStorySend` payloads, with `leaseLock` and `username` attributes. Advertised via the Profile 7 flag in `listMachInfo`. |

Two adjacent message families are in scope as clearly bounded extras. First, the **support structures** used across messages: `mosExternalMetadata` (`mosScope` ∈ OBJECT/STORY/PLAYLIST, `mosSchema`, free-form `mosPayload`), `objPaths` (`objPath*`, `objProxyPath*`, optional `objMetadataPath`, each with a required `techDescription` attribute), `item`/`storyItem`, `messageID`, and the field formats of spec §6 (times, `objTB` timebases such as 59.94, etc.). Second, the **web-control / ActiveX plug-in messages** of spec chapter 5 (`ncsAck`, `ncsReqAppInfo`, `ncsAppInfo`, `ncsReqAppMode`, `ncsStoryRequest`, `ncsItemRequest`, `ncsItem`, `ncsReqAppClose`, `ncsReqStoryAction`, plus `roStorySend`/`mosItemReplace` in plug-in context). These are host↔plug-in messages rather than network traffic, but they share the codec, and they are the natural payload for the WebAssembly target (HTML5 controls hosted in NCS UIs). They are specified as the optional `imos_plugin` module but — per plan review — carry low priority and remain unscheduled for 1.0; because the codec groundwork is shared, the module can be added later without architectural change.

Finally, v4.0 formally **removes** thirteen legacy messages (`roStoryAppend`, `roStoryInsert`, `roStoryReplace`, `roStoryMove`, `roStoryMoveMultiple`, `roStorySwap`, `roStoryDelete`, `roItemInsert`, `roItemReplace`, `roItemMoveMultiple`, `roItemDelete`, `roStat`, `roItemStat`). A v4.0 endpoint must never *initiate* them, but the spec allows receiving them for legacy interop — and real 2.8-era NCS installations (several of our target dialects) still emit them. Policy: the codec parses all thirteen; the 2.8-compat session layer accepts them and normalizes them onto `roElementAction`/`roElementStat` semantics before they reach application handlers; the 4.0 session layer rejects them outbound at compile time (they simply have no send API in 4.0 mode) and, on receive, accepts-and-normalizes with a diagnostic.

### 3.3 Reliability rules the library must embody

These spec behaviors shape the session layer and are treated as requirements, not options. Message flow is strictly sequential per channel: the sender does not transmit the next message on a channel until the previous one is acknowledged, while the channels remain mutually independent. Endpoints retry unacknowledged messages after a timeout, and acknowledgment within 60 seconds of receipt is the recommended ceiling (we default to acking immediately and to a configurable retry timer well under 60 s). Unacknowledged and queued messages should be buffered such that an application or machine restart does not destroy them — this sentence in the spec is the direct mandate for `IMessageStore` (§6). `messageID` exists to support intelligent retry, which we extend into receive-side duplicate suppression so retries are idempotent. And the spec defines a resynchronization heuristic we implement verbatim: if a MOS receives a message referencing an unknown `roID`/`storyID`/`itemID`, it assumes a prior communication error and issues `roReq` for that RO, rebuilding from the `roList` reply; the object-side analogue uses `mosReqObj`/`mosReqObjList`. Unknown tags inside otherwise well-formed messages must be ignored without error — the codec therefore preserves unknown subtrees (which also gives us lossless round-tripping for dialect passthrough).

### 3.4 Encoding policy

The spec's general message-format section (carried forward into 4.0) mandates ISO 10646 UCS-2, big-endian, for all message content, while RFC 6455 requires WebSocket *text* frames to be UTF-8 — the one place the two documents genuinely collide. Plan review resolved the collision in favor of the spec's mandate. **On send, the default is UCS-2 BE on every transport**; over WebSocket, UCS-2 payloads therefore travel as *binary* frames. UTF-8 is available as an explicit per-peer configuration for peers that adopt the web-native reading, in which case WebSocket carriage switches to text frames. **The receive side matches the configured encoding** rather than sniffing liberally: a frame type or encoding that contradicts the peer's configuration is surfaced as a protocol error whose diagnostic names the likely misconfiguration, so operators see a setup problem immediately instead of hunting intermittent mojibake. Internally, everything is normalized to UTF-8 after decode. The same engine backs 2.8 (UCS-2 BE default, with per-peer dialect overrides — UTF-8 and little-endian variants have been observed in the field) and defines an explicit policy for characters outside the BMP, which UCS-2 cannot represent (default: replace with U+FFFD and warn; configurable to reject). The policy is exercised on the wire during M2 and against third-party peers at M8.

## 4. Architecture

### 4.1 Design principles

**Sans-I/O core.** All protocol logic — codecs, channel sequencers, heartbeat/keepAlive scheduling, resync heuristics, profile workflows — lives in a pure library (`imos_core`) that consumes and produces events and byte buffers. It never opens a socket, spawns a thread, reads a clock, or touches a file directly; `ITransport`, `IClock`, `ITimerQueue`, `IRandom`, `ILogger`, and `IMessageStore` are injected. Consequences: the identical core compiles for all six targets including WebAssembly; GoogleTest drives every profile deterministically (a fake clock makes a 60-second ack-timeout test run in microseconds); and both role libraries are thin compositions over the same engine, so a fix lands once.

**Embedded-first ergonomics for `imos`.** The primary deployment is inside MOS devices, so the default consumption model is a pull-mode event pump: the host calls `session.poll()` (or drains an event queue) from its own loop, and the library creates no threads of its own in this mode. A convenience runner that owns a thread and delivers callbacks exists for hosts that want it (and is what `incs`-based tools use), but nothing in the core requires it. Footprint matters: libxml2 is built with a minimal feature set (no HTTP/FTP/xpath modules beyond what `mosReqObjList` translation needs, no catalogs), exceptions and RTTI are confined to module internals, and allocations on hot paths are arena-friendly.

**Strict/lenient duality.** In production the parser is Postel-lenient exactly as the spec directs (unknown tags ignored and preserved; well-formedness required, validity not). In tests and in a debug configuration, every inbound and outbound message is additionally validated against the MOS 4.0 XSD (extracted from spec chapter 9 into our fixtures) so drift is caught early without punishing field interop.

**No surprises at boundaries.** No exceptions or C++ types cross the public C++ API in error paths (a `Result<T, Error>` type carries failures); nothing C++ crosses the C ABI at all.

### 4.2 Layering

```
+---------------------------------------------------------------+
|  Device / NCS application code                                 |
+---------------------------▲-----------------------------------+
|  Role APIs:  imos (device)     incs (newsroom)                 |
|  profile services P0–P7, RunningOrderModel, ObjectDirectory    |
+---------------------------▲-----------------------------------+
|  Session layer: per-peer PeerSession                           |
|   channels {mom, ro, aux} · in-flight-1 sequencer · ack/retry  |
|   messageID correlation & dedupe · heartbeat/keepAlive         |
|   resync (roReq on unknown IDs) · reconnect & passive mode     |
+---------------------------▲-----------------------------------+
|  Codec: typed message model ⇄ XML (libxml2)                    |
|   encoding engine UTF-8 ⇄ UCS-2BE · unknown-subtree retention  |
+---------------------------▲-----------------------------------+
|  Framing: 1 msg / WS frame · TCP stream deframer · SOAP env    |
+---------------------------▲-----------------------------------+
|  Transports (ITransport impls)                                 |
|   ws40_ixws (IXWebSocket client+server, TLS, deflate/zlib)     |
|   ws40_emscripten (WASM client)                                |
|   tcp28 (POSIX/Winsock)                                        |
|   ws3_soap (IXWebSocket HttpClient/HttpServer + libxml2)       |
+---------------------------------------------------------------+
   cross-cutting: IMessageStore · DialectProfile · config · log
```

### 4.3 Deliverable libraries and repository layout

The build produces `imos_core` (engine; not a supported public API by itself), `imos` and `incs` (role libraries, C++ API), `imos_capi` and `incs_capi` (C ABI shims), and optional modules `imos_compat28`, `imos_ws3`, `imos_plugin` (web-control messages), and `imos_store_file` (the default `IMessageStore`). Each is available as both a static archive and a shared library (§9). Tools built from the same tree: `incs-sim` (the NRCS simulator, §11), `mos-sim` (a scriptable device peer for testing `incs`), and `imos-cli` (message inspector, capture converter, store dump/repair).

```
ImaginaryMOS/
├─ 3rdparty                    # dependency sources, pinned to targeted release
├─ cmake/                     # toolchains, presets, package config
├─ core/                      # sans-IO engine: codec, session, profiles
│  ├─ codec/  ├─ session/  ├─ profiles/  ├─ encoding/
├─ imos/                      # device-role API
├─ incs/                      # NCS-role API
├─ capi/                      # C ABI (imos_c.h, incs_c.h)
├─ transports/                # ws40_ixws / ws40_emscripten / tcp28 / ws3_soap
├─ store/                     # IMessageStore + file backend
├─ dialects/                  # sofie / inews / enps / octopus profiles
├─ plugin/                    # ncs* web-control message module
├─ tools/                     # incs-sim, mos-sim, imos-cli
├─ tests/                     # unit / conformance / interop / fuzz / soak
├─ fixtures/                  # spec examples, DTD, XSD, dialect corpora
├─ examples/                  # minimal device, prompter, wasm demo
├─ reference/                 # development reference
├─ docs/                      # user and developer documentation
└─ LICENSES/                  # project license files
```

### 4.4 Channel and session model

A `PeerSession` owns the relationship with one remote party (one `mosID`↔`ncsID` pairing) and up to three logical **channels** — `mom`, `ro`, `aux` — matching the spec's port/channel scheme. Each message type knows its channel (captured from the per-message "Port" annotations in the spec during codec generation), so routing is automatic. Per channel, the sequencer enforces the spec's in-flight-of-one rule: a FIFO of pending outbound messages, exactly one awaiting `mosAck`/`roAck`, a retry timer (default 10 s, capped retries then link-fault escalation), and persistence of the pending set through `IMessageStore` so a crash mid-conversation resumes cleanly. Inbound, a sliding window of recently seen `messageID`s per peer suppresses duplicates from peer retries (the ack is re-sent; the application is not re-notified). The session also runs the Profile 0 machinery — heartbeat exchange with loop protection and GMT-offset extraction, keepAlive emission at the configured idle threshold (never faster than 30 s) — and the resynchronization heuristic of §3.3, surfaced to the application as a "resyncing" state rather than a stream of confusing callbacks.

Connection management is topology-aware. In 4.0 standard mode the session opens outbound WebSocket connections for the channels this role originates on and accepts inbound connections for the rest, matching them to the session by the URL's `mosID`/`ncsID`/`channel` parameters. In 4.0 passive mode, whichever side is configured "inner" opens all channels outward with `passive=true` and holds them; drops trigger immediate reconnection with small jitter and capped backoff, per the spec's "as quickly as possible." In 2.8 mode the same session logic binds to listener sockets and dials peers per the per-message port conventions. The application sees one uniform `PeerSession` regardless.

### 4.5 Message model and codec

Every message and structure in §3.2 gets a typed C++ representation (e.g. `msg::RoElementAction`, `msg::MosObj`, `structure::Item`). A code-generation step consumes the DTD/XSD extracted from the spec (plus a hand-maintained annotations file for channel assignment, request/response pairing, and profile membership) to produce struct skeletons, field metadata, and serializer scaffolding; the genuinely irregular parts — mixed-content `storyBody`, `mosPayload` as opaque ANY, `roElementAction`'s source/target structures — are hand-written against libxml2's reader/writer APIs. Three properties are non-negotiable: **lossless unknown-content retention** (unknown elements/attributes ride along in a raw-subtree slot and are re-emitted on serialization — required by the spec's unknown-tag rule and essential for dialect passthrough); **opaque foreign payloads** (`mosPayload`, including embedded NewsML and the `Read1stMEMasBody` pattern, is exposed both as a raw XML string and as an optional parsed view, never re-normalized by default); and **spec-example fidelity** (every example message in the spec document is extracted into `fixtures/spec-examples/` and must round-trip byte-cleanly modulo declared whitespace policy — the spec explicitly invites using its examples as test input).

libxml2 is used in stream/reader mode with hardening flags fixed on (§12). The encoding engine converts UTF-8⇄UCS-2BE without pulling in ICU, with the out-of-BMP policy from §3.4.

### 4.6 Role APIs: `imos` and `incs`

`imos` presents the device author with per-profile handler interfaces to implement — `IMachineInfoProvider` (P0), `IObjectSource` (P1/P3: enumerate, search, create-placeholder, act-on-object), `IRunningOrderSink` (P2/P4), `IControlSink` (P5: cue and READY/EXECUTE/PAUSE/STOP/SIGNAL), `IStorySink` (P4 `roStorySend`), and helpers to *originate* device-side messages (`sendObj`, `sendElementStat`, `sendItemReplace`, `requestStoryAction` for P7). Because most devices need it, the library includes an optional `RunningOrderModel`: a shadow RO store that applies the roConstruction family and `roElementAction` mutations transactionally, answers "what is the current playlist" queries, tracks per-element status for `roElementStat` emission, and — when handed an unknown-ID condition — drives the resync flow automatically. Devices may ignore it and consume raw messages.

`incs` mirrors this from the newsroom side: an RO authoring API whose `commit()` diffs the desired running order against the last acknowledged state and emits a minimal, spec-correct `roElementAction`/`roStorySend` sequence (using MOVE rather than delete+insert, as the spec demands for story-send-linked ROs); an object directory that maintains the `mosObj` metadatabase via Profile 1 sync and Profile 3 search; and P5/P7 counterparts. `incs` is a fully supported library, but its acceptance bar is driven by what `incs-sim` and the conformance suites need.

## 5. Transport implementations

### 5.1 MOS 4.0 over WebSocket (primary)

Built on IXWebSocket for both client and server roles on Linux, macOS, Windows, Android, and iOS. Responsibilities: URL construction/parsing for the `mosID`/`ncsID`/`channel`/`passive` query scheme plus vendor-extra parameters (preserved verbatim); HTTP Basic authorization headers on the upgrade per the spec's recommendation, with a pluggable credential provider and constant-time verification server-side; TLS via the platform-appropriate backend IXWebSocket supports (OpenSSL on Linux/Android, Secure Transport or OpenSSL on Apple platforms, Schannel/OpenSSL on Windows — pinned per target in M2), with the spec's certificate policy implemented as configuration: CA-trusted always accepted, self-signed acceptance as an explicit switch (plus optional pinning); permessage-deflate negotiated through IXWebSocket's zlib support, with inflation caps (§12). Server-side, one listener can serve many peers: the acceptor validates credentials and parameters, then hands the connection to the matching `PeerSession`'s channel slot, rejecting mismatched or duplicate channel claims. One MOS message per WebSocket message is enforced on send and tolerated-with-diagnostic on receive if a peer coalesces; per the §3.4 policy, payload frames are binary (UCS-2 BE) by default and text (UTF-8) only for peers configured that way.

### 5.2 MOS 2.8.x TCP (compat module `imos_compat28`)

IXWebSocket does not expose a supported raw-TCP server abstraction, so this module carries a small, dedicated cross-platform socket layer (POSIX sockets / Winsock2, non-blocking, level-triggered readiness) — a deliberate decision to keep IXWebSocket strictly on WebSocket/HTTP duty. On top of it: listener/dialer management for 10540/10541 (and 10542 where a dialect uses it), the UCS-2 BE wire codec, and a streaming deframer that tokenizes the byte stream into complete `<mos>` documents using a resynchronizing scanner with a hard per-message size cap, feeding each document to the shared codec. Heartbeats run per connection as 2.8 practice expects. Clean four-way socket shutdown is handled carefully (the spec singles out FIN_WAIT_2/CLOSE_WAIT pathologies), and half-open detection rides the heartbeat timer. The thirteen removed legacy messages are accepted and normalized here (§3.2). TLS on raw 2.8 sockets is nonstandard and out of scope.

### 5.3 MOS 3.x WebService (compat module `imos_ws3`)

Scope: interoperate with 3.x peers across the same profile workflows, not to re-create a general SOAP stack. The compatibility target is MOS 3.8.4: its WSDLs from mosprotocol.com are vendored as fixtures, and a verification spike at the start of M6 pins the exact operation names, bindings, and SOAP version against them and against any reachable reference behavior. Implementation approach: hand-rolled SOAP 1.1 envelope construction/parsing with libxml2 (the payloads are our existing MOS messages), transported over IXWebSocket's `HttpClient` for outbound calls and `HttpServer` for inbound, with gzip via zlib. The session layer's sequencing maps naturally onto request/response operations; long-poll or callback-style patterns, if the WSDLs demand them, are absorbed inside the transport so the core stays unchanged.

### 5.4 Platform transport matrix

| Platform | 4.0 WS | 2.8 TCP | 3.x WS | Notes |
|---|---|---|---|---|
| Linux (GCC, x86_64/aarch64) | client+server | client+server | client+server | Reference platform |
| macOS (Clang, arm64/x86_64) | client+server | client+server | client+server | |
| Windows (MSVC, x64/arm64) | client+server | client+server | client+server | Winsock2; Schannel or OpenSSL TLS |
| Android (arm64-v8a) | client+server | client+server | client | Server roles subject to app lifecycle caveats, documented |
| iOS (device+simulator) | client+server | client+server | client | Background-socket caveats documented |
| WebAssembly (Emscripten) | **client only** | — | — | Browser sandbox: no raw TCP, no inbound listeners |

The WebAssembly build swaps in `ws40_emscripten`, a transport over `emscripten/websocket.h`: client-only, callbacks on the main browser thread (the core's single-strand model absorbs this), `wss` required from secure pages. This is less of a limitation than it looks: MOS 4.0 **passive mode** requires only outbound client connections from the inner party, so a browser/WASM `imos` can participate as a fully functional passive-mode device — the topology the spec invented for exactly this kind of constrained network position. The deferred plug-in message module (§3.2) would additionally let WASM builds implement HTML5 web controls once it is scheduled. 2.8 and 3.x are declared unsupported on WASM, enforced at configure time.

## 6. `IMessageStore` and file-backed persistence

The store exists to satisfy the spec's durability recommendation (§3.3) and to make retry idempotent. The interface is deliberately small and transport-agnostic:

```cpp
class IMessageStore {
public:
  virtual Result<StreamHandle> open(PeerId, Channel) = 0;
  virtual Result<Seq>  append(StreamHandle, const OutboundRecord&) = 0;  // durable before send
  virtual Result<void> markAcked(StreamHandle, Seq) = 0;
  virtual Result<PendingCursor> pending(StreamHandle) = 0;              // replay after restart
  virtual Result<void> noteInbound(PeerId, MessageIdView, Timestamp) = 0; // dedupe window
  virtual Result<bool> seenInbound(PeerId, MessageIdView) = 0;
  virtual Result<void> compact(StreamHandle) = 0;
  virtual Result<void> sync(StreamHandle) = 0;
  virtual ~IMessageStore() = default;
};
```

The default `imos_store_file` backend implements it as one directory per peer with per-channel append-only journal segments. Each record is length-prefixed with a fixed header — sequence number, direction, `messageID`, timestamp, flags, and a zlib `crc32` over header+payload — followed by the serialized message, optionally deflate-compressed above a size threshold. Durability policy is configurable because embedded flash wear is real: `sync_each_append` (safest), `sync_on_send` (default: durable before the bytes hit the transport), or `batched(ms)`. Acked prefixes are dropped by segment rotation plus compaction; recovery scans tolerate a torn final record (truncate-and-continue) and verify CRCs; snapshots of the dedupe table and cursor state are written via write-temp-then-atomic-rename. Platform specifics are isolated in a tiny file-abstraction: `fdatasync`/`O_CLOEXEC` on POSIX, `FlushFileBuffers` and share-mode discipline on Windows. On WebAssembly the file backend compiles against Emscripten's FS (MEMFS by default, IDBFS mountable by the host page), and because the interface is public, applications on any platform may substitute their own store (e.g., an existing device database). A store inspection/repair mode lives in `imos-cli`.

## 7. Dialect support: Sofie, iNEWS, ENPS, Octopus

Real NCS installations deviate from the letter of the spec in small, breaking ways, so dialect handling is **data-driven configuration, not scattered `if`s**. A `DialectProfile` bundles declarative settings and a few hook points, selected per peer by configuration or inferred from `listMachInfo`/ID patterns: encoding policy and byte order; timestamp tolerance (timezone-less times, missing `Z`, fractional-second variants) mapped onto the spec §6 formats; `messageID` shape (numeric vs. free string) and reuse behavior; empty-element vs. omitted-element conventions; acceptance of the thirteen legacy sequence messages and which `roElementAction` operations the peer actually emits/understands; ack-latency expectations and retry tuning; heartbeat/keepAlive cadence; failover host lists ("buddy" style) and reconnection etiquette; per-vendor `mosSchema` registries with `mosPayload` passthrough guarantees; and size/character tolerances (control characters in slugs and similar field-report realities). The unknown-tag retention of §4.5 means vendor-private extensions survive a round trip through us untouched.

Because iNEWS, ENPS, and Octopus are proprietary and no NRCS is available to us, initial quirk catalogs are sourced from public artifacts and encoded as both dialect settings and simulator "personalities" (§11): **Sofie TV Automation's `mos-connection`** — a production-grade open-source MOS 2.8.5 implementation — serves as the reference third-party peer and a rich mine of documented workarounds; **mosromgr** (BBC) contributes real-world message corpora and, importantly, an independent running-order merge semantics we use as an oracle; **Superdesk** contributes NRCS-side behavior where its MOS-adjacent components apply. Each dialect ships three artifacts: a `DialectProfile` preset, a fixture corpus, and a conformance-delta test suite documenting exactly where the dialect departs from baseline. The honest residual risk — that field units will surface quirks absent from public code — is mitigated by making new quirks expressible in configuration/personality data without code changes wherever possible, and by `imos-cli`'s capture-import path that turns a customer wire log into a replayable simulator scenario (§11). This is recorded as risk R-2.

## 8. Public API surface

### 8.1 C++ API

Namespaces `imos::` and `incs::`, C++20 idioms at the boundary (`std::string_view`/`std::span` in, value types out, designated-initializer config structs, `Result<T, Error>` instead of thrown exceptions across the API). A flavor of device bring-up:

```cpp
imos::Config cfg{
  .identity   = {.mosID = "vtr7.playout.hq.example.mos"},
  .listMachInfo = {.manufacturer = "Imaginary", .model = "VTR-7",
                   .deviceType = imos::DeviceType::MOS,
                   .profiles = imos::ProfileSet::All()},
  .transports = {.ws40 = {.listen = {"0.0.0.0", 443},
                          .tls = imos::Tls::FromFiles(cert, key),
                          .allowSelfSignedPeers = true},
                 .tcp28 = {.enable = true}},          // legacy peers
  .store      = imos::FileStore::At("/var/lib/vtr7/mos"),
};
imos::Device dev(cfg);
dev.attach(myObjectSource);        // Profiles 1 & 3
dev.attach(myRunningOrderSink);    // Profiles 2 & 4 (or use RunningOrderModel)
dev.attach(myControlSink);         // Profile 5
while (running) dev.poll(std::chrono::milliseconds{10});   // pull mode; no library threads
```

Peers are added declaratively (endpoint URL or 2.8 host/ports, dialect preset, standard/passive topology), and every inbound event carries its `PeerSession` context. The C++ API is source-stable within a major version; binary stability is promised only at the C layer.

### 8.2 C ABI

`imos_c.h` / `incs_c.h` follow strict C-ABI discipline: opaque handles (`imos_device_t*`, `imos_peer_t*`), integer status codes with `imos_status_message()`, UTF-8 strings with explicit ownership (caller-owned inputs; library-owned outputs freed via `imos_string_free`), callback registration as function pointer + `void* user_data`, and an event-pump entry point (`imos_device_poll`) mirroring pull mode so single-threaded hosts and foreign-language runtimes integrate cleanly. No exception may cross the boundary (enforced by `noexcept` shims and a catch-all-to-status wrapper), no C++ type appears in any signature, and struct arguments are versioned by a leading `size` field to allow additive evolution. Versioning: `IMOS_ABI_VERSION`, `imos_version()` for the semantic version, ELF symbol versioning plus SOVERSION on Linux, `__declspec(dllexport)`/`visibility("default")` export macros with hidden-by-default builds everywhere. The C ABI covers both `imos` and `incs` fully at 1.0 — `imos` because it is the embedding and future-bindings surface, `incs` per plan review, so simulator hosts and NCS integrators get the same stability guarantee. Thread-safety contracts are documented per function and enforced with debug-mode assertions.

## 9. Build system, targets, packaging

**CMake ≥ 3.24 with presets** per official target. Dependencies (IXWebSocket, libxml2, zlib, GoogleTest) are pinned via `FetchContent` with an escape hatch to system/vcpkg packages (`IMOS_USE_SYSTEM_DEPS`); libxml2 is configured to a minimal module set and zlib may be satisfied by zlib-ng in compat mode. Key options: `IMOS_BUILD_SHARED` / `IMOS_BUILD_STATIC` (both by default, via object libraries so one configure produces both link styles), `IMOS_ENABLE_COMPAT28`, `IMOS_ENABLE_WS3`, `IMOS_ENABLE_PLUGIN_MSGS`, `IMOS_BUILD_TESTS`, `IMOS_BUILD_TOOLS`, plus sanitizer and coverage toggles. Installed trees include CMake package config files and pkg-config files; symbol visibility is hidden by default with explicit export macros.

| Target | Toolchain | Arches | Link styles | Packaging | CI execution |
|---|---|---|---|---|---|
| Linux | GCC 14 (Debian 13) · glibc ≥ 2.41 | x86_64, aarch64 | static + shared (`.so`, SOVERSION) | tarballs, pkg-config, CMake config | native x86_64; aarch64 native runner or qemu-user |
| macOS | Apple Clang · macOS ≥ 12 | arm64, x86_64 (+universal) | static + `.dylib` | tarball / optional pkg | native |
| Windows | MSVC v143+ · Windows ≥ 10 | x64, ARM64 | static (`/MT`,`/MD` variants) + DLL | zip with import libs | native x64; ARM64 cross-compile, smoke on ARM64 runner when available |
| Android | NDK r26+ · min API 28 | arm64-v8a | static + shared | AAR (prefab) | host unit tests + emulator smoke via adb |
| iOS | Xcode Clang · iOS ≥ 16 | device arm64; simulator arm64 + x86_64 | static (primary) + dynamic framework | **XCFramework** | simulator test bundle via `xcodebuild test` |
| WebAssembly | Emscripten 4.0.7 (pinned) | wasm32 | **static only** | `.a` + JS glue + demo page | core/codec tests under Node; WS smoke vs `incs-sim` via headless browser |

Notes: Linux builds and CI run in Debian 13 (trixie) containers, so the pinned GCC 14 / glibc 2.41 pair is exactly what the distribution ships; iOS/Android TLS backends and entitlements are pinned during M2's transport work; WASM builds single-threaded by default (no pthreads requirement); dynamic linking on WASM (SIDE_MODULE) is explicitly out of scope for 1.0. CI (GitHub Actions) runs the full matrix on every merge: build both link styles, run tests where the platform allows, plus dedicated jobs for ASan/UBSan/TSan, MSVC ASan, clang-tidy/format, and coverage.

## 10. Test strategy (GoogleTest / GoogleMock)

The stated requirement is *full coverage of each MOS profile*, which we interpret as two enforced gates, not one: a **semantic conformance gate** — every profile's complete message set and its spec-described workflow exercised in both roles over every applicable transport — and a **code-coverage gate** (llvm-cov/gcovr; core ≥ 90 % line at M5, ratcheted thereafter, with the codec's message-type coverage required at 100 %).

**Unit layer.** Codec tests per message type: round-trip of every spec example (extracted verbatim into `fixtures/spec-examples/`, per the spec's own invitation to use them for testing), field-format tests against §6 (timestamps, `objTB` 59.94-style timebases), encoding tests (UTF-8⇄UCS-2BE conversion, configured-encoding and frame-type enforcement per §3.4, BOM handling, out-of-BMP policy, non-Latin scripts), unknown-tag retention round-trips, and XSD validation in strict mode. Property-style generative tests shake serializer/parser symmetry. Session tests use GoogleMock fakes for `ITransport`, `IClock`, and `IMessageStore` to pin the reliability rules: in-flight-of-one per channel with cross-channel independence, retry-on-timeout schedules, duplicate-`messageID` suppression with ack re-emission, durable-before-send ordering against the store, heartbeat loop-avoidance, keepAlive cadence floors, resync-on-unknown-ID flows, and passive-mode reconnect behavior — all deterministic via the fake clock.

**Profile conformance layer.** For each Profile 0–7, a suite runs a real `imos` against a real `incs` over an in-memory transport pair and walks the spec's "General Work Flow" narrative plus negative branches: NACK paths with `statusDescription` propagation, out-of-order and wrong-state messages, oversized and malformed inputs, mid-workflow disconnects with store-driven resume, and profile-specific edges (`mosReqAll` pause-mode interleaving; `roElementAction` MOVE-vs-delete+insert on story-send-linked ROs; `roCtrl` scope selection via empty IDs; `Read1stMEMasBody` payload preservation; P6 FQID classification; P7 lease handling). The same suites are then re-executed parameterized over the three real transports — 4.0 WS over loopback (ws and wss, standard and passive), 2.8 TCP over loopback, and 3.x where applicable — so transport regressions can't hide behind the in-memory pair. The build emits a machine-readable **conformance matrix** (profile × message × direction × transport × result) as a CI artifact; any hole fails the build. Legacy-message normalization (the removed thirteen) gets its own suite in 2.8 mode.

**Adversarial and endurance layers.** libFuzzer harnesses target the four parsing surfaces — XML message ingest, the 2.8 stream deframer, the 4.0 URL/parameter parser, and store recovery — seeded from the fixture corpus and run nightly under ASan/UBSan. Soak jobs run a 24-hour `imos`↔`incs-sim` conversation with fault injection and assert zero sequence violations and bounded memory; benchmark jobs track codec throughput and large-RO handling (10 k-story `roListAll`, multi-megabyte `roStorySend`) for regression.

**Cross-platform execution** follows the CI column of the target table, with the invariant that the sans-I/O core and codec test binaries run on all six targets, and transport-inclusive suites run wherever the platform permits.

## 11. NRCS simulation and third-party interoperability (M8)

Because no commercial NRCS is available for interop testing, the M8 milestone is satisfied by a purpose-built simulator plus an open-source interop lab — designed together so that "passing M8" is a concrete, reviewable claim rather than a vibe.

**`incs-sim`** is a headless, scriptable NCS built on `incs` (and therefore also a continuous proving ground for `incs` itself). Scenarios are YAML: timed steps, expected inbound messages with field matchers, assertions, and branches. It layers **dialect personalities** — the §7 profiles plus deliberately misbehaving variants — onto every scenario, and offers **fault injection** as first-class scenario verbs: drop/hold a connection mid-sequence, withhold or duplicate ACKs, replay `messageID`s, deliver legacy messages in 4.0 mode, send oversized or malformed XML, stall mid-frame, and (for wss) present rotated or self-signed certificates. It speaks all three transports in both standard and passive topologies, and it can **record and replay**: `imos-cli` converts wire captures (pcap or line logs, with an anonymizer) into scenarios, which is the mechanism by which future field encounters with real iNEWS/ENPS/Octopus systems become permanent regression tests. A thin sibling, `mos-sim`, plays the device role for exercising `incs`.

**Open-source interop lab** (docker-compose, run nightly and at M8 gate): Sofie's **`mos-connection`** as the golden third-party 2.8.5 TCP peer, driven by a harness that walks our conformance scenarios against it in both directions; **mosromgr** as a semantic oracle — our `incs` emits roCreate/roElementAction/roStorySend streams for a corpus of running orders, mosromgr independently merges them, and the resulting RO must match our own `RunningOrderModel`'s state (and vice versa for parsing its bundled real-world samples); **Superdesk** exercised for NRCS-side flows to the extent its MOS-facing components allow, treated as best-effort enrichment rather than a gate. Sofie's server components are an optional end-to-end scenario if lab time permits.

**M8 exit criteria** (the definition of done for interop): all Profile 0–7 conformance scenarios green against `incs-sim` on 4.0-standard, 4.0-passive, 2.8, and 3.x transports with baseline and all four dialect personalities; the 2.8 suite green against `mos-connection` bidirectionally; mosromgr oracle parity across the RO corpus with zero divergences; 24-hour fault-injected soak with zero sequencing/durability violations; fuzzers at ≥ 72 h aggregate without open crashes; and a written dialect-delta report per dialect. Residual limitation stated plainly in the report: personalities encode publicly known behavior, not vendor certification.

## 12. Security

Transport security follows the spec's model: `wss` is the default and the documented recommendation for anything leaving a protected network, `ws` permitted for lab/local use; CA-issued certificates always accepted; self-signed acceptance implemented as the explicit, logged configuration switch the spec calls for, with optional certificate pinning on top; credentials carried in HTTP Basic authorization headers on the WebSocket upgrade rather than in URLs, verified in constant time, with a pluggable credential provider for device integration.

Parsing surfaces are hardened by construction. libxml2 runs with network access disabled, DTD loading and entity substitution off, and enforced limits on document size, nesting depth, and entity expansion; the 2.8 deframer and the 4.0 URL parser enforce hard size caps before any XML work begins; permessage-deflate and stored-message inflation are bounded to defeat decompression bombs; and all four surfaces are the standing fuzz targets of §10. Messages that fail hardening are rejected with diagnostics and never reach profile logic.

Supply chain and lifecycle: all dependencies pinned by hash with an SBOM emitted per release; zlib and libxml2 tracked for CVEs with a stated patch-release SLA (these two have steady advisory cadences and are the main watch items); reproducible builds where toolchains allow; secrets never logged, with message-body logging off by default and redaction filters when enabled. A short threat-model document (spoofed peers, malicious NCS, hostile network, compromised store file) is an M2 deliverable and is revisited at M8.

## 13. Milestones and sequencing

Milestones are cumulative and each ends with its tests in CI. The user-visible numbering is chosen so that **M8 is the interoperability milestone**, matching the project's framing of "M8 interop testing." Durations are indicative engineer-weeks on the critical path assuming three to four engineers working in parallel streams (core/protocol, transports/platforms, test/tooling); the total lands around two quarters to GA, and the estimates should be treated as planning shapes, not commitments.

| Milestone | Contents | Exit criteria (abridged) | Indicative |
|---|---|---|---|
| **M0 — Bootstrap** | Repo, CMake presets + dependency pinning, CI skeleton across all six targets (build-only), coding standards, DTD/XSD + all example messages extracted from the spec into `fixtures/` | Green build matrix; fixtures verified against spec | 2 wk |
| **M1 — Core foundations** | Message model + codegen, codec for the full §3.2 vocabulary incl. legacy-13 parsing, encoding engine, framing (WS/TCP deframer spike), session skeleton, `IMessageStore` + file backend | Every spec example round-trips; store crash-recovery tests green; fuzzers running | 4–5 wk |
| **M2 — 4.0 transport + Profile 0** | `ws40_ixws` client/server with TLS + auth + deflate, standard & passive topologies, `ws40_emscripten`, Profile 0 end-to-end both roles, threat model v1, §3.4 wire-encoding policy (UCS-2 default) exercised end-to-end | P0 conformance green on WS across desktop targets + WASM client; passive reconnect tests green | 3–4 wk |
| **M3 — Profiles 1–2** | Object workflow, roConstruction family, `RunningOrderModel`, resync heuristic, `incs` RO authoring/diff | P1+P2 conformance green (in-memory + WS); store-backed resume test green | 4 wk |
| **M4 — Profiles 3–4** | Search family, object create/action, `mosItemReplace`, `roStorySend` incl. `Read1stMEMasBody`, `roReqAll`/`roListAll` | P3+P4 conformance green; NewsML-payload preservation tests green | 3–4 wk |
| **M5 — Profiles 5–7** | `roItemCue`/`roCtrl`, FQID utilities (P6), `roReqStoryAction` (P7) | P5–P7 conformance green; coverage gate ≥ 90 % core | 2–3 wk |
| **M6 — Legacy transports** | `imos_compat28` (TCP, UCS-2, legacy-13 normalization), `imos_ws3` (3.8.4 WSDL spike → SOAP transport), conformance suites re-run per transport | Full matrix green on 2.8 and 3.x (3.8.4) where applicable; 3.8.4 bindings pinned | 4–5 wk |
| **M7 — Dialects, C ABI freeze, packaging** | Four `DialectProfile`s + delta suites, C ABI complete and frozen, packaging artifacts for all targets (XCFramework, AAR, DLL/zip, tarballs, wasm bundle), docs draft | ABI review signed off; install-and-link smoke tests from a clean consumer project on every target | 3–4 wk |
| **M8 — Interoperability** | `incs-sim`/`mos-sim` feature-complete (personalities, fault injection, record/replay), OSS interop lab (mos-connection, mosromgr oracle, Superdesk), soak + extended fuzz | The §11 exit criteria, in full | 4 wk |
| **M9 — Hardening + RC** | Bug burn-down, performance passes, security review vs. threat model, documentation complete, conformance report generator | Release candidate; zero known sequencing/durability defects | 2–3 wk |
| **M10 — GA 1.0** | Release, versioning/support policy, maintenance cadence (CVE SLA, dialect-quirk intake process) | Tagged 1.0; support policy published | 1 wk |

Sequencing rationale: the 2.8 deframer and UCS-2 work get a *spike* in M1 even though the full compat module lands in M6, because stream deframing and encoding shape codec APIs and retrofitting them is expensive. The 4.0 WebSocket transport precedes profile breadth so every profile is developed against a real transport from M3 onward. Dialects precede M8 so interop runs with personalities from day one of that milestone.

## 14. Risks and resolved questions

| ID | Risk | Mitigation |
|---|---|---|
| R-1 | Spec ambiguity on 4.0 wire encoding (UCS-2 mandate vs. RFC 6455 UTF-8 text frames) causes peer mismatches | Follow the UCS-2 mandate by default (binary WebSocket frames), with UTF-8 as a per-peer option; receive expects the configured encoding (§3.4); exercised against peers in M2/M8 |
| R-2 | Proprietary dialects (iNEWS, ENPS, Octopus) hold quirks not visible in public sources | Data-driven `DialectProfile`s, capture-import-to-scenario pipeline, explicit dialect-delta reports; treat vendor certification as post-1.0 |
| R-3 | MOS 3.x WSDL/operation variance across published minor versions | Target fixed at 3.8.4; M6 opens with a verification spike against its WSDLs; transport isolates any long-poll/callback patterns from the core |
| R-4 | UCS-2 cannot represent non-BMP characters (emoji in slugs will happen) | Explicit replace-or-reject policy, tested, documented per dialect |
| R-5 | IXWebSocket server path under many peers / hostile input | Device-side peer counts are naturally small; connection caps, upgrade-time validation, fuzzed parameter parsing, soak with fault injection |
| R-6 | WASM constraints (client-only, main-thread callbacks) surprise integrators | Positioning as passive-mode/plug-in target with an enforced support matrix at configure time |
| R-7 | Windows ARM64 and Android/iOS CI execution gaps | Cross-compile always; on-device/emulator smoke tiers; expand as runner availability allows |
| R-8 | libxml2/zlib CVE cadence; dependency drift | Pinned deps + SBOM + patch SLA (§12); minimal libxml2 feature build shrinks exposure |
| R-9 | Embedded footprint creep | The size-critical targets (WASM, iOS, Android) build statically, so dead-code elimination plus genuinely link-optional modules bound footprint; no CI size budgets needed |
| R-10 | `roElementAction`/RO-model semantic corner cases diverge from field behavior | mosromgr as an independent oracle (§11); spec-example fidelity gate |

The draft's open questions were resolved at plan review and are folded into the sections above; the decisions of record are: **OQ-1** the 4.0 wire default follows the spec's UCS-2 mandate over binary WebSocket frames, with UTF-8 as a per-peer configuration and receive-side expectations matching the configured encoding (§3.4); **OQ-2** the MOS 3.x compatibility target is 3.8.4 (§5.3); **OQ-3** the `incs` C ABI ships at 1.0 alongside `imos` (§8.2); **OQ-4** platform floors are Android API 28, iOS 16, Windows 10, macOS 12, and glibc 2.41, with the Linux toolchain pinned to what Debian 13 ships (GCC 14) and Emscripten pinned at 4.0.7 (§9); **OQ-5** the license is Apache-2.0 (the dependency set — IXWebSocket BSD-3, libxml2 MIT, zlib License, GoogleTest BSD-3 — is compatible); **OQ-6** the plug-in (HTML5 web control) module is low priority and stays unscheduled until stakeholders state otherwise (§3.2); **OQ-7** no host logging/metrics framework integration is required — the injected `ILogger` interface is the contract (§4.1).

## 15. Documentation and final deliverables

Shipping alongside the libraries: Doxygen-generated API references for the C++ and C surfaces; an integrator's guide per profile that mirrors the spec's workflow narratives and shows both roles; a dialect handbook capturing each `DialectProfile` and its delta suite; a porting/embedding guide (pull-mode integration, store tuning for flash, footprint knobs, threading contracts); transport operations notes (certificate policy, passive-mode deployment, firewall guidance straight from the spec's model); the machine-generated conformance matrix and M8 interop report; and worked examples — a minimal device, a prompter-style consumer of `roStorySend`/`roItemCue`, an `incs-sim` scenario cookbook, and a browser demo of the WASM passive-mode client. Post-1.0 maintenance is governed by the support policy from M10: semantic versioning anchored on the C ABI, a security patch SLA, and a standing intake path that converts field captures into dialect regression scenarios.

---

*Prepared against the MOS Protocol v4.0.1 specification included in this project; profile and message inventories in §3.2, transport behavior in §3.1/§5, and reliability rules in §3.3 were extracted directly from that document. The canonical online reference remains mosprotocol.com.*
