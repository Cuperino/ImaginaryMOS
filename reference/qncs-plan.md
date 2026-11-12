# QNCS Plan — a newsroom computer system on ImaginaryMOS, Qt 6.12.0 and KDDockWidgets

**Status:** proposal (2026-11-11). Nothing here is built yet.
**Product name:** **QNCS** (Q News Control System). Binaries and libraries
use the `qncs` prefix (`qncs-app`, `qncsd`, `qncs-core`, …). Default `ncsID`:
`qncs`, configurable per site (a site using Profile 6 sets an FQID instead).
**Fixed constraints (from the request):** built on ImaginaryMOS; Qt **6.12.0**;
KDDockWidgets for the docking shell; runs on **Linux, macOS and Windows**;
licensed **GPLv3**.
**Relationship to the master plan:** this is a *consumer* of ImaginaryMOS, not
a milestone of it. It is distinct from `incs-sim` (master plan §11), which is a
headless scripted test peer; QNCS is an end-user product. It lives in its
own repository (§3.2); this document sits here only because the plan depends
on `incs` details.

Decisions are numbered **(D*n*)**. D1, D2, D3, D5 and D6 were settled by the
owner on 2026-11-12 and are recorded in §13 (D1–D7 decided; D8, WebAssembly, open).

## 1. Summary

A newsroom computer system is where a newsroom writes stories, arranges them
into rundowns (MOS "running orders"), times the show, attaches media from
playout/graphics/prompter devices, and keeps those devices in step with the
rundown as it changes on air. The MOS half of that — sessions, sequencing,
retries, RO diffing, object directories, story send — already exists in
`incs`. What this project adds is everything around it: a domain model and
store, an editing and timing engine, a multi-user service, and a dockable
desktop client.

The architecture is three layers in one codebase:

1. **`qncs-core`** — Qt Core–only domain library: stories, rundowns, timing,
   undo, persistence, permissions. No GUI, no MOS.
2. **`qncs-mos`** — the MOS gateway: one `incs::Ncs` + `RunningOrderAuthor` +
   transport endpoint per configured device, pumped from a Qt event loop,
   translating between the domain model and MOS.
3. **`qncs-app`** — the Qt Widgets desktop client on KDDockWidgets.

A headless **`qncsd`** composes (1)+(2) as the newsroom server, storing
everything in PostgreSQL. It serves many clients at once and can run as an
active/standby pair with automatic failover (§6.1). Until the network layer
lands (N6) the desktop app hosts the same service in-process against a local
PostgreSQL; that mode is kept afterwards for development and tests only. A
single-seat installation is simply `qncsd` and PostgreSQL on the same machine
as the app.

## 2. Scope

### 2.1 In scope for 1.0

- Rundowns: create/open/archive, story rows with user-configurable columns,
  drag-reorder, float/skip, breaks, multi-select operations, undo/redo.
- Story editor: script body with presenter text, production instructions and
  inline media items; story metadata form; version history.
- Timing: read-rate estimated durations, actual durations from device
  feedback, cumulative and back-timing, over/under against a hard out.
- MOS as the NCS, all profiles ImaginaryMOS implements (0–7), on all three
  transports (4.0 WebSocket, 2.8 TCP, 3.8 SOAP).
- Media browser per device (directory + search), drag an object into a story.
- Prompter workflow (`roStorySend`), device status display, on-air cueing.
- Multi-user: server, accounts and roles, story locking, presence, live
  updates across clients.
- High availability: active/standby servers with automatic failover.
- Saved dock layouts ("workspaces"), multi-monitor floating windows.
- Installers for the three platforms; English + one further UI language as
  the i18n proof.

### 2.2 Explicitly out of scope for 1.0

- Wire-service ingest, assignment desk/planning, archive search beyond own
  rundowns, social/web publishing.
- Hosting MOS plug-ins (the `ncs*` ActiveX/HTML messages): ImaginaryMOS
  defers them (`plugin/`), and so do we. Revisit after 1.0 via Qt WebEngine.
- Acting as a MOS *device*. The `imos` library is used in tests only.
- Mobile clients. The server protocol (§6) is designed not to preclude
  them. (A WebAssembly build is under discussion — §9.1.)
- Vendor certification against commercial devices. Interop claims are limited
  to what we can test (§10.3).

## 3. Licensing and repository

### 3.1 Licence compatibility

| Component | Licence | Into a GPLv3 work |
|---|---|---|
| QNCS (this project) | GPL-3.0 | — |
| ImaginaryMOS | Apache-2.0 | Compatible (one-way; the reason GPLv2 would not work) |
| KDDockWidgets | GPL-2.0-only OR GPL-3.0-only (or commercial) | Taken under GPL-3.0-only |
| Qt 6.12.0 | LGPL-3.0 / GPL-3.0 per module (or commercial) | Compatible; GPL-only Qt modules become usable |
| libxml2 / zlib / IXWebSocket (inside ImaginaryMOS) | MIT / zlib / BSD-3-Clause | Compatible |
| OpenSSL 3 (wss) | Apache-2.0 | Compatible |
| PostgreSQL client library (libpq) | PostgreSQL Licence (permissive) | Compatible |

**(D1, decided) SPDX identifier: `GPL-3.0-only`.** KDDockWidgets offers no
"or later" either, so sources and binaries carry the same terms.

Consequences to plan for:

- SPDX header on every source file; REUSE-compliant `LICENSES/` directory
  (same convention as ImaginaryMOS).
- Every binary release ships corresponding source (tagged tarball including
  submodule revisions) and a third-party notices file; Apache-2.0 `NOTICE`
  content from ImaginaryMOS is carried through.
- No Mac App Store or Microsoft Store distribution with DRM terms; ship
  direct downloads.
- **(D2, decided)** Contributions require the owner's CLA
  (<https://github.com/Cuperino/Signatures/blob/main/CLA.md>). A CLA check
  runs on every pull request from N0; `CONTRIBUTING.md` links the text.
- Protocol fixes found while building QNCS go **upstream into
  ImaginaryMOS under Apache-2.0**, never into a GPL fork of it.

### 3.2 Repository

An entirely new repository (decided), separate from ImaginaryMOS, so the
Apache-2.0 library never acquires GPL files. Proposed layout:

```
qncs/
  CMakeLists.txt  CMakePresets.json  LICENSES/  docs/
  3rdparty/
    ImaginaryMOS/        # submodule, pinned commit
    KDDockWidgets/       # submodule, pinned 2.x tag
  core/                  # qncs-core   (Qt6::Core, Qt6::Sql)
  mos/                   # qncs-mos    (qncs-core + imos::incs + imos::transports)
  service/               # qncs-service: command/event API, auth, sync protocol
  server/                # qncsd       (headless; Qt6::Network, Qt6::WebSockets)
  app/                   # qncs-app    (Qt Widgets + KDDockWidgets)
    models/              # QAbstractItemModels and presenters; no widget code
    docks/               # one widget class per dock
    editor/              # script editor (QTextDocument-based)
  tests/                 # unit, integration (real imos::Device peers), GUI
  packaging/             # AppImage/Flatpak, dmg, Windows installer
```

## 4. Technology decisions

### 4.1 Qt 6.12.0

- Pinned exactly; CI installs that version on all three platforms and fails
  on drift. Patch bumps (6.12.x) are deliberate PRs.
- Modules: Core, Gui, Widgets, Sql (PostgreSQL driver, server side only), Network, WebSockets
  (client↔server only — MOS traffic uses ImaginaryMOS's own transports),
  Concurrent, LinguistTools, Test. No QML anywhere in the product.
- C++20, CMake with presets mirroring ImaginaryMOS's (`linux`, `macos`,
  `windows`, `linux-asan`, `linux-tsan`, `linux-coverage`). Presets must
  forward `CMAKE_POLICY_VERSION_MINIMUM=3.5`, which ImaginaryMOS's vendored
  zlib/IXWebSocket need under CMake 4.x.
- Toolchains: GCC or Clang on Linux, Apple Clang (universal arm64 + x86_64)
  on macOS, MSVC 2022 on Windows. Confirm each against the Qt 6.12 supported
  platforms list in N0 and record minimum OS versions there.

### 4.2 UI stack (D3, decided): Qt Widgets for everything

KDDockWidgets selects its frontend at build time; we build it with
`KDDockWidgets_FRONTENDS=qtwidgets`, `KDDockWidgets_QT6=ON`. This is its
original and most exercised frontend, and Widgets gives the two controls the
product leans on hardest ready-made: `QTableView` over a
`QAbstractItemModel` for the rundown grid, and `QTextEdit`/`QTextDocument`
for the script editor (§8.3).

Behaviour still lives outside widget classes — in item models and presenter
objects under `app/models/` — so it can be unit-tested without showing a
window.

### 4.3 KDDockWidgets

- Pinned 2.x tag as a submodule, built from source as part of our tree so
  the Qt version always matches. Every Qt bump re-runs the docking test
  suite before merge.
- Used for: dock registration, tabbing/splitting/floating, `LayoutSaver`
  serialisation, affinities (keep story editors out of the tool-dock areas),
  and a persistent central area holding open rundowns as tabs.
- Known soft spot to test early: floating-window drag and placement on
  Wayland. If it is unacceptable, default the Linux build to XCB via
  `QT_QPA_PLATFORM` and document it.

### 4.4 ImaginaryMOS

- Consumed as a pinned submodule via `add_subdirectory` for development
  (static libs, one toolchain), with a `QNCS_USE_SYSTEM_IMAGINARYMOS` option
  that switches to `find_package(ImaginaryMOS CONFIG REQUIRED)` for distro
  packaging. Targets: `imos::incs`, `imos::transports`, and `imos::imos` for
  tests only.
- Built with `IMOS_ENABLE_TLS=ON`, `IMOS_ENABLE_COMPAT28=ON`,
  `IMOS_ENABLE_WS3=ON`.
- Still open upstream and their effect here: **M7 dialect profiles** (needed
  for N8's device-quirk handling; not blocking earlier milestones) and **M8
  `mos-sim`** (would strengthen N8 interop; we do not wait for it).

### 4.5 Persistence (D4, decided): PostgreSQL

PostgreSQL is the only database backend. It was chosen over SQLite for one
reason: streaming replication gives the newsroom server a warm standby and
automatic failover (§6.1). Its many-writer concurrency is not used — the
service remains the single writer.

- **Only the server links the database driver.** Qt's PostgreSQL driver
  needs `libpq` at run time; `qncsd` packages bundle or depend on it per
  platform. Once clients are networked (N6) the desktop app carries no
  database code at all.
- **Minimum PostgreSQL 16**; the exact floor is confirmed in N1 against what
  the three platforms package.
- All access goes through `core/` repository interfaces. They exist for
  testability (in-memory fakes for unit tests), not to keep a second SQL
  backend alive; PostgreSQL features (`jsonb`, `tsvector` search, advisory
  locks, `LISTEN/NOTIFY`) may be used freely behind them.
- Schema migrations are numbered, forward-only, transactional, and tested
  from every released version.
- **The MOS message store lives in the database too.** ImaginaryMOS's
  `IMessageStore` (unacked-message durability) is an injected interface;
  `qncs-mos` implements it on PostgreSQL instead of using the library's
  file backend, so pending device messages replicate to the standby with
  everything else. The implementation must pass the same crash-recovery
  expectations as the library's file store.

## 5. Domain model (`qncs-core`)

| Entity | Notes | MOS mapping |
|---|---|---|
| **Rundown** | Title, air date/time, planned duration, hard out, state (draft / MOS-active / archived), ordered stories | `roID`, RO header fields |
| **Story** | Slug, page number, segment, status flags (float, skip, approved), presenter, body, metadata | `storyID`, `storySlug`, `storyNum` |
| **Body** | Ordered blocks: presenter paragraph, production instruction, item reference | `storyBody` `<p>` / `<pi>` / `<storyItem>` |
| **Item** | Reference to a device object plus in/out/duration, trigger, channel, opaque payload | `itemID`, `objID`, `mosID`, `mosExternalMetadata` |
| **Device** | A configured MOS peer: `mosID`, transport, endpoints, credentials, encoding, enabled profiles | one `incs::Ncs` |
| **MediaObject** | Cached directory entry per device | `mosObj` |
| **User / Role** | Account, permissions per rundown folder | — |
| **Revision** | Immutable story snapshot per save | — |

Rules that shape the code:

- **IDs are ours and stable.** `roID`/`storyID`/`itemID` are generated once
  (compact, within MOS length limits) and never reused; display numbering is
  separate.
- **Opaque payloads stay opaque.** `mosExternalMetadata` and item payloads
  are stored as the bytes the device gave us and returned unmodified —
  ImaginaryMOS guarantees this through the codec, and the store must not
  undo it by re-serialising XML.
- **Every mutation is a command** producing events (§6). Undo/redo, audit,
  multi-user sync and MOS output all hang off that single stream.
- **Timing is a pure function** of rundown state plus the per-presenter read
  rate, recomputed incrementally. It reuses the spec time formats ImaginaryMOS
  already parses rather than inventing its own.

## 6. Service layer and multi-user

One authoritative service owns the model. Clients send **commands**
(`MoveStories`, `EditBody`, `InsertItem`, …); the service validates,
applies, persists, and broadcasts **events** in a total order per rundown.
Clients apply their own commands optimistically and reconcile on the echoed
event.

- **In-process (until N6; development and tests after):** the service runs
  inside the app; commands and events are queued signal/slot calls.
- **Networked (N6):** the same messages over a WebSocket (TLS), CBOR-encoded,
  with a protocol version handshake. A reconnecting client sends its last
  seen event number and receives the gap or a snapshot.
- **Concurrency model (D5, decided): story-level pessimistic locks**, not
  collaborative text merging. One user edits a story body at a time; others
  see it read-only with the holder's name; locks expire on disconnect and can
  be broken by a privileged role. Rundown structure (order, flags) is
  lock-free and resolved by server ordering. This matches newsroom practice
  and maps directly onto Profile 7, where a *device* asks to edit a story
  (`incs::ILeasePolicy` is implemented on top of the same lock table).
- **Auth:** local accounts with salted password hashes at 1.0; the interface
  leaves room for LDAP/OIDC. Client-side secrets go in the OS keychain.

### 6.1 High availability

Two `qncsd` machines, one active and one standby, plus a small witness node
as tie-breaker.

```
     clients                          MOS devices
        └────────► service address ◄───────┘        (floating IP or DNS)
                         │
          ┌──────────────┴──────────────┐
    qncsd A (active)              qncsd B (standby)
    service + MOS gateway         gateway off, no clients
          │                             │
    PostgreSQL primary ─────────► PostgreSQL replica
                  streaming replication
                         ▲
                   witness node
```

- **Database failover is delegated** to established PostgreSQL tooling
  (Patroni or pg_auto_failover — chosen in N7); we do not write our own.
  Replication is synchronous by default so no confirmed command is lost;
  asynchronous is a site option.
- **Exactly one active `qncsd`.** Each instance tries to hold a session
  advisory lock on the current primary. The holder runs the service and the
  MOS gateway; an instance that loses the lock or its database connection
  stops its gateway and drops its clients immediately. This is what keeps
  two NCSs with the same `ncsID` from ever talking to the devices.
- **Takeover:** the standby acquires the lock, loads state, starts the
  gateway, and the service address moves to it. Unacknowledged MOS messages
  are resent from the database-backed store (devices drop duplicates by
  `messageID`), then each device's running orders are reconciled with the
  database through the library's normal resync path.
- **Clients** reconnect automatically with their last seen event number,
  re-claim their story locks, and resend unsaved edits.
- **Target:** service restored within 60 seconds with no operator action.
  Devices keep playing what they already hold during the gap.
- **Platforms (D7, decided):** `qncsd` — including the failover code paths —
  builds and passes its CI test suite on Linux, macOS and Windows. The
  replicated pair as a deployed system (PostgreSQL replication, failover
  tooling, address takeover, kill-the-server rig) is actively tested on
  Linux only; on the other two it is expected to work but is not exercised.

## 7. MOS gateway (`qncs-mos`)

### 7.1 Hosting `incs`

`incs::Ncs` models exactly one peer relationship and owns no threads, so the
gateway is a set of **`PeerLink`** objects, one per configured device:

```
PeerLink (QObject, lives on the gateway thread)
 ├─ incs::Ncs                       session + Profiles 0–7, NCS side
 ├─ incs::RunningOrderAuthor        desired state → minimal MOS diffs
 ├─ transport endpoint              Ws40Endpoint | Tcp28Endpoint | Ws3Endpoint
 ├─ IMessageStore (PostgreSQL)      pending messages, replicated
 └─ Qt adapters                     IClock, ITimerQueue, IRandom, ILogger
```

- A single gateway thread runs all links. Each link arms a `QTimer` from
  `Ncs::nextDeadline()`, calls `poll()`/`dispatch()`, then drains every
  `next*()` queue (`nextEvent`, `nextDirectoryEvent`, `nextElementStatus`,
  `nextItemReplace`, `nextQueryPage`, `nextCtrlResult`, `nextItemCue`,
  `nextStoryActionAudit`) into service commands. How transport activity wakes
  the loop is pinned in the N0 spike against the transports' pump API.
- The gateway **never waits on the network or the UI**; its only
  synchronous calls are short message-store transactions (§4.5). It talks to the
  service only through commands and events.
- All devices share this NCS's `ncsID`; each link carries its own `mosID`,
  transport, wire encoding (UCS-2 BE default, UTF-8 per peer) and topology
  (4.0 standard or passive).

### 7.2 Mapping the model to profiles

| Profile | QNCS behaviour |
|---|---|
| **0** | Connection state, heartbeat and `listMachInfo` per device in a Devices dock; reconnect is the library's. |
| **1** | Media browser fed by the object directory; `requestAll` on first connect, incremental `mosObj` after. |
| **2** | A rundown made *MOS-active* for a device is mirrored into that link's `RunningOrderAuthor`; model events update desired state and a short debounce calls `commit()`, which emits the minimal `roElementAction`/`roReplace` sequence. `roElementStat` from the device drives the status and actual-duration columns. `roReq` is answered by the library from the author. |
| **3** | Search box → `queryObjects` with paging; "new placeholder" → `createObject`; `mosItemReplace` from the device is accepted via `nextItemReplace()` and written back into the story item. Directory resync prefers the P3 path when advertised. |
| **4** | Rundowns flagged for prompting call `setStorySendLinked` and send bodies with `sendStory` on save/approve; `roReqAll` is answered by the library. |
| **5** | Operator "cue/take" actions send `roCtrl`/`roItemCue`; inbound cues highlight the on-air row. |
| **6** | FQID settings per site, validated through `Ncs::validateConfig` at device set-up so a bad config is a form error, not a runtime surprise. |
| **7** | Device-originated story edits are gated by the lock table (§6), applied as ordinary commands attributed to the device, and surfaced in story history. |

Two policies need a decision per device and therefore a settings UI:

- **Item filtering:** send the device every story but only *its* items
  (default), or the full rundown.
- **Commit cadence:** immediate, debounced (default ~250 ms), or manual
  "publish" for cautious sites.

### 7.3 Things the protocol imposes on the product

- **UCS-2 peers cannot carry characters outside the BMP.** The editor must
  warn before such text is sent to a UCS-2 device; exact behaviour is
  settled in N3 against the library's encoding policy (master plan §3.4).
- **2.8 TCP has no authentication.** The device form requires a peer host
  for those links (the transport already restricts accepted connections to
  it) and the UI labels them as unauthenticated.
- **Resync is normal, not an error.** When a device loses the RO, the library
  resyncs; the UI shows it as a transient state on the device, not a dialog.
- If QNCS needs something `incs` does not expose, **fix it upstream** in
  `core/`/`incs` with tests there. No protocol logic in `qncs-mos`.

## 8. Desktop client (`qncs-app`)

### 8.1 Docks

| Dock | Content |
|---|---|
| **Rundown** (central, tabbed) | `QTableView` grid over a C++ model: configurable columns, inline edit, drag-reorder, multi-select, status colouring, timing columns |
| **Story editor** | Script body + metadata; several can be open, tabbed or floated |
| **Directory** | Rundown folders, open/create/archive |
| **Media browser** | Per-device object list, search, thumbnails where the device provides them; drag source |
| **Devices** | Connection state, profiles, last error, reconnect/resync actions |
| **Timing** | Show clock, over/under, back-time to hard out |
| **MOS monitor** | Live message log per device and channel, filterable — the engineering view |
| **History** | Story revisions with diff and restore |

Workspaces (saved KDDockWidgets layouts) ship as presets — *Writer*,
*Producer*, *Engineer* — and users save their own; layouts are versioned so
an upgrade that adds a dock does not discard them.

### 8.2 Rules for the widget layer

- One widget class per dock under `app/docks/`; it owns layout and painting
  only. State and behaviour sit in a model or presenter that has no
  `QWidget` dependency.
- No MOS types above `qncs-mos`; the UI sees domain types and service
  commands only.
- Every action is a `QAction` in one central table, reachable from the
  keyboard with a remappable shortcut — rundown operators do not use the
  mouse on air. The same table feeds menus, toolbars and the macOS menu bar.
- Custom painting goes through delegates and the palette, not per-widget
  style sheets; dark default, light available.

### 8.3 Script editor (D6, decided): rich-text document with a block structure

One `QTextDocument` per story, edited in a `QTextEdit` subclass. The MOS
story body is a sequence of typed blocks, and the document mirrors it:

- Each `QTextBlock` carries a **block type** in its format (presenter text,
  production instruction, item) plus a stable block ID in its user data.
  Type determines styling, whether the block counts toward read time, and
  how it serialises (`<p>`, `<pi>`, `<storyItem>`).
- An **item** is a non-editable block rendered as a chip through a custom
  `QTextObjectInterface`; it holds only the item ID, and the item's data
  stays in the domain model.
- The document is a *view* of the domain body. A converter maps
  body↔document in both directions and is the single place the mapping
  lives; edits are turned into `EditBody` commands block by block, so undo,
  history and MOS output see blocks, never HTML.
- Character formatting is limited to what `storyBody` can carry; paste is
  sanitised down to that set.

Selection, IME, undo grouping, find/replace and spell-check hooks come from
`QTextEdit`. The work is in the converter and in keeping block IDs stable
across split, merge and paste — that is the N0 spike.

## 9. Platforms and packaging

| | Linux | macOS | Windows |
|---|---|---|---|
| Toolchain | GCC/Clang, Ninja | Apple Clang, universal binary | MSVC 2022 |
| Bundle | AppImage + Flatpak; `.deb` for `qncsd` | signed, notarised `.dmg` (`macdeployqt`) | signed installer (`windeployqt` + CPack) |
| Server | systemd unit | launchd plist | Windows service |
| Config/data | `QStandardPaths` everywhere; no hard-coded paths |
| Database | PostgreSQL from the distribution, Homebrew or the official installer; `qncsd` set-up creates the schema |

Cross-platform obligations beyond "it compiles":

- Firewall prompts for the listening MOS ports (macOS, Windows) explained in
  first-run set-up; the legacy ports 10540–10542 are configurable.
- HiDPI and mixed-DPI multi-monitor verified with floating docks on each OS.
- Native menu bar and shortcuts on macOS (Cmd vs Ctrl) from one action table.
- Every component — libraries, `qncs-app`, `qncsd` — is supported on all
  three. CI builds, runs the full test suite (including the PostgreSQL-backed
  tests) and packages on all three on every merge; release artifacts come
  only from CI.

### 9.1 WebAssembly (D8, open)

Requirement as stated by the owner: **`qncsd` should also run on
WebAssembly.** Qt's WebAssembly target runs inside a browser sandbox, which
rules out three things the native server depends on:

| Native `qncsd` needs | In a browser |
|---|---|
| Listen for client connections | Not possible — a page can only dial out |
| Listen on MOS ports; 2.8 TCP; 3.8 SOAP server | Not possible; only outbound WebSocket, i.e. MOS 4.0 where the *device* accepts the connection (ImaginaryMOS's `ws40_emscripten` client) |
| PostgreSQL via `libpq` | Not possible — no raw sockets; would need a different store or a WebSocket bridge to the database |

So a WebAssembly build cannot be the newsroom server. What it can be is one
of the following, and the plan needs the owner to pick:

- **(a) Browser client.** `qncs-app` compiled to WebAssembly, connecting to a
  native `qncsd` over the existing WebSocket protocol. Qt Widgets and
  KDDockWidgets' Widgets frontend must both be verified on the wasm target
  (floating windows cannot leave the browser tab).
- **(b) Self-contained browser edition.** The service and MOS gateway
  compiled into the page with the app: single user, MOS 4.0 outbound only,
  and a second, browser-local storage backend behind the repository
  interfaces — which reopens the "PostgreSQL only" decision for that build.
- **(c) Both.**

Whichever is chosen, the wasm build joins CI (build plus headless-browser
tests) and gets a milestone; `core/`, `service/` and `mos/` are kept free of
threads-required and filesystem-required code paths from N1 so the option
stays cheap.

## 10. Quality

### 10.1 Tests

- **Unit** (Qt Test): domain commands and timing engine against in-memory
  repositories; repositories and migrations against a real PostgreSQL in
  CI on all three OSes; body↔`storyBody` conversion (round-trip property tests).
- **MOS integration:** each test stands up a real `imos::Device` in-process
  as the peer and drives the gateway over loopback on all three transports —
  ImaginaryMOS ships both roles, so no mock of the protocol is ever written.
  Assertions are on the device's resulting RO, not on message text.
- **Multi-user:** N scripted clients against one `qncsd`, randomised command
  interleavings, convergence checked; lock and reconnect cases.
- **GUI:** Qt Test per dock against fake models (`QTest` key/mouse events,
  offscreen platform in CI); script-editor edit sequences checked against
  the resulting body; layout save/restore round-trips across versions.
- **Soak:** 24 h rundown churn against two devices with connection drops; no
  divergence between model and device RO.

### 10.2 Gates in CI

clang-format, clang-tidy, clazy; ASan/UBSan and TSan presets on
Linux; ≥ 85 % line coverage on `core/` and `mos/`; REUSE licence lint; a
dependency-pin check (Qt = 6.12.0, submodule SHAs).

### 10.3 Interoperability

Sofie `mos-connection` as an independent open-source MOS device peer, plus
ImaginaryMOS's `mos-sim` and dialect personalities when its M7/M8 land. Any
commercial device a pilot site offers becomes a recorded regression case.
The 1.0 release notes state plainly which peers were tested.

## 11. Milestones

Indicative durations assume two to three engineers; treat them as shapes.

| Milestone | Contents | Exit criteria | Indicative |
|---|---|---|---|
| **N0 — Bootstrap and spikes** | Repo, licence scaffolding, CMake presets, CI on three OSes, pinned Qt 6.12.0 / KDDockWidgets / ImaginaryMOS. Spikes: (a) KDDockWidgets (Widgets frontend) on 6.12.0 on all three OSes incl. Wayland and mixed DPI; (b) block-structured `QTextDocument` script editor with lossless body round-trip; (c) `incs::Ncs` pumped from a Qt event loop against an in-process `imos::Device` | Green three-OS build of a docking shell; spike findings written up; P0 handshake in the spike | 3 wk |
| **N1 — Domain core** | Model, command/event service (in-process), PostgreSQL repositories and migrations, in-memory fakes for unit tests, undo/redo, timing engine | Core unit suite green; a rundown survives restart; timing matches a reference table | 4 wk |
| **N2 — Shell and editing** | Rundown grid, story editor, directory, timing dock, workspaces, shortcut table, theming | A show can be written, ordered and timed with no MOS device; layouts persist | 5 wk |
| **N3 — MOS gateway, Profiles 0–2** | Device configuration UI, `PeerLink`, 4.0 WebSocket (standard + passive, TLS, auth), RO mirroring through `RunningOrderAuthor`, status feedback, Devices and MOS monitor docks | Edits in the UI converge on an `imos::Device`'s RO under random churn and forced disconnects | 4 wk |
| **N4 — Media, Profiles 1 and 3** | Media browser, search and paging, drag-to-story, placeholders, `mosItemReplace` ingest | Three-channel workflow green end to end from the UI | 3 wk |
| **N5 — Prompter and control, Profiles 4, 5, 7** | Story send, cue/take, on-air highlighting, device-originated story edits through the lock table | Prompter round trip green; P7 edit appears in history with attribution | 3 wk |
| **N6 — Multi-user** | `qncsd`, sync protocol, accounts and roles, story locks, presence, reconnect; gateway moves server-side | Convergence suite green with 10 clients; N1–N5 suites still pass through the networked path | 6 wk |
| **N7 — High availability** | Database-backed `IMessageStore`, active/standby election, PostgreSQL replication and failover tooling, service-address takeover, client and device recovery, kill-the-server test rig | Killing the active server mid-churn restores service within 60 s; no confirmed command lost; devices converge with no duplicate or missing RO changes | 5 wk |
| **N8 — Legacy, Profile 6, interop** | 2.8 TCP and 3.8 SOAP device types, FQID settings, dialect quirks (as ImaginaryMOS M7 allows), `mos-connection` lab | MOS integration suite green on all three transports; interop report written | 3 wk |
| **N9 — Packaging and polish** | Installers and signing, server service units, i18n and second language, accessibility pass, user and admin documentation | Clean-machine install and first-run on each OS from CI artifacts | 3 wk |
| **N10 — Hardening → 1.0** | Soak, performance (500-story rundown, 50 k objects per device), security review, bug burn-down | Soak clean; no open data-loss or desync defects; tagged 1.0 with source release | 3 wk |

Sequencing rationale: the service boundary exists from N1 so that going
multi-user in N6 is adding a wire, not rewriting state handling. The UI
precedes MOS so the gateway has a real model to mirror. The 4.0 transport
comes first and the legacy ones last, mirroring how ImaginaryMOS itself was
built. A usable single-seat release (app plus a local PostgreSQL) exists after N5 if an early pilot is
wanted.

## 12. Risks

| Risk | Mitigation |
|---|---|
| KDDockWidgets misbehaves on Qt 6.12.0 or on one platform (Wayland especially) | N0 spike (a) before any UI work; XCB default on Linux as the fallback; upstream fixes under its GPL terms |
| Block identity in the script editor breaks under split/merge/paste, corrupting the body mapping | N0 spike (b); converter covered by round-trip property tests |
| Scope creep — an NRCS can absorb unlimited features | §2.2 is the contract; additions need a milestone, not a branch |
| Qt 6.12.0 pin ages (security fixes arrive in later patches) | Pin is exact but reviewed each patch release; bump is one PR gated by the full suite |
| No commercial MOS devices to test against | In-process `imos::Device`, `mos-connection`, record real traffic at pilot sites; say so in release notes |
| An `incs` API gap blocks a workflow | Budget upstream time in N3–N5; fix in ImaginaryMOS with tests |
| GPL obligations missed in a release | Source tarball and notices are CI artifacts of the same job that builds installers |
| Multi-user retrofitting | Command/event service from N1; in-process and networked are the same code path with a different carrier |
| Both servers active at once (split brain) sends conflicting updates to devices | Single advisory lock on the database primary; gateway stops on lock or connection loss; witness node; fault-injection tests in N7 |
| A device misbehaves when its NCS reconnects from another machine mid-show | Floating address so the NCS endpoint never changes; test each transport in N7; record per-device findings |
| Synchronous database writes for the message store slow the MOS gateway | Measure in the N3 gateway work; batch per poll cycle if needed |
| PostgreSQL adds installation burden for small sites | Installer for `qncsd` offers to set up a local database; single-machine layout documented |

## 13. Decisions

| | Decision | Status |
|---|---|---|
| — | Entirely new repository | Decided 2026-11-12 |
| D1 | Licence identifier `GPL-3.0-only` | Decided 2026-11-12 |
| D2 | Contributions under the owner's CLA | Decided 2026-11-12 |
| D3 | Qt Widgets for the whole UI | Decided 2026-11-12 |
| D4 | PostgreSQL as the only database, with automatic failover in 1.0 | Decided 2026-11-12 |
| D5 | Story-level locks | Decided 2026-11-12 |
| D6 | Rich-text document with a block structure | Decided 2026-11-12 |
| D7 | All components supported and CI-tested on Linux, macOS and Windows; the replicated pair actively tested on Linux only | Decided 2026-11-12 |
| D8 | What "`qncsd` on WebAssembly" means (§9.1) | **Open** |
| — | Product name QNCS; default `ncsID` `qncs` | Decided 2026-11-12 |
