# Profile 4 Plan — Advanced RO / Content List Workflow

**Milestone:** M4 (opens it) · **Work order:** 3 of [1, 2, 4, 5, 7, 6, 3] (after P2, before P5; **P3 is deferred to last** — do not assume its machinery exists)
**Prerequisites:** Profile 2 (`RunningOrderModel`, `RunningOrderAuthor` diff engine, ro-channel conformance harness)
**Spec grounding:** master plan §3.2 row 4; spec copy `reference/MOS Protocol 4.0 (Current).html`

Profile 4 adds RO enumeration and full story bodies: `roReqAll`/`roListAll`
let a device discover every MOS-active RO, and `roStorySend` carries
complete scripts (mixed prose + embedded item references) to prompters and
publishing endpoints. The `Read1stMEMasBody` pattern makes this the plan's
NewsML-payload-preservation gate.

## 1. Scope

### 1.1 Messages (all on the **ro** channel; already in `tools/codegen/annotations.json`)

| Message       | Expected response | Direction (primary) | Notes |
|---------------|-------------------|---------------------|-------|
| `roReqAll`    | `roListAll`       | MOS → NCS           | empty body; "list everything MOS-active" |
| `roListAll`   | — (terminal reply)| NCS → MOS           | `<ro>` **headers only** (roID/roSlug/channel/times/trigger + metadata); details via P2's `roReq` |
| `roStorySend` | `roAck`           | NCS → MOS           | full story body; see 1.2 |

### 1.2 `roStorySend` body model

`storyBody` is the one genuinely irregular content model in this milestone
(master plan §4.5 already anticipates hand-written codec work here):

- Mixed content inside `<p>`: character runs interleaved with inline
  `<em>`, `<pi>` (presenter instruction), `<tab>` markers. The generated
  `StoryBody` view exposes `p(i)`/`storyPresenter(i)`/`storyItem(i)`
  children, but paragraph-run iteration (text/em/pi/tab in document order)
  needs a small hand-written helper layer.
- `<storyItem>`: embedded references to MOS objects placed in the story
  (generated `StoryItem` view is complete: itemID/objID/mosID/channels/
  timing/macros/mosExternalMetadata).
- **`Read1stMEMasBody="true"`** (attribute on `storyBody`, accessor already
  generated): the first `mosExternalMetadata` block *substitutes for* the
  body — agency copy (NewsML etc.) arrives this way and the `mosPayload`
  must be preserved **verbatim** (the codec's opaque-payload invariant does
  the heavy lifting; this profile must simply never normalize or re-wrap
  it on the way to the host).

### 1.3 Sequencing rule inherited by `incs`

Story moves on story-send-linked ROs must use `roElementAction` MOVE,
never DELETE+INSERT (a delete would tell the prompter to discard the
script). P2's diff engine already prefers MOVE universally; this round
makes the rule **mandatory** (not heuristic) for ROs flagged as
story-send-linked, with a test that would catch a regression to
delete+insert.

## 2. Current foundations

- Generated views: `RoReqAll`, `RoListAll` (`ro(i)`), `Ro` (header
  fields), `RoStorySend`, `StoryBody`, `StoryItem` — all present in
  `core/include/imos/codec/gen/views.h`.
- Fixtures: `roReqAll.xml`, `roListAll.xml`, `roStorySend.xml` (gated);
  `roStorySend-2/-3.illustrative.xml` are the spec's own non-well-formed
  fragments — **not** gated, do not "fix" them, but use them as parser
  leniency probes.
- P2's `RunningOrderModel` (device) and authoring store (NCS) provide both
  ends' state; P1's harness provides the two-role fixtures.

## 3. Deliverables

### 3.1 Codec helpers: `core/codec/story_body.{h → include/imos/codec/story_body.h, cpp}`

Hand-written over `XmlNode` (not generated — mark clearly why):

```cpp
namespace imos::codec {
/// Document-order iteration of a <p> element's mixed content:
/// Text | Em | Pi | Tab runs, lossless (unknown inline tags surface as
/// Raw runs and re-serialize verbatim).
class ParagraphRuns { /* begin()/end() over Run{kind, text, node} */ };
/// Read1stMEMasBody resolution: returns the body paragraphs, or the first
/// mosExternalMetadata as an opaque payload view when the attribute is
/// "true" — never a re-serialized copy.
struct ResolvedStoryBody { /* variant<Paragraphs, OpaquePayloadRef> */ };
Result<ResolvedStoryBody> resolveStoryBody(const gen::RoStorySend&);
}
```

### 3.2 `core/profiles/profile4.{h,cpp}`

- **Device side:**
  - `IStorySink` (§4.6): `onStory(const StoryView&)` — invoked per
    accepted `roStorySend` with the resolved body; ack/NACK via return
    `Result`. Prompter-shaped: stories keyed by roID/storyID, replace-on
    -resend semantics.
  - RO discovery: `requestAllRunningOrders()` → `roReqAll`; the `roListAll`
    reply populates a header index in (or alongside) `RunningOrderModel`;
    per-RO detail then flows through the existing P2 `roReq` path — wire
    these together so a cold-started device can fully hydrate: `roReqAll`
    → for each interesting header → `roReq` → `roList` → model.
- **NCS side:**
  - Answer `roReqAll` from the authoring store with a headers-only
    `roListAll` (terminal reply; strip story/item detail).
  - `sendStory(roID, storyID)`: build `roStorySend` from authored content,
    including `Read1stMEMasBody` passthrough when the authored story *is*
    an agency payload; mark the RO story-send-linked for the diff rule.

### 3.3 `imos` / `incs` facade growth

`imos::Device::attach(IStorySink&)`; hydrate-on-connect option using the
roReqAll flow. `incs::Ncs` exposes `sendStory` and the story-send-linked
flag on its authoring API.

## 4. Tests (`tests/profiles/profile4_test.cpp` + codec tests)

- **Codec layer** (`tests/codec/story_body_test.cpp`): paragraph-run
  iteration over `roStorySend.xml` (order, tab/pi/em boundaries, unknown
  inline tag → Raw run, byte-clean re-serialization); leniency probes with
  the two illustrative fixtures (must parse under the production-lenient
  policy or fail with the documented diagnostic — pin whichever the codec
  does today).
- **NewsML preservation (M4 exit criterion)**: build a fixture with a
  multi-kilobyte NewsML `mosPayload` under `Read1stMEMasBody="true"`; send
  NCS → device over ws40 loopback in **both wire encodings** (UCS-2 BE
  binary frames and the UTF-8 text-frame option); the payload bytes the
  sink sees must equal the authored bytes exactly.
- **Conformance walk**: cold device hydration (`roReqAll` → headers →
  `roReq` per RO → model converges with P2 state); story send/resend
  replaces content; story move on a story-send-linked RO emits MOVE
  (regression test asserts no DELETE+INSERT ever appears on the wire);
  `roStorySend` for an unknown `roID` triggers the P2 resync driver, and
  the story applies after rebuild.
- **Scale probe** (pre-M9 benchmark seed, not a gate): 10k-story
  `roListAll` parse + index time recorded; multi-megabyte `roStorySend`
  within parse limits.
- Negative: NACK from sink propagates `statusDescription`; oversized body
  rejected by `XmlParseLimits` before profile logic (hardening rule §12).

## 5. Exit criteria (Profile 4 slice of M4)

- P4 conformance green (in-memory + ws40) on `linux` / `linux-asan`;
  `wasm` builds clean.
- NewsML-payload preservation tests green in both wire encodings.
- MOVE-rule regression test green.
- Format/SPDX clean; prior suites unregressed. M4 completes with P5 + P7.

## 6. Out of scope

`roReqStoryAction` (P7 — wraps this profile's `roStorySend`; next-but-one
in order); `mosItemReplace` (P3, despite touching stories — it rides the
ro channel but belongs to the object workflow, deferred to last);
prompter example app (docs/examples, M9-adjacent).
