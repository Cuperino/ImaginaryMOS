<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Profile 7 — MOS-controlled running-order editing {#profile7}

[TOC]

Profile 7 inverts the usual authority. Normally the NCS edits the running order
(@ref profile2); here the **device** asks the NCS to create, update, move, or
delete stories, via `roReqStoryAction` on the **Ro** channel. The NCS remains
the authority: it validates the request under a **time-bounded lease**, applies
it if allowed, and the change converges back to the device through the ordinary
Profile 2/4 echo. The device **never self-applies** — it waits for the echo.

- Device side: @ref imos::Device::requestStoryAction, then
  @ref imos::Device::nextStoryActionResult.
- NCS side: policy via @ref incs::Ncs::setLeasePolicy and
  @ref incs::Ncs::setStoryActionCapabilityPolicy; audit via
  @ref incs::Ncs::nextStoryActionAudit.

## Device side: request a story action {#p7_request}

Fill in a @ref imos::profiles::StoryActionRequest for the
@ref imos::profiles::StoryAction you want; populate only the fields that
operation needs.

**Create a story** (leave `storyID` empty inside the `<story>` to have the NCS
assign one):

```cpp
imos::profiles::StoryActionRequest req;
req.operation      = imos::profiles::StoryAction::New;
req.roID           = "RO-EVENING";
req.story          = imos::incs::RunningOrderAuthor::makeStory("", "Breaking", "");
req.beforeStoryID  = "S3";                 // insert before S3 (empty = append)
req.leaseSeconds   = 30;                    // hold the edit lease this long
req.username       = "vtr-operator";
device.requestStoryAction(req);
```

**Move** one or more stories, or **delete** one:

```cpp
imos::profiles::StoryActionRequest move;
move.operation     = imos::profiles::StoryAction::Move;
move.roID          = "RO-EVENING";
move.storyIDs      = {"S5", "S6"};          // MOVE may carry several sources
move.beforeStoryID = "S2";
device.requestStoryAction(move);
```

The NCS's `roAck` resolves through @ref imos::Device::nextStoryActionResult. For
a successful **NEW**, the NCS-assigned storyID comes back in `assignedStoryID`:

```cpp
while (auto r = device.nextStoryActionResult()) {
    if (!r->ok) { onRejected(r->roStatus); continue; }   // lease denied, bad shape…
    if (!r->assignedStoryID.empty()) rememberNewStory(r->assignedStoryID);
    // The actual RO change arrives separately, as the Profile 2/4 echo into
    // device.runningOrderModel(). Do not apply it yourself.
}
```

> **UPDATE bodies** mirror Profile 4's two forms: set at most one of
> `storyBody` (literal script) or `agencyMetadata` (the `Read1stMEMasBody`
> agency form). See @ref profile4.

## NCS side: authority, leases, and capability {#p7_ncs}

The NCS applies the request under a **lease** so two editors can't clobber one
story. The default @ref incs::SingleHolderLeasePolicy grants one holder per
`{roID, storyID}` for the requested duration; supply your own
@ref incs::ILeasePolicy for house rules:

```cpp
ncs.setLeasePolicy(&myLeasePolicy);   // nullptr → the built-in single-holder TTL
```

Capability gating follows the spec's strict/lenient duality
(@ref incs::Profile7NcsResponder::CapabilityPolicy):

```cpp
using CapPolicy = imos::incs::Profile7NcsResponder::CapabilityPolicy;
ncs.setStoryActionCapabilityPolicy(CapPolicy::Strict);  // NACK peers that never
                                                        // advertised Profile 7
// CapPolicy::Lenient (default): accept and record an audit entry regardless.
```

Every resolved request lands in the audit stream — drain it for a simulator or
an operations log:

```cpp
while (auto a = ncs.nextStoryActionAudit()) {
    log("%s story %s in %s by %s → %s",
        a->accepted ? "applied" : "rejected",
        a->storyID.c_str(), a->roID.c_str(), a->username.c_str(), a->detail.c_str());
}
```

When accepted, the NCS makes the edit as an ordinary author operation, so it
reaches the device as the normal Profile 2/4 construction echo — one code path,
whether the change originated at the newsroom desk or the device.
