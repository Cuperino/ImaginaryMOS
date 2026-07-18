<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Profile 6 — MOS redirection {#profile6}

[TOC]

Profile 6 answers one question about every object reference a message carries:
**does this object live on *my* MOS, elsewhere in my enterprise, or on a foreign
MOS I must fetch it from out-of-band?** It adds **no messages** — it is a set of
**Fully Qualified MOS ID (FQID)** utilities plus opt-in enforcement wired into
the object/running-order ingest paths. The transfer of a foreign object itself
stays *outside* MOS, exactly as the spec intends.

Everything here is **off by default** — a non-claiming device behaves exactly as
if Profile 6 didn't exist.

## Fully Qualified MOS IDs {#p6_fqid}

An FQID is the dotted `family.machine[.location[.enterprise]].mos` shape. The
value layer in `<imos/fqid.h>` parses, composes, and classifies them:

```cpp
imos::Result<imos::Fqid> parsed = imos::parseFqid("vtr.acme.studio.corp.mos");
if (parsed) {
    const imos::Fqid& id = *parsed;
    // id.family, id.machine, id.location, id.enterprise
    std::string round = id.compose();   // == the original for every valid id
}
```

@ref imos::classify decides where a referenced object lives relative to a
`self` FQID, yielding a @ref imos::Locality "Locality":

| Locality | Meaning | Action |
| -------- | ------- | ------ |
| `Self` | same machine + location (family may differ — colocated) | nothing to transfer |
| `SameEnterprise` | different machine/location, same enterprise scope | reachable intra-enterprise |
| `Foreign` | valid FQID, **different enterprise** | the out-of-band transfer case |
| `NotFqid` | legacy/plain mosID | Profile 6 rules don't apply |

The tie-breaks are precise (and pinned by unit tables): comparison is exact and
case-sensitive, `family` never affects locality, and enterprise scope alone
bounds `SameEnterprise`.

## Enabling enforcement {#p6_enable}

@ref imos::profiles::Profile6Config has three independent toggles:

```cpp
imos::profiles::Profile6Config p6;
p6.requireFqidSelf         = true;  // my own ID must be an FQID (identity)
p6.flagForeignReferences   = true;  // post ForeignObjectReferenced per foreign ref
p6.rejectForeignReferences = false; // (compose with the flag to NACK instead)

imos::DeviceConfig config{session, info};
config.profile6 = p6;               // classification is relative to session.mosID
```

Because a misconfiguration here should be caught at bring-up rather than on air,
**gate the config through the static validator before constructing** the
facade — it fails when Profile 6 is enabled but the relevant IDs are not FQIDs:

```cpp
if (imos::Result<void> gate = imos::Device::validateConfig(config); !gate) {
    fatal("Profile 6 bring-up: %s", gate.error().message().c_str());
}
imos::Device device{std::move(config), deps};
```

> A facade built from a config the validator rejects still *runs* — but with
> Profile 6 **disabled**, and it posts one `ProtocolError` session event
> carrying the diagnostic. The validator lets you fail fast instead.

The NCS mirror, @ref incs::Ncs::validateConfig with @ref incs::NcsConfig, checks
this NCS's `ncsID` for identity and — when flag/reject is on — the **peer
device's** `mosID` (the classification base for authored references).

## Classifying references {#p6_classify}

Ask directly where any `mosID` lives:

```cpp
switch (device.classifyMosID(someObjectMosID)) {
    case imos::Locality::Self:
    case imos::Locality::SameEnterprise: useDirectly(); break;
    case imos::Locality::Foreign:        arrangeTransfer(); break;
    case imos::Locality::NotFqid:        legacyPath(); break;
}
```

`classifyMosID` returns `NotFqid` whenever Profile 6 is not enabled-and-valid,
so it is always safe to call.

With `flagForeignReferences` on, ingested running-order and story content is
classified automatically: each foreign reference surfaces as a
@ref imos::SessionEventType `ForeignObjectReferenced` event through
`nextEvent()`, and story references appear on
@ref imos::profiles::StoryView "StoryView::references" (see @ref profile4). That
event is the seam where your host kicks off the out-of-band fetch:

```cpp
while (auto ev = device.nextEvent()) {
    if (ev->type == imos::SessionEventType::ForeignObjectReferenced) {
        arrangeOutOfBandTransfer(ev->diagnostic);   // names the foreign object
    }
}
```

With `rejectForeignReferences`, those same messages are **NACKed** before
storage instead of accepted-and-flagged.

## Authoring-time rejection (NCS) {#p6_author}

On the NCS, the same classifier is available to @ref incs::RunningOrderAuthor "RunningOrderAuthor":
with `rejectForeignReferences` armed, an edit or story body that references an
object **foreign to the peer device** fails at authoring time (an
`InvalidArgument` from the edit call) rather than NACKing on air — you learn the
reference can't be satisfied before you ever commit. @ref incs::Ncs::classifyMosID
classifies relative to the peer device's `mosID` for the same reason.
