<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Profile 4 — Advanced running-order workflow {#profile4}

[TOC]

Profile 4 adds two things on top of @ref profile2 "Profile 2": **enumeration** (ask which
running orders exist and fetch them) and **story bodies** (the full script text
of a story, carried by `roStorySend`). It rides the **Ro** channel.

- Device side: @ref imos::Device::requestAllRunningOrders and the
  `hydrateRunningOrdersOnConnect` option for cold-start convergence; a story
  consumer via @ref imos::profiles::IStorySink; and story queries.
- NCS side: author bodies with @ref incs::RunningOrderAuthor::setStoryBody /
  @ref incs::RunningOrderAuthor::setAgencyStoryBody, then push with
  @ref incs::Ncs::sendStory. The NCS answers `roReqAll` with headers only.

## Device side: enumerate and hydrate {#p4_enumerate}

`roReqAll` asks the NCS for every MOS-active running order and gets back a
`roListAll` **header index**. @ref imos::Device::requestAllRunningOrders can then
fetch each listed RO into the Profile 2 shadow model in one call:

```cpp
device.requestAllRunningOrders(/*hydrate=*/true);

// ...after the exchange settles...
for (const auto& roID : device.runningOrderHeaderIDs()) {
    const imos::codec::XmlNode* header = device.runningOrderHeader(roID); // verbatim <ro> header
    // hydrate=true also populated device.runningOrderModel() for each roID.
}
```

A **cold-started device** can converge with no host code at all by setting the
config option, which fires the same enumerate-and-hydrate the moment the Ro
channel connects:

```cpp
imos::DeviceConfig config{session, info};
config.hydrateRunningOrdersOnConnect = true;   // roReqAll + fetch on Ro connect
```

> That hook lives in @ref imos::Device::onChannelConnected, so route
> connectivity through the **facade** (not straight into `session()` via a
> transport `pump`) if you rely on it — see @ref gs_loop.

## NCS side: author and send story bodies {#p4_bodies}

A story body is authored separately from the running-order *construction* — it
is script content, not structure, so `commit()` never sends it. Author it, then
push it explicitly. There are two forms.

**Full script** — a `<storyBody>` subtree of paragraphs, presenter cues, and
embedded `<storyItem>` references:

```cpp
auto& author = ncs.author();
author.setStoryBody("RO-EVENING", "S1", storyBodyNode);
ncs.sendStory("RO-EVENING", "S1");   // roStorySend; roAck resolves via author().nextEvent()
```

**Agency copy** — the spec's `Read1stMEMasBody` form, where a
`<mosExternalMetadata>` block's `mosPayload` (NewsML, IPTC, …) *is* the body.
It is stored and transmitted **byte-verbatim**, never re-normalized:

```cpp
author.setAgencyStoryBody("RO-EVENING", "S2", mosExternalMetadataNode);
ncs.sendStory("RO-EVENING", "S2");
```

Authoring a body marks the RO **story-send-linked**, which makes the Profile 2
MOVE-not-DELETE+INSERT rule *mandatory* for it: a prompter consuming the scripts
must never be told to discard and re-learn every one, so `commit()` will never
collapse such an RO's diff into a wholesale `roReplace`.

The `roAck` resolves through @ref incs::RunningOrderAuthor::nextEvent like any
committed message (see @ref profile2).

## Device side: consume stories {#p4_consume}

A prompter, publishing endpoint, or archive attaches an
@ref imos::profiles::IStorySink. It is invoked once per accepted `roStorySend`;
its outcome drives the `roAck` (return success → "OK", an error → the NACK
detail). A resend of the same `roID`/`storyID` replaces the previous content.

```cpp
class Prompter : public imos::profiles::IStorySink {
public:
    imos::Result<void> onStory(const imos::profiles::StoryView& story) override {
        if (story.body.bodyIsAgencyPayload()) {
            // Agency copy: read the verbatim payload from the first MEM block.
            const imos::codec::XmlNode* mem = story.body.agencyMetadata;
            renderAgency(mem);
        } else {
            // Walk the paragraphs as interleaved text/markup runs.
            for (const auto* p : paragraphsOf(story.body.storyBody)) {
                imos::codec::ParagraphRuns runs{*p};
                for (const auto& run : runs) append(run.text);
            }
        }
        return {};   // → roAck "OK"
    }
};

Prompter prompter;
device.attach(prompter);
```

Without a sink, accepted stories are still acked, stored, and queryable:

```cpp
for (const auto& storyID : device.storyIDs("RO-EVENING")) {
    const imos::codec::Message* send = device.story("RO-EVENING", storyID); // verbatim roStorySend
}
```

> **Profile 6 interaction.** When a classifying Profile 6 config is attached, a
> story's `<storyItem>` references are classified onto `StoryView::references`
> in document order, so a prompter can tell which referenced objects live on a
> *foreign* MOS. Agency payloads stay opaque and are never mined. See
> @ref profile6.
