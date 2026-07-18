<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# ImaginaryMOS

**ImaginaryMOS** is a portable C++20 implementation of the
[MOS Protocol v4.0](https://mosprotocol.com/) — the messaging contract between
broadcast **Newsroom Computer Systems (NCS)** and **Media Object Servers
(MOS)** such as video servers, character generators, prompters, and publishing
endpoints. It ships two role libraries over one shared, I/O-free core:

| Library | Role | Facade | Typical host |
| ------- | ---- | ------ | ------------ |
| `imos`  | Media Object Server (the device) | @ref imos::Device | Playout server, CG, prompter, still store |
| `incs`  | Newsroom Computer System | @ref incs::Ncs | Rundown/newsroom system, or the interop simulator |

Both facades compose the same **sans-I/O core**: the codec, session layer, and
profile state machines never open a socket, spawn a thread, or read a clock.
Transports, time, storage, and randomness are injected behind narrow
interfaces, which is what makes one codebase run natively on Linux, macOS,
Windows, Android, and iOS, and in the browser via WebAssembly.

## Where to start

- @ref getting_started — build the libraries, inject the runtime dependencies,
  wire a role to a transport, and run the pull-mode host loop. **Read this
  first.**
- @ref architecture — the layering, the sans-I/O rule, pull-mode integration,
  the encoding policy, and how a message crosses the stack.
- @ref profiles_overview — a dedicated guide for each MOS profile (0–7), with
  worked examples for **both** roles.

## The profiles at a glance

| Profile | Name | Guide |
| ------- | ---- | ----- |
| 0 | Basic device communication (machine info, heartbeats) | @ref profile0 |
| 1 | Basic object-based workflow (catalog push/fetch/sync) | @ref profile1 |
| 2 | Basic running-order workflow (`roConstruction`) | @ref profile2 |
| 3 | Advanced object-based workflow (create/modify/search/replace) | @ref profile3 |
| 4 | Advanced running-order workflow (enumeration + story bodies) | @ref profile4 |
| 5 | Item control (`roCtrl`, `roItemCue`) | @ref profile5 |
| 6 | MOS redirection (Fully Qualified MOS IDs) | @ref profile6 |
| 7 | MOS-controlled running-order editing (`roReqStoryAction`) | @ref profile7 |

> These profile numbers are the spec's own. The library implements all of
> Profiles 0–7 over the MOS 4.0 WebSocket transport.

## Status

The public C++ API is **not yet stable** — it is being hardened toward 1.0.
Treat signatures shown in these guides as current-but-movable, and pin a
commit if you build against them. See the project `README.md` for the milestone
status blockquote.

## License

Apache-2.0. Vendored dependencies (IXWebSocket, libxml2, zlib, GoogleTest)
retain their own licenses.
