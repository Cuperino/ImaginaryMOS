<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# core/profiles

Per-profile (0–7) workflow services over the session layer. Populated across
**M2–M5**. So far: `profile0.{h,cpp}` (M2) — `MachineInfo` ⇄ `listMachInfo`,
lenient peer parsing, and `Profile0Responder`, which answers inbound
`reqMachInfo` on a `PeerSession` (keepAlive/heartbeat cadence lives in the
session itself).
