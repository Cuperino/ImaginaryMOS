<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Profile 2 — Basic running-order workflow {#profile2}

[TOC]

Profile 2 is the **running order** (rundown): the NCS builds and edits it; the
device keeps a shadow copy and reports element status back. This is the
`roConstruction` family — `roCreate`, `roReplace`, `roMetadataReplace`,
`roElementAction` (INSERT/REPLACE/MOVE/SWAP/DELETE), `roDelete`,
`roReadyToAir` — plus bidirectional `roElementStat`. All of it rides the **Ro**
channel.

The two sides are asymmetric, and the library reflects that:

- **NCS side — you author desired state.** @ref incs::RunningOrderAuthor lets
  you edit a running order as a whole; `commit()` **diffs** your desired state
  against what the device holds and emits the *minimal* correct message
  sequence. You never hand-assemble `roElementAction`s.
- **Device side — you get a shadow model for free.** @ref
  imos::profiles::RunningOrderModel is a transactional shadow store the facade
  keeps in sync. You query it; you don't build it.

## NCS side: author and commit {#p2_author}

Reach the author via @ref incs::Ncs::author. Edit desired state with validated
mutations, then `commit()` a running order to enqueue the diff.

```cpp
auto& author = ncs.author();

author.create("RO-EVENING", "Evening News");

// Stories and items are <story>/<item> subtrees; use the builders.
auto lead = imos::incs::RunningOrderAuthor::makeStory("S1", "Lead", "1");
author.insertStory("RO-EVENING", lead);           // append (empty beforeStoryID)

auto clip = imos::incs::RunningOrderAuthor::makeItem(
    "I1", /*objID*/ "W2", /*mosID*/ "acme.vtr.example", "Fire clip");
author.insertItem("RO-EVENING", "S1", clip);

author.setHeaderField("RO-EVENING", "roChannel", "A");
author.setReadyToAir("RO-EVENING", "READY");

// Diff desired vs. the device's projected state → minimal message sequence.
imos::Result<std::vector<imos::Seq>> seqs = author.commit("RO-EVENING");
```

The diff is smart on purpose (plan §3.2): a reorder becomes a **MOVE** rather
than DELETE+INSERT (mandatory for story-send-linked ROs — see @ref profile4), a
transposition becomes a **SWAP**, a header-only change becomes
`roMetadataReplace`, and a delta past a threshold collapses into a single
wholesale `roReplace`. An edit the device would NACK is rejected **at authoring
time**, before it reaches the wire.

Each committed message resolves through a @ref incs::CommitEvent — drain them to
learn the device's per-message `roAck` verdict:

```cpp
while (auto ev = author.nextEvent()) {
    if (ev->deliveryFailed) { onLinkFault(ev->roID); continue; }
    if (!ev->ok) {
        // The roAck was a NACK; ev->statuses carries the per-element detail.
        for (auto& s : ev->statuses)
            log("NACK %s/%s: %s", s.storyID.c_str(), s.itemID.c_str(), s.status.c_str());
        continue;
    }
    onCommitted(ev->roID, ev->seq);
}
```

> A NACK on a committed message marks the device projection **unknown**, so the
> next `commit()` for that RO automatically falls back to a wholesale
> `roReplace` to re-establish a known baseline.

## Device side: the shadow model {#p2_device}

The device applies inbound construction messages into a
@ref imos::profiles::RunningOrderModel automatically. Query it via
@ref imos::Device::runningOrderModel "runningOrderModel":

```cpp
const auto& model = device.runningOrderModel();

for (const auto& roID : model.runningOrderIDs()) {
    for (const auto& storyID : model.storyIDs(roID)) {
        for (const auto& itemID : model.itemIDs(roID, storyID)) {
            // model.item(roID, storyID, itemID) → the verbatim <item> subtree
        }
    }
    if (auto air = model.readyToAir(roID); air && *air == "READY")
        cue(roID);
}
```

If you would rather own running-order state yourself, opt out of the model by
attaching an @ref imos::profiles::IRunningOrderSink — construction messages then
flow to your sink verbatim and you own resync. Most devices use the model.

## Resync: the §3.3 heuristic {#p2_resync}

When a construction message references a running order the device doesn't
recognize, the device does **not** guess — it requests a fresh copy. This shared
resync driver announces itself as a @ref imos::SessionEventType
`ResyncNeeded` event through `device.nextEvent()`, and the NCS answers from its
authored desired state (which then becomes the device's new projected baseline).
The same driver backs the unknown-ID cases in Profiles 3, 4, and 5. It is
reusable directly (@ref imos::profiles::Profile2Responder::requestRunningOrder /
`noteUnknownRunningOrder`) if you build a custom responder.

Item-reference resolution during construction is governed by
@ref imos::Device::setUnknownObjectPolicy "setUnknownObjectPolicy": `Accept` (default) records the RO with
per-element statuses even when an `objID` isn't in the catalog; `Reject` NACKs
the element.

## Element status, both directions {#p2_elemstat}

`roElementStat` (spec §3.7.1) reports that an element is playing, stopped,
ready, and so on. Either role can originate it; `<time>` is stamped from the
injected clock on send.

**Device → NCS** (the common direction — the device reports what's on air):

```cpp
imos::profiles::ElementStatusReport report;
report.element = imos::profiles::RoElement::Item;
report.roID    = "RO-EVENING";
report.storyID = "S1";
report.itemID  = "I1";
report.status  = "PLAY";
device.reportElementStatus(report);   // validated against + recorded into the model
```

**NCS → device** (2.8.5+ semantics) uses @ref incs::Ncs::sendElementStat with
the same struct. Each role drains ingested reports from the other via
`nextElementStatus()`:

```cpp
while (auto st = ncs.nextElementStatus())   // device-originated, already acked
    onDeviceStatus(*st);
```
