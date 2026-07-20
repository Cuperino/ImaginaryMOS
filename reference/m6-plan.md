# M6 Plan — Legacy Transports (`imos_compat28`, `imos_ws3`)

**Milestone:** M6 (master plan §13) — the first milestone after the M3–M5
profile arc; M7 packaging already landed early.
**Deliverables (master plan):** `imos_compat28` (2.8.x TCP, UCS-2, legacy-13
normalization), `imos_ws3` (3.8.4 WSDL spike → SOAP transport), conformance
suites re-run per transport.
**Exit criteria:** full matrix green on 2.8 and 3.x (3.8.4) where applicable;
3.8.4 bindings pinned.
**Spec grounding:** `reference/MOS Protocol 2.8.5 (Final).html` and
`reference/MOS Protocol 3.8.4 (Current).html` (both vendored 2026-07-18 from
mosprotocol.com at the user's direction — the site's TLS chain is broken, so
these local copies are the source of truth, same rule as the 4.0 HTML).

Core principle, restated from master plan §4.4/§5: **the sans-I/O core does
not change.** Both modules are `ITransport` implementations plus host-side
pump glue, exactly like `ws40_ixws`. `PeerSession` already accepts the
legacy-13 (normalize-with-diagnostic on receive, no send API), already runs
per-channel in-flight-of-one, and already speaks any `WireEncoding`.

## 1. `imos_compat28` — MOS 2.8.x TCP (plan §5.2)

### 1.1 Facts pinned from the 2.8.5 spec

- **Ports and directions** ("Message Transport"): the **NCS listens** on the
  MOS Lower Port **10540** (Media Object Metadata — our `mom`); the **MOS
  listens** on the MOS Upper Port **10541** (Running Order — our `ro`).
  Port **10542** carries the `mosReqObjList` family (our `aux`); the NCS (or
  NCS client) *initiates* those queries, so the **MOS listens** on 10542.
  Ports are examples/site-configurable since 2.5 — all three are config knobs.
- **Bidirectional sockets, ack on arrival socket**: both ports are used
  bi-directionally; "the acknowledgment of a message will be sent down the
  same socket on which the original message was received."
- **Discrete-session tolerance**: peers may open a fresh socket per message
  ("though this is not very efficient") and must be handled; each listener
  must accept **multiple simultaneous connections** (two-ports-four-sockets
  note). Recommended practice is one long-lived socket per port.
- **Sequencing**: one message in flight per *port*, ports independent —
  identical to the session's per-channel in-flight-of-one rule.
- **Wire encoding**: ISO 10646 UCS-2, **big endian**, no BOM, no length
  framing — the byte stream is delimited only by the XML (`StreamDeframer`).
- **Heartbeats**: optional, per machine pair, "at regular intervals";
  practice runs them per connection. Half-open detection rides this timer.
- **Shutdown**: clean four-way close is called out (FIN_WAIT_2 / CLOSE_WAIT
  pathologies) — always `shutdown(SHUT_WR)` then drain to EOF before close.
- **Legacy thirteen**: parsed by the codec since M1; `PeerSession` already
  accepts-and-normalizes via `codec::normalizeLegacy` and refuses to send
  them. The 2.8 suite gets the dedicated normalization battery (plan §10).

### 1.2 Design

New sources under `transports/tcp28/`, folded into the existing
`imos::transports` library target (same pattern as `ws40_ixws`; no separate
CMake component — "imos_compat28" is the plan's *module* name, and the
packaging story already ships one transports archive).

- **Socket layer** (`tcp28_socket.{h,cpp}`, internal): thin cross-platform
  wrapper — POSIX sockets / Winsock2 (WSAStartup refcount), non-blocking,
  level-triggered readiness via `poll()`/`WSAPoll`, IPv4+IPv6. No IXWebSocket
  involvement (deliberate, plan §5.2). Guarded by `IMOS_ENABLE_COMPAT28`
  (already an option; ON by default, OFF under Emscripten).
- **`Tcp28Endpoint`** (`transports/include/imos/transports/tcp28.h`):
  *one* class for both roles — unlike WS there is no client/server split;
  each role dials some channels and listens on others:
  - Device role: **dials** peer `mom` (10540), **listens** `ro` (10541) and
    `aux` (10542).
  - NCS role: **listens** `mom`, **dials** `ro` and `aux`.
  - `aux` is optional (off unless the host enables it — P3 peers only).
  Config: role, peer host, per-channel ports, local bind address,
  `WireEncoding` (default `Ucs2Be`; `Ucs2Le` observed in the field is a
  dialect knob), `DeframerOptions` cap, reconnect policy (reuses
  `ReconnectPolicy`), heartbeat is the session's business (config unchanged).
- **Threading contract**: identical to ws40 — one internal I/O thread runs
  the poll loop and only touches sockets and queues; the host calls
  `pump(PeerSession&)` from its own loop to drain connectivity events and
  deframed documents into the session, and `send()` enqueues to the I/O
  thread. The core is never touched from the I/O thread.
- **Send path**: `send(channel, frame)` writes the already-encoded bytes on
  the socket currently bound to that channel — for a listening channel, the
  most recent *live* accepted connection (reply-on-arrival-socket falls out
  naturally because acks are sent while that connection is current); for a
  dialing channel, the held outbound connection (dialed on demand /
  reconnected per policy). Not connected → `ErrorCode::NotConnected`
  (session retry logic already covers the gap).
- **Receive path**: per-connection `StreamDeframer` (wire-encoding aware);
  each complete document is queued as one inbound frame for `pump()`.
  Deframer discards/overflows surface as transport diagnostics.
- **Accept policy**: multiple simultaneous connections per listener are
  accepted (spec requires ≥2); the newest connection that *delivers bytes*
  becomes the channel's reply socket. Idle extra connections are harmless
  and reaped on EOF. A per-listener connection cap (default modest, e.g. 8)
  bounds resource use (plan §12 posture).
- **Shutdown**: `stop()` performs orderly `shutdown(write)` + bounded drain
  + close on every socket; EOF from the peer closes read side and reaps.

### 1.3 Tests

- `tests/transports/tcp28_transport_test.cpp` (real sockets over loopback,
  registered like the ws40 suite):
  - device↔NCS Profile 0 both roles over real 10540/10541-style listeners
    (ephemeral ports in tests);
  - the legacy-13 normalization battery in 2.8 mode — raw legacy documents
    written onto the socket, normalized `roElementAction`/`roElementStat`
    observed at the application seam, plus the diagnostic;
  - discrete-socket-per-message tolerance (fresh connection per message);
  - split/coalesced TCP delivery (byte-dribble and multi-doc writes) through
    the deframer;
  - reconnect-after-drop on a dialed channel; ack-on-arrival-socket check;
  - conformance re-runs over the TCP pair for **every wire profile**
    (extended past the original P1/P2 scope on 2026-07-19): P1 object
    workflow, P2 authoring/status, P3 three-channel with real aux-port
    traffic (schema + search + paging + placeholder create), the P4
    agency-byte gate in UCS-2 BE *and* LE, P5 item control, and P7 story
    actions — P6 defines no wire messages.
- `tools/conformance_matrix.py` grows transport axes for tcp28 and ws3:
  every wire profile (0–5, 7) must have loopback evidence on each, plus the
  legacy battery and the WSDL pin.

## 2. `imos_ws3` — MOS 3.x WebService (plan §5.3)

### 2.1 Spike findings (bindings pinned — this section is the decision of record)

Source: the full WSDL is **embedded in the 3.8.4 spec HTML** (§8). Quirk of
record: the 3.8.4 document ships a WSDL whose comment and section header say
**“MOS.WSDL version 3.8.2 September 22, 2006”** — 3.8.2 *is* the current
bindings artifact for the 3.8.4 compatibility target.

- **Service surface**: one service `MOSWebService`, one port
  `MOSWebServiceSoap`, **SOAP 1.1** (`http://schemas.xmlsoap.org/wsdl/soap/`
  binding, transport `http://schemas.xmlsoap.org/soap/http`),
  **document/literal**, namespace `http://mosprotocol.com/webservices/`,
  `soapAction = http://mosprotocol.com/webservices/<operation>`.
  Encoding is **UTF-8**; **one TCP port** for everything (channels collapse).
- **24 operations** (request/response each): heartbeat, reqMachInfo, mosObj,
  mosReqObj, mosReqAll, mosListAll, mosObjCreate, mosReqObjList,
  mosReqSearchableSchema, mosReqObjAction, roCreate, roReplace, roDelete,
  roReq, roMetadataReplace, roElementAction, roElementStat, roReadyToAir,
  mosItemReplace, roReqAll, roStorySend, roItemCue, roCtrl,
  roReqStoryAction. Pure replies (listMachInfo, mosAck, mosObjList,
  mosListAll-as-reply, roAck, roList…) are **not operations** — they are the
  operation *outputs* (WSDL service documentation says so explicitly).
- **Response mapping** (from the WSDL schema):
  - ack-only: mosObj/mosReqAll/mosListAll/mosObjCreate/mosReqObjAction →
    `mosAck_type`; roCreate/roReplace/roDelete/roMetadataReplace/
    roElementAction/roElementStat/roReadyToAir/mosItemReplace/roStorySend/
    roItemCue/roCtrl/roReqStoryAction → `roAck_type`.
  - data-carrying: heartbeat → `heartbeat_type` (echo); reqMachInfo →
    `MachInfo_type`; mosReqObj → `mosObj_type` **+** `mosAck_type`;
    mosReqObjList → `mosObjList_type`; mosReqSearchableSchema →
    `mosListSearchableSchema_type`; roReq → `roList_type` + `roAck_type`;
    roReqAll → `ArrayOfRo_type` + `roAck_type`.
- **Body shape**: each request body is
  `<op xmlns="…/webservices/"><mosHeader_input>mosID|ncsID|messageID
  </mosHeader_input><op_input>…classic message children…</op_input></op>`;
  responses are `<opResponse><opResult>…</opResult>[<ack…Result>…]</opResponse>`.
  **`_type` container quirk**: elements that repeat in classic MOS are
  restructured as a singular container holding repeated `<name>_type`
  children (e.g. `<mosExternalMetadata><mosExternalMetadata_type>…`,
  `objPath`/`objProxyPath` under `objPaths`). The classic↔SOAP transform is
  therefore **table-driven from the WSDL schema**, not purely mechanical.
- **Session mapping**: the HTTP response *is* the ack (plus any data reply),
  which maps 1:1 onto the sequencer's in-flight-of-one; no long-poll or
  callback pattern exists in these bindings — nothing leaks into the core.

### 2.2 Design

- **Fixture**: `tools/extract_wsdl.py` extracts the WSDL verbatim-modulo-
  markup from the 3.8.4 HTML into `fixtures/wsdl/MOSWebService-3.8.2.wsdl`
  (validated well-formed at extraction; CI diff-gate alongside the existing
  fixtures rule). Never hand-edit.
- **Tables, not codegen** (amended at implementation): the WSDL is a frozen
  2006 artifact, so the operation recipes (header name, hoisted fields,
  input wrapper, reply/ack-result kinds) and the ArrayOfX container set are
  **hand-pinned in `ws3_transform.cpp`**, and the ws3 unit suite
  cross-checks them against the vendored fixture at test time (24 ops, all
  names known message types) — same fidelity as build-time codegen without
  the build machinery. Notable pinned quirks: `roDelete` uses a bare
  `mosHeader` (not `mosHeader_input`), `roReqStoryAction`'s input is
  `roStorySend_input`, `story_type`'s item array container is capitalized
  `Item`, and classic attributes (`roElementAction operation`,
  `roElementStat element`, `mosReqObjList username`…) become hoisted sibling
  elements — or leading input elements for `mosReqObjAction`.
- **`Ws3Endpoint`** (`transports/include/imos/transports/ws3.h`, sources in
  `transports/ws3_soap/`, folded into `imos::transports`, guarded by
  `IMOS_ENABLE_WS3`): both roles again in one class — each side hosts the
  service (IXWebSocket `HttpServer`) *and* calls the peer's
  (`HttpClient`), gzip via zlib, optional Basic auth reusing the ws40
  credential machinery. TLS per `TlsOptions` where IXWebSocket's HTTP stack
  supports it.
- **Outbound**: `send(channel, frame)` (channel ignored — one pipe) decodes
  the UTF-8 wire bytes, parses the `<mos>` document, wraps per the operation
  table, POSTs with `SOAPAction`. The HTTP response is unwrapped into one or
  two synthesized `<mos>` reply documents (envelope rebuilt from session
  IDs + the request's `messageID`) and queued inbound for `pump()`.
  Data-reply-bearing responses deliver the data message (the session treats
  it as the terminal reply, clearing in-flight); the redundant ack result is
  dropped. Unsupported roots (4.0-only messages, e.g. the mosItemReplace →
  P3 replaceItem nuance aside, anything absent from the 24) are rejected
  with `ErrorCode::Unsupported` + diagnostic — profile negotiation keeps
  them off this transport in practice.
- **Inbound**: the HTTP handler unwraps the request into a `<mos>` document,
  queues it, and **parks the HTTP exchange** keyed by `messageID`. The
  host's pump feeds the session; when the session emits the matching reply
  via `send()`, the endpoint routes it into the parked exchange as the SOAP
  response (synthesizing the ack result element next to a data result where
  the WSDL wants both) instead of a new outbound call. A parked exchange
  that outlives a timeout answers with a SOAP fault.
- **Sessions built on ws3 run `WireEncoding::Utf8`** end to end, with
  `keepAliveIntervalMillis = 0` (keepAlive is not a 3.x operation; liveness
  is the heartbeat op). `Ws3Endpoint::setPeerServiceUrl` re-points the peer
  URL after start — two ephemeral endpoints can't know each other's URLs up
  front, and it covers failover-style re-pointing.

### 2.3 Tests

- WSDL fixture extraction gate (well-formed, 24 operations, pinned
  namespace/soapActions — a unit test over the vendored file).
- Transform unit tests: classic↔SOAP round-trip for representative messages
  incl. the `_type` container cases (mosExternalMetadata, objPaths) and the
  spec's own SOAP samples where extractable.
- `tests/transports/ws3_transport_test.cpp`: loopback HTTP pair running
  Profile 0 (heartbeat/reqMachInfo), a P1 push (mosObj → mosAck in the
  response), and parked-exchange timeout → SOAP fault — plus (extended
  2026-07-19) facade-level re-runs of **every wire profile** over SOAP:
  P1 push/fetch/NACK/batch (mosListAll's ArrayOfMosObj container on the
  wire), P2 authoring/mutation/status, the P4 agency-byte gate through the
  roStorySend_input transform, P5 item control, P7 story actions, and the
  P3 advanced workflow (schema/search/paging via mosReqObjList ⇄
  mosObjList's mosList⇄list rule, placeholder create with the
  objID-in-statusDescription ACK).

## 3. Work order

1. compat28 socket layer + endpoint + loopback P0 test (§1.2, §1.3 first two
   bullets).
2. Legacy-13 battery + deframer-abuse + reconnect tests; P1/P2 re-run over
   TCP.
3. WSDL extraction tool + fixture + pin test (§2.2 first two bullets).
4. ws3 endpoint + transform tables + loopback suite.
5. Matrix/CI/docs/README close-out.

Out of scope, unchanged: TLS on raw 2.8 sockets (nonstandard); WASM builds
of either module (configure-time unsupported, plan §5.4); dialect-specific
tolerances (M7 — the dialect knobs land where §7 says, not here).
