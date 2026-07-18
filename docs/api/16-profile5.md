<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Profile 5 — Item control {#profile5}

[TOC]

Profile 5 is playback control and timing feedback over the running order. Two
messages, both on the **Ro** channel:

- `roCtrl` — the NCS commands **READY / EXECUTE / PAUSE / STOP / SIGNAL** at
  running-order, story, or item scope.
- `roItemCue` — a timed notification that an item event happened (or is about
  to), flowing in **either** direction (prompter feedback, teleprompter marks).

- NCS side: @ref incs::Ncs::sendCtrl, @ref incs::Ncs::sendItemCue,
  @ref incs::Ncs::nextCtrlResult.
- Device side: consume via @ref imos::profiles::IControlSink, or drain
  @ref imos::Device::nextCtrlRequest / @ref imos::Device::nextItemCue; originate
  cues with @ref imos::Device::sendItemCue.

## Scope: how a `roCtrl` addresses a target {#p5_scope}

The @ref imos::profiles::CtrlScope is selected by *which* IDs you supply — the
message carries the lower levels as empty elements (spec §3.7.3):

| Scope | Supply | Targets |
| ----- | ------ | ------- |
| `RunningOrder` | `roID` | the whole running order |
| `Story` | `roID` + `storyID` | one story |
| `Item` | `roID` + `storyID` + `itemID` | one item |

## NCS side: send control {#p5_ncs}

@ref incs::Ncs::sendCtrl takes the scope, all three ID slots (leave the unused
ones empty), and the @ref imos::profiles::CtrlCommand "CtrlCommand":

```cpp
using imos::profiles::CtrlScope;
using imos::profiles::CtrlCommand;

// Take the whole running order to air:
ncs.sendCtrl(CtrlScope::RunningOrder, "RO-EVENING", "", "", CtrlCommand::Execute);

// Ready a single item:
ncs.sendCtrl(CtrlScope::Item, "RO-EVENING", "S1", "I1", CtrlCommand::Ready);
```

The device's `roAck` resolves through @ref incs::Ncs::nextCtrlResult "nextCtrlResult":

```cpp
while (auto r = ncs.nextCtrlResult()) {   // resolves sendCtrl and sendItemCue
    if (!r->ok) onControlRejected(r->roStatus);   // e.g. unknown target
}
```

> **Unknown targets escalate to resync.** If the device receives a `roCtrl` for
> a running order, story, or item it doesn't know, it doesn't silently drop it —
> it NACKs and the unknown-ID triggers the shared §3.3 resync driver (see
> @ref p2_resync). Keeping the shadow model current is what makes control
> reliable.

## Device side: consume control {#p5_device}

Attach an @ref imos::profiles::IControlSink. `onCtrl`'s **outcome drives the
`roAck`** (success → "OK", an error → the NACK detail); a cue is a notification,
so `onItemCue` returns nothing and is always acked.

```cpp
class Playout : public imos::profiles::IControlSink {
public:
    imos::Result<void> onCtrl(const imos::profiles::CtrlRequest& req) override {
        switch (req.command) {
            using C = imos::profiles::CtrlCommand;
            case C::Ready:   return preroll(req.roID, req.storyID, req.itemID);
            case C::Execute: return play(req.roID, req.storyID, req.itemID);
            case C::Stop:    return stop(req.roID, req.storyID, req.itemID);
            default: return imos::makeError(imos::ErrorCode::Unsupported, "command not handled");
        }
    }
    void onItemCue(const imos::profiles::ItemCue& cue) override { markCue(cue); }
};

Playout playout;
device.attach(playout);
```

Without a sink, control requests and cues queue for the drains instead
(already acked "OK"):

```cpp
while (auto req = device.nextCtrlRequest()) apply(*req);
while (auto cue = device.nextItemCue())     mark(*cue);
```

## Cues, and the prompter-origination quirk {#p5_cue}

A device (typically a **prompter**) can originate a `roItemCue` back toward the
NCS with @ref imos::Device::sendItemCue. One spec detail is genuinely
counterintuitive and worth calling out: to address the item's **parent MOS**,
its name rides the envelope's `<ncsID>` slot, not `<mosID>`. The facade handles
the addressing when you pass `addresseeNcsID`:

```cpp
imos::profiles::ItemCue cue;
cue.roID        = "RO-EVENING";
cue.storyID     = "S1";
cue.itemID      = "I1";
cue.roEventType = "CUE";
cue.roEventTime = "2026-07-18T09:00:00Z";   // verbatim; parseSpecTime reads it back on receive

device.sendItemCue(cue, /*addresseeNcsID=*/"acme.vtr.example");
```

Note that in @ref imos::profiles::ItemCue, `mosID` is the **target** MOS from the
item reference embedded in the story — not the envelope originator. On the
receiving side, `roEventTime` is carried verbatim and also parsed into
`eventTime` (a @ref imos::WallClockTime) via `parseSpecTime` when the format is
recognized; `metadata` carries any `<mosExternalMetadata>` riders verbatim in
both directions. The peer's `roAck` for a device-originated cue surfaces as an
ordinary `AckReceived` through `device.nextEvent()`.
