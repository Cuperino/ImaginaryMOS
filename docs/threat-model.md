<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# ImaginaryMOS Threat Model — v1 (M2)

Deliverable of milestone M2 (plan §12/§13); revisited at M8 with the interop
lab findings. Scope at v1: the sans-I/O core, the MOS 4.0 WebSocket transport
(`ws40_ixws`, `ws40_emscripten`), and the file-backed message store. The 2.8
TCP and 3.x SOAP transports are assessed when they land (M6).

## Assets

- **Editorial traffic and metadata** — running orders, story bodies, object
  metadata. Confidentiality is usually low-to-medium (pre-broadcast editorial
  can be embargo-sensitive); integrity and availability are high: corrupted
  or dropped sequences disrupt on-air playout.
- **Credentials** — HTTP Basic username/password pairs presented on the
  WebSocket upgrade; TLS private keys configured into the server.
- **The store** — journals under the device's data directory buffer
  unacknowledged messages across restarts; they contain message bodies.
- **Device availability** — `imos` is embedded in playout-critical devices;
  a crash or a wedge is an on-air incident.

## Trust boundaries

1. **Network peer ↔ transport** (WebSocket upgrade, frames)
2. **Transport ↔ sans-I/O core** (decoded frames, connectivity events)
3. **Core ↔ application** (typed messages, events)
4. **Store file ↔ core** (journal records read back at startup)
5. **Configuration ↔ library** (endpoints, credentials, certificate policy)

## Adversaries and scenarios

### T1 — Spoofed peer (network attacker connecting to our listener)

An attacker who can reach the listen port claims a `mosID`/`ncsID`/`channel`
on the upgrade URL.

Mitigations in place (M2):

- Registered-peer check: connections claiming an unknown pairing are closed
  (application close code 4404) before any frame reaches the codec. The
  rejection reason does not confirm which identities exist.
- HTTP Basic credentials on the upgrade, verified through the pluggable
  `ICredentialVerifier`; the bundled `FixedCredentialVerifier` compares in
  constant time (`imos::ws40::constantTimeEquals` — length differences fold
  into the accumulator; both username and password are always compared).
- Duplicate-channel claims are rejected (4409), so an attacker cannot evict
  a legitimate connection by re-claiming its channel slot; the standing
  connection keeps the claim.
- `wss` with the spec's certificate policy: CA-trusted always accepted;
  self-signed certificates admissible **by default** (the spec expects MOS
  devices/NCSes to accept them), with the spec-recommended disable option
  (`TlsOptions::allowSelfSignedPeers = false`) for deployments that require
  CA-trusted or pinned peers; optional pinning
  (`TlsOptions::pinnedPeerCertFile`) overrides both. Note the consequence:
  under the default, `wss` provides confidentiality and integrity against
  passive attackers but peer identity rests on Basic auth (and pinning where
  configured), not on the certificate chain.

Known limitations (accepted at M2, documented):

- IXWebSocket completes the WebSocket handshake before our validation runs;
  rejection is a post-upgrade close, not an HTTP 401/404. An attacker
  learns that a MOS endpoint exists. Revisit at M8 if fingerprinting
  matters to deployments.
- Basic credentials authenticate the *connection*, not individual messages.
  Message-level `mosID`/`ncsID` consistency against the claimed channel
  identity is session policy, tightening in M3 when the profile services
  route by peer identity.
- No lockout/rate limit on failed upgrades; deployments should front
  Internet-exposed listeners with standard rate limiting. Peer counts on
  devices are naturally small (R-5).

### T2 — Malicious or compromised NCS (authenticated peer sends hostile data)

A peer with valid credentials — or on an unauthenticated lab link — sends
malformed, oversized, or semantically hostile messages.

Mitigations:

- **Hardened parsing surfaces** (plan §12), all four under standing
  libFuzzer harnesses seeded from the fixture corpus (`linux-fuzz` preset;
  ASan+UBSan):
  1. XML ingest — libxml2 in reader mode with network access, DTD loading,
     and entity substitution off; document-size cap (default 4 MiB) enforced
     before parsing, depth cap during it.
  2. 2.8 stream deframer — hard per-message cap before any XML work,
     resynchronizing scanner.
  3. 4.0 connection-URL parser — 8 KiB cap before decoding; duplicate,
     empty, malformed, and NUL-bearing parameters rejected; accepted parses
     are round-trip stable (fuzz-verified).
  4. Store recovery — CRC verification, torn-final-record truncation.
- **Frame-type/encoding policy** (§3.4): a frame whose WebSocket type
  contradicts the peer's configured encoding is reported with a diagnostic
  naming the misconfiguration and never delivered — hostile frames cannot
  select a lenient decode path.
- **Coalesced frames** are split only when the result is a clean sequence of
  well-formed documents; anything else surfaces as a protocol error.
- **Duplicate `messageID` suppression** keeps replayed messages from
  re-triggering application handlers (re-acks are emitted only for bare
  ACK types, never regenerated rich replies).
- **Sequencing invariants** (in-flight-of-one, response-set correlation,
  messageID echo check) reject out-of-protocol replies.
- permessage-deflate is negotiated through IXWebSocket's zlib path;
  IXWebSocket enforces decompression buffering per message. Inflation caps
  on stored messages apply in the store layer.

Open at v1: per-message-type semantic limits (e.g. story counts) belong to
the profile services (M3–M5). Deflate amplification relies on IXWebSocket's
internal bounds; an explicit inflation ceiling knob is tracked for M6
alongside the 2.8 transport caps.

### T3 — Hostile network (interception, tampering, replay, DoS)

- `wss` is the default recommendation for anything leaving a protected
  network (`ws` permitted for lab/local use); the TLS backend is pinned to
  OpenSSL on Linux, Android, Windows, and macOS (uniform certificate
  behavior across targets), with iOS on Secure Transport (no system
  OpenSSL) — decided at M2, `cmake/IMOSDependencies.cmake`.
- Credentials ride the `Authorization` header, never the URL, so they don't
  land in access logs or proxies' request lines (spec recommendation).
- Passive mode exists precisely so inner parties expose no inbound ports;
  reconnects use capped exponential backoff with jitter, so a flapping
  network cannot amplify into a reconnect storm.
- Replay across reconnects is absorbed by the dedupe window; retry behavior
  is idempotent by design (§3.3).
- Half-open connections are detected by heartbeat/keepAlive cadence at the
  session layer, not by transport pings (WebSocket ping is disabled;
  liveness is protocol-visible).
- DoS: connection cap (`ServerConfig::maxConnections`, default 32),
  handshake timeout, size caps before parse. A determined flood still
  saturates the listener — front with infrastructure controls when exposed.

### T4 — Compromised store file (attacker with filesystem access)

An attacker who can write the journal directory can already tamper with the
device; the store's job is *integrity detection and crash honesty*, not
secrecy:

- Every record carries a CRC32 over header+payload; recovery rejects
  corrupt records and truncates torn tails rather than replaying garbage.
- Snapshots are written temp-then-atomic-rename; a crash never leaves a
  half-written snapshot in play.
- Replayed payloads still pass through the full codec limits on load —
  a tampered journal cannot smuggle an oversized document past the parser.
- No secrets are stored: credentials live in host configuration, never in
  journals. Message bodies *are* present; deployments with confidentiality
  requirements should place the store on protected storage (documented in
  the porting guide).
- CRC32 is corruption detection, not authentication. Authenticated journal
  records (MAC) are out of scope for 1.0; noted for the M8 revisit.

### T5 — Supply chain

- libxml2, zlib, IXWebSocket, GoogleTest pinned as git submodules by hash;
  libxml2 built with a minimal module set (no HTTP, no catalogs, no
  XInclude) shrinking exposure; zlib/libxml2 tracked for CVEs with a patch
  SLA (plan §12); SBOM emission lands with the release pipeline (M7).
- The Emscripten toolchain is pinned (4.0.7) for reproducible WASM builds.

## Logging and secrets

`ILogger` output never includes credentials. Message-body logging is off by
default project-wide; transport diagnostics quote metadata (channel names,
close reasons, byte counts), not payloads. Redaction filters for opt-in body
logging arrive with the diagnostics work in M8.

## Residual-risk register (v1)

| # | Residual risk | Disposition |
|---|---|---|
| R-T1 | Post-upgrade rejection reveals endpoint existence | Accepted; revisit M8 |
| R-T2 | No upgrade rate limiting in-library | Deployment guidance; device peer counts small |
| R-T3 | Deflate bounds delegated to IXWebSocket | Explicit ceiling knob with M6 transport caps |
| R-T4 | Store records CRC'd but not authenticated | Out of scope 1.0; document |
| R-T5 | Browser WASM client cannot send Basic auth | Documented; vendor URL params or cookie-side auth |
| R-T7 | Self-signed certificates accepted by default → active MitM possible on default-config wss without pinning | Spec-conformant posture (decided 2026-07-15); disable option + pinning provided; document in ops notes |
| R-T6 | messageID counters restart when acked journals compact, colliding with peers' dedupe windows | Durable counter high-water mark tracked for M3 store work |
