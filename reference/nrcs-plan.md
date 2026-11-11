# NRCS Plan — a newsroom computer system on ImaginaryMOS, Qt 6.12.0 and KDDockWidgets

**Status:** proposal (2026-11-11). Nothing here is built yet.
**Working name:** `nrcs` (placeholder — pick a product name before N0 ends; it
ends up in bundle IDs, the default `ncsID`, and config paths).
**Fixed constraints (from the request):** built on ImaginaryMOS; Qt **6.12.0**;
KDDockWidgets for the docking shell; runs on **Linux, macOS and Windows**;
licensed **GPLv3**.
**Relationship to the master plan:** this is a *consumer* of ImaginaryMOS, not
a milestone of it. It is distinct from `incs-sim` (master plan §11), which is a
headless scripted test peer; the NRCS is an end-user product. It lives in its
own repository (§3.2); this document sits here only because the plan depends
on `incs` details.

Decisions marked **(D*n*)** are recommendations that need the owner's
confirmation; everything else follows from the constraints.

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

1. **`nrcs-core`** — Qt Core–only domain library: stories, rundowns, timing,
   undo, persistence, permissions. No GUI, no MOS.
2. **`nrcs-mos`** — the MOS gateway: one `incs::Ncs` + `RunningOrderAuthor` +
   transport endpoint per configured device, pumped from a Qt event loop,
   translating between the domain model and MOS.
3. **`nrcs-app`** — the Qt Quick desktop client on KDDockWidgets.

A headless **`nrcsd`** composes (1)+(2) as the newsroom server. Until the
network layer lands (N6) the desktop app embeds the same service in-process
("standalone mode"), which stays supported afterwards for single-seat use,
demos and tests.

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
- Saved dock layouts ("workspaces"), multi-monitor floating windows.
- Installers for the three platforms; English + one further UI language as
  the i18n proof.

### 2.2 Explicitly out of scope for 1.0

- Wire-service ingest, assignment desk/planning, archive search beyond own
  rundowns, social/web publishing.
- Hosting MOS plug-ins (the `ncs*` ActiveX/HTML messages): ImaginaryMOS
  defers them (`plugin/`), and so do we. Revisit after 1.0 via Qt WebEngine.
- Acting as a MOS *device*. The `imos` library is used in tests only.
- Mobile and web clients. The server protocol (§6) is designed not to
  preclude them.
- Vendor certification against commercial devices. Interop claims are limited
  to what we can test (§10.3).

## 3. Licensing and repository

### 3.1 Licence compatibility

| Component | Licence | Into a GPLv3 work |
|---|---|---|
| NRCS (this project) | GPL-3.0 | — |
| ImaginaryMOS | Apache-2.0 | Compatible (one-way; the reason GPLv2 would not work) |
| KDDockWidgets | GPL-2.0-only OR GPL-3.0-only (or commercial) | Taken under GPL-3.0-only |
| Qt 6.12.0 | LGPL-3.0 / GPL-3.0 per module (or commercial) | Compatible; GPL-only Qt modules become usable |
| libxml2 / zlib / IXWebSocket (inside ImaginaryMOS) | MIT / zlib / BSD-3-Clause | Compatible |
| OpenSSL 3 (wss) | Apache-2.0 | Compatible |
| SQLite | Public domain | Compatible |

**(D1) SPDX identifier: `GPL-3.0-only`.** The request says "GPLv3", and
because KDDockWidgets offers no "or later", distributed binaries are GPLv3
regardless. `GPL-3.0-or-later` on our own sources is the alternative if
future-proofing matters more than simplicity; decide before the first commit,
since relicensing later needs every contributor's consent.

Consequences to plan for:

- SPDX header on every source file; REUSE-compliant `LICENSES/` directory
  (same convention as ImaginaryMOS).
- Every binary release ships corresponding source (tagged tarball including
  submodule revisions) and a third-party notices file; Apache-2.0 `NOTICE`
  content from ImaginaryMOS is carried through.
- No Mac App Store or Microsoft Store distribution with DRM terms; ship
  direct downloads.
- Contributions under DCO sign-off, no CLA **(D2)** — which means the project
  cannot later be dual-licensed. Choose a CLA instead if that door must stay
  open.
- Protocol fixes found while building the NRCS go **upstream into
  ImaginaryMOS under Apache-2.0**, never into a GPL fork of it.

### 3.2 Repository

A new repository, separate from ImaginaryMOS, so the Apache-2.0 library never
acquires GPL files. Proposed layout:

```
nrcs/
  CMakeLists.txt  CMakePresets.json  LICENSES/  docs/
  3rdparty/
    ImaginaryMOS/        # submodule, pinned commit
    KDDockWidgets/       # submodule, pinned 2.x tag
  core/                  # nrcs-core   (Qt6::Core, Qt6::Sql)
  mos/                   # nrcs-mos    (nrcs-core + imos::incs + imos::transports)
  service/               # nrcs-service: command/event API, auth, sync protocol
  server/                # nrcsd       (headless; Qt6::Network, Qt6::WebSockets)
  app/                   # nrcs-app    (Qt Quick + KDDockWidgets)
    viewmodels/          # C++ QAbstractItemModels / QObjects exposed to QML
    qml/                 # QML modules, one per dock
  tests/                 # unit, integration (real imos::Device peers), QML
  packaging/             # AppImage/Flatpak, dmg, Windows installer
```

## 4. Technology decisions

### 4.1 Qt 6.12.0

- Pinned exactly; CI installs that version on all three platforms and fails
  on drift. Patch bumps (6.12.x) are deliberate PRs.
- Modules: Core, Gui, Qml, Quick, QuickControls2, Sql (SQLite driver),
  Network, WebSockets (client↔server only — MOS traffic uses ImaginaryMOS's
  own transports), Concurrent, LinguistTools, Test, QuickTest.
- C++20, CMake with presets mirroring ImaginaryMOS's (`linux`, `macos`,
  `windows`, `linux-asan`, `linux-tsan`, `linux-coverage`). Presets must
  forward `CMAKE_POLICY_VERSION_MINIMUM=3.5`, which ImaginaryMOS's vendored
  zlib/IXWebSocket need under CMake 4.x.
- Toolchains: GCC or Clang on Linux, Apple Clang (universal arm64 + x86_64)
  on macOS, MSVC 2022 on Windows. Confirm each against the Qt 6.12 supported
  platforms list in N0 and record minimum OS versions there.

### 4.2 UI stack **(D3): Qt Quick, with KDDockWidgets' Qt Quick frontend**

KDDockWidgets selects its frontend at build time
(`KDDockWidgets_FRONTENDS=qtquick`, `KDDockWidgets_QT6=ON`), so this is a
one-way choice per binary.

Why Qt Quick: GPU-rendered dense lists and a themable dark control-room UI,
`TableView` for the rundown grid, and a far easier path to a touch-friendly
gallery layout later. What it costs: the Qt Widgets frontend of KDDockWidgets
is the older and more exercised one, and Qt Quick has no drop-in equivalent
of `QTextEdit` with custom inline objects for the script editor (§7.3).

The mitigation is structural: **all behaviour lives in C++ view-models**
(`app/viewmodels/`), QML is presentation only. If the N0 spikes (§11) fail,
switching to Widgets replaces `app/qml/` and the dock registration, not the
application.

### 4.3 KDDockWidgets

- Pinned 2.x tag as a submodule, built from source as part of our tree so
  the Qt version always matches. Its Qt Quick frontend reaches into Qt
  internals, so every Qt bump re-runs the docking test suite before merge.
- Used for: dock registration, tabbing/splitting/floating, `LayoutSaver`
  serialisation, affinities (keep story editors out of the tool-dock areas),
  and a persistent central area holding open rundowns as tabs.
- Known soft spot to test early: floating-window drag and placement on
  Wayland. If it is unacceptable, default the Linux build to XCB via
  `QT_QPA_PLATFORM` and document it.

### 4.4 ImaginaryMOS

- Consumed as a pinned submodule via `add_subdirectory` for development
  (static libs, one toolchain), with a `NRCS_USE_SYSTEM_IMAGINARYMOS` option
  that switches to `find_package(ImaginaryMOS CONFIG REQUIRED)` for distro
  packaging. Targets: `imos::incs`, `imos::transports`, and `imos::imos` for
  tests only.
- Built with `IMOS_ENABLE_TLS=ON`, `IMOS_ENABLE_COMPAT28=ON`,
  `IMOS_ENABLE_WS3=ON`.
- Still open upstream and their effect here: **M7 dialect profiles** (needed
  for N7's device-quirk handling; not blocking earlier milestones) and **M8
  `mos-sim`** (would strengthen N7 interop; we do not wait for it).

### 4.5 Persistence **(D4): SQLite behind a repository interface**

SQLite in WAL mode via Qt Sql, owned by exactly one process (the standalone
app or `nrcsd`). All access goes through `core/` repository interfaces so a
PostgreSQL backend can be added for large sites without touching callers.
Schema migrations are numbered, forward-only, and tested from every released
version.

ImaginaryMOS's file-backed `IMessageStore` (unacked-message durability) gets
its own directory per device beside the database — it is the library's
resume mechanism and is not folded into our schema.

## 5. Domain model (`nrcs-core`)

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

- **Standalone:** the service runs in-process; commands and events are queued
  signal/slot calls.
- **Networked (N6):** the same messages over a WebSocket (TLS), CBOR-encoded,
  with a protocol version handshake. A reconnecting client sends its last
  seen event number and receives the gap or a snapshot.
- **Concurrency model (D5): story-level pessimistic locks**, not
  collaborative text merging. One user edits a story body at a time; others
  see it read-only with the holder's name; locks expire on disconnect and can
  be broken by a privileged role. Rundown structure (order, flags) is
  lock-free and resolved by server ordering. This matches newsroom practice
  and maps directly onto Profile 7, where a *device* asks to edit a story
  (`incs::ILeasePolicy` is implemented on top of the same lock table).
- **Auth:** local accounts with salted password hashes at 1.0; the interface
  leaves room for LDAP/OIDC. Client-side secrets go in the OS keychain.

## 7. MOS gateway (`nrcs-mos`)

### 7.1 Hosting `incs`

`incs::Ncs` models exactly one peer relationship and owns no threads, so the
gateway is a set of **`PeerLink`** objects, one per configured device:

```
PeerLink (QObject, lives on the gateway thread)
 ├─ incs::Ncs                       session + Profiles 0–7, NCS side
 ├─ incs::RunningOrderAuthor        desired state → minimal MOS diffs
 ├─ transport endpoint              Ws40Endpoint | Tcp28Endpoint | Ws3Endpoint
 ├─ IMessageStore (file-backed)     per-device directory
 └─ Qt adapters                     IClock, ITimerQueue, IRandom, ILogger
```

- A single gateway thread runs all links. Each link arms a `QTimer` from
  `Ncs::nextDeadline()`, calls `poll()`/`dispatch()`, then drains every
  `next*()` queue (`nextEvent`, `nextDirectoryEvent`, `nextElementStatus`,
  `nextItemReplace`, `nextQueryPage`, `nextCtrlResult`, `nextItemCue`,
  `nextStoryActionAudit`) into service commands. How transport activity wakes
  the loop is pinned in the N0 spike against the transports' pump API.
- The gateway **never blocks and never calls into the UI**. It talks to the
  service only through commands and events.
- All devices share this NCS's `ncsID`; each link carries its own `mosID`,
  transport, wire encoding (UCS-2 BE default, UTF-8 per peer) and topology
  (4.0 standard or passive).

### 7.2 Mapping the model to profiles

| Profile | NRCS behaviour |
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
- If the NRCS needs something `incs` does not expose, **fix it upstream** in
  `core/`/`incs` with tests there. No protocol logic in `nrcs-mos`.

## 8. Desktop client (`nrcs-app`)

### 8.1 Docks

| Dock | Content |
|---|---|
| **Rundown** (central, tabbed) | `TableView` grid over a C++ model: configurable columns, inline edit, drag-reorder, multi-select, status colouring, timing columns |
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

### 8.2 Rules for the QML layer

- One QML module per dock, registered declaratively (`qt_add_qml_module`),
  compiled ahead of time, `qmllint` clean in CI.
- No business logic or MOS types in QML; view-models expose
  `QAbstractItemModel`s and invokable commands only.
- Every action reachable from the keyboard with a remappable shortcut —
  rundown operators do not use the mouse on air.
- Theme through one `Theme` singleton; dark default, light available.

### 8.3 Script editor

The MOS story body is already a sequence of typed blocks, so the editor is
**block-based (D6)**: a list of presenter-text blocks (`TextArea`),
production-instruction blocks and item chips, with keyboard navigation and
selection that crosses block boundaries handled by the view-model. This
avoids fighting Qt Quick's lack of custom inline text objects and makes the
body↔`storyBody` mapping lossless by construction. Cross-block selection and
IME behaviour are the hard parts and are spiked in N0.

## 9. Platforms and packaging

| | Linux | macOS | Windows |
|---|---|---|---|
| Toolchain | GCC/Clang, Ninja | Apple Clang, universal binary | MSVC 2022 |
| Bundle | AppImage + Flatpak; `.deb` for `nrcsd` | signed, notarised `.dmg` (`macdeployqt`) | signed installer (`windeployqt` + CPack) |
| Server | systemd unit | launchd plist | Windows service |
| Config/data | `QStandardPaths` everywhere; no hard-coded paths |

Cross-platform obligations beyond "it compiles":

- Firewall prompts for the listening MOS ports (macOS, Windows) explained in
  first-run set-up; the legacy ports 10540–10542 are configurable.
- HiDPI and mixed-DPI multi-monitor verified with floating docks on each OS.
- Native menu bar and shortcuts on macOS (Cmd vs Ctrl) from one action table.
- CI builds, tests and packages all three on every merge; release artifacts
  come only from CI.

## 10. Quality

### 10.1 Tests

- **Unit** (Qt Test): domain commands, timing engine, repositories and
  migrations, body↔`storyBody` conversion (round-trip property tests).
- **MOS integration:** each test stands up a real `imos::Device` in-process
  as the peer and drives the gateway over loopback on all three transports —
  ImaginaryMOS ships both roles, so no mock of the protocol is ever written.
  Assertions are on the device's resulting RO, not on message text.
- **Multi-user:** N scripted clients against one `nrcsd`, randomised command
  interleavings, convergence checked; lock and reconnect cases.
- **QML:** Qt Quick Test per dock against fake view-models; layout
  save/restore round-trips across versions.
- **Soak:** 24 h rundown churn against two devices with connection drops; no
  divergence between model and device RO.

### 10.2 Gates in CI

clang-format, clang-tidy, clazy, `qmllint`; ASan/UBSan and TSan presets on
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
| **N0 — Bootstrap and spikes** | Repo, licence scaffolding, CMake presets, CI on three OSes, pinned Qt 6.12.0 / KDDockWidgets / ImaginaryMOS. Spikes: (a) KDDockWidgets Qt Quick on 6.12.0 on all three OSes incl. Wayland and mixed DPI; (b) block-based script editor; (c) `incs::Ncs` pumped from a Qt event loop against an in-process `imos::Device` | Green three-OS build of a docking shell; written go/no-go on D3 and D6; P0 handshake in the spike | 3 wk |
| **N1 — Domain core** | Model, command/event service (in-process), SQLite repositories and migrations, undo/redo, timing engine | Core unit suite green; a rundown survives restart; timing matches a reference table | 4 wk |
| **N2 — Shell and editing** | Rundown grid, story editor, directory, timing dock, workspaces, shortcut table, theming | A show can be written, ordered and timed with no MOS device; layouts persist | 5 wk |
| **N3 — MOS gateway, Profiles 0–2** | Device configuration UI, `PeerLink`, 4.0 WebSocket (standard + passive, TLS, auth), RO mirroring through `RunningOrderAuthor`, status feedback, Devices and MOS monitor docks | Edits in the UI converge on an `imos::Device`'s RO under random churn and forced disconnects | 4 wk |
| **N4 — Media, Profiles 1 and 3** | Media browser, search and paging, drag-to-story, placeholders, `mosItemReplace` ingest | Three-channel workflow green end to end from the UI | 3 wk |
| **N5 — Prompter and control, Profiles 4, 5, 7** | Story send, cue/take, on-air highlighting, device-originated story edits through the lock table | Prompter round trip green; P7 edit appears in history with attribution | 3 wk |
| **N6 — Multi-user** | `nrcsd`, sync protocol, accounts and roles, story locks, presence, reconnect; gateway moves server-side | Convergence suite green with 10 clients; standalone mode still passes N1–N5 suites | 6 wk |
| **N7 — Legacy, Profile 6, interop** | 2.8 TCP and 3.8 SOAP device types, FQID settings, dialect quirks (as ImaginaryMOS M7 allows), `mos-connection` lab | MOS integration suite green on all three transports; interop report written | 3 wk |
| **N8 — Packaging and polish** | Installers and signing, server service units, i18n and second language, accessibility pass, user and admin documentation | Clean-machine install and first-run on each OS from CI artifacts | 3 wk |
| **N9 — Hardening → 1.0** | Soak, performance (500-story rundown, 50 k objects per device), security review, bug burn-down | Soak clean; no open data-loss or desync defects; tagged 1.0 with source release | 3 wk |

Sequencing rationale: the service boundary exists from N1 so that going
multi-user in N6 is adding a wire, not rewriting state handling. The UI
precedes MOS so the gateway has a real model to mirror. The 4.0 transport
comes first and the legacy ones last, mirroring how ImaginaryMOS itself was
built. A usable single-seat release exists after N5 if an early pilot is
wanted.

## 12. Risks

| Risk | Mitigation |
|---|---|
| KDDockWidgets' Qt Quick frontend misbehaves on Qt 6.12.0 or on one platform (Wayland especially) | N0 spike (a) before any UI work; behaviour in C++ view-models keeps a Widgets fallback affordable; upstream fixes under its GPL terms |
| Script editor in Qt Quick is harder than estimated | N0 spike (b); block model is independent of the widget toolkit |
| Scope creep — an NRCS can absorb unlimited features | §2.2 is the contract; additions need a milestone, not a branch |
| Qt 6.12.0 pin ages (security fixes arrive in later patches) | Pin is exact but reviewed each patch release; bump is one PR gated by the full suite |
| No commercial MOS devices to test against | In-process `imos::Device`, `mos-connection`, record real traffic at pilot sites; say so in release notes |
| An `incs` API gap blocks a workflow | Budget upstream time in N3–N5; fix in ImaginaryMOS with tests |
| GPL obligations missed in a release | Source tarball and notices are CI artifacts of the same job that builds installers |
| Multi-user retrofitting | Command/event service from N1; standalone is the same code path with a different carrier |

## 13. Decisions needed before N0 starts

1. **D1** — `GPL-3.0-only` (recommended) or `GPL-3.0-or-later`.
2. **D2** — DCO (recommended) or CLA.
3. **D3** — Qt Quick (recommended, subject to the N0 spike) or Qt Widgets.
4. **D4** — SQLite-only at 1.0 (recommended) or PostgreSQL from the start.
5. **D5** — story-level locking (recommended) or collaborative editing.
6. **D6** — block-based script editor (recommended) or rich-text document.
7. Product name and default `ncsID` convention.
