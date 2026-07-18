<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Profiles {#profiles_overview}

The MOS Protocol groups its messages into **profiles** — coherent slices of
functionality a device and NCS can implement independently and advertise in
`listMachInfo`. ImaginaryMOS implements all of Profiles 0–7 over the MOS 4.0
WebSocket transport. Each guide below shows the workflow from **both** roles,
with code against the real facade API. All of them assume you have already built
the pieces from @ref getting_started (a configured facade, injected
dependencies, a transport, and a running host loop).

## The profiles

- @subpage profile0 — **Basic device communication.** Machine info exchange,
  heartbeats, and keepAlive. The foundation every relationship starts from.
- @subpage profile1 — **Basic object-based workflow.** The device publishes,
  serves, and syncs its media-object catalog to the NCS.
- @subpage profile2 — **Basic running-order workflow.** The NCS builds and edits
  running orders; the device shadows them and reports element status.
- @subpage profile3 — **Advanced object-based workflow.** Placeholder create,
  modify, delete; object search over an XPath subset; item-data replacement.
  The first traffic on all three channels at once.
- @subpage profile4 — **Advanced running-order workflow.** RO enumeration and
  cold-start hydration, plus full story bodies (`roStorySend`).
- @subpage profile5 — **Item control.** `roCtrl` READY/EXECUTE/PAUSE/STOP/SIGNAL
  at running-order, story, or item scope, and timed `roItemCue` notifications.
- @subpage profile6 — **MOS redirection.** Fully Qualified MOS IDs and
  foreign-reference classification, so an NCS can federate objects across
  multiple MOSes.
- @subpage profile7 — **MOS-controlled RO editing.** The device asks the NCS to
  create/update/move/delete stories under a time-bounded lease.

## How the profiles compose

The profiles layer, they don't stack in isolation:

- **0 is mandatory** — every relationship negotiates machine info first.
- **1 and 2 are the core** object and running-order workflows. **3 extends 1**
  (richer object lifecycle + search); **4 extends 2** (enumeration + story
  bodies); **5 and 7 act on** the running orders that 2/4 maintain.
- **6 is orthogonal** — it classifies the IDs the *other* profiles carry, and is
  off by default.

Unknown-ID situations in the running-order profiles converge through one shared
**§3.3 resync driver**: when a construction or control message references a
running order or element the device doesn't recognize, the device requests a
fresh copy rather than guessing. You will see it referenced from the Profile 2,
3, 4, and 5 guides.
