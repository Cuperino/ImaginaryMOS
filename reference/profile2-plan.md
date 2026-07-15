# Profile 2 Plan — Basic Running Order / Content List Workflow

**Milestone:** M3 (completes it) · **Work order:** 2 of [1, 2, 4, 5, 7, 6, 3] (after P1, before P4)
**Prerequisites:** Profile 1 round (terminal-reply enqueue, `tests/support/` harness, `imos::Device` / `incs::Ncs` facades)
**Spec grounding:** master plan §3.2 row 2, §3.3 resync, §4.6 (`RunningOrderModel`, `incs` authoring/diff); spec copy `reference/MOS Protocol 4.0 (Current).html`

Profile 2 is the "roConstruction" family: the NCS builds and mutates running
orders on the device, the device reports element status back. This round
also delivers the two §4.6 centerpieces (`RunningOrderModel` on the device
side, the diffing RO authoring API on the NCS side) and the §3.3
resynchronization heuristic behind the session's existing `ResyncNeeded`
seam — the largest single round of M3–M5.

## 1. Scope

### 1.1 Messages (all on the **ro** channel; encoded in `tools/codegen/annotations.json`)

| Message             | Expected response | Direction (primary) | Notes |
|---------------------|-------------------|---------------------|-------|
| `roAck`             | — (terminal ack)  | both                | carries per-element status lists (`storyID`/`itemID`/`objID`/`status`) |
| `roCreate`          | `roAck`           | NCS → MOS           | full RO with stories/items |
| `roReplace`         | `roAck`           | NCS → MOS           | wholesale replacement |
| `roMetadataReplace` | `roAck`           | NCS → MOS           | RO header fields only |
| `roDelete`          | `roAck`           | NCS → MOS           | |
| `roReq`             | `roList` or `roAck` | MOS → NCS         | resync fetch; NACK `roAck` if unknown |
| `roList`            | — (terminal reply) | NCS → MOS          | authoritative RO snapshot |
| `roReadyToAir`      | `roAck`           | NCS → MOS           | |
| `roElementAction`   | `roAck`           | NCS → MOS           | the **only** sanctioned sequence-modification message in 4.0 |
| `roElementStat`     | `roAck`           | **bidirectional** (2.8.5+) | element = RO / STORY / ITEM |

`roElementAction` operations (attribute `operation`): INSERT, REPLACE,
MOVE, SWAP, DELETE, targeting stories or items via `element_target`
(insertion point) + `element_source` (payload/IDs) — generated views
`RoElementAction`/`ElementTarget`/`ElementSource` already expose this.

### 1.2 The removed-thirteen boundary

v4.0 removes `roStoryInsert`, `roStat`, & co. The codec has parsed and
normalized them onto `roElementAction`/`roElementStat` semantics since M1
(`core/codec/legacy.cpp`); `PeerSession::enqueue` already rejects them
outbound. This round's job is only to **verify the receive path**: a legacy
message arriving in 4.0 mode must reach the P2 machinery as its normalized
equivalent with a diagnostic — covered by tests, not new machinery.

### 1.3 Resync heuristic (§3.3, implemented verbatim)

If the MOS receives a message referencing an unknown `roID` / `storyID` /
`itemID`, it assumes a prior communication error: it issues `roReq` for
that RO and rebuilds from the `roList` reply, surfacing a single
"resyncing" state to the application rather than a stream of confusing
callbacks. Decision of record for this round: **unknown-ID detection lives
in the profile layer** (only it knows the IDs); `PeerSession` stays
message-agnostic and its `SessionEventType::ResyncNeeded` event is emitted
by the profile machinery through the session's event queue seam.

## 2. Current foundations

- Generated views for all ten messages (`views.h`: `RoCreate`, `Story`,
  `Item`, `RoElementAction`, `RoElementStat` with `attrElement()`, `RoAck`
  with the parallel status lists, …).
- Gated fixtures: `roCreate.xml`, `roReplace.xml`, `roMetadataReplace.xml`,
  `roDelete.xml`, `roReq.xml`, `roList.xml`, `roReadyToAir.xml`,
  `roAck.xml`, eleven `roElementAction*.xml` (one per operation/shape),
  four `roElementStat*.xml` — a ready-made mutation corpus for model tests.
- From the P1 round: `enqueueReply`, `TwoRoleFixture`, role facades.
- `SessionEventType::ResyncNeeded` already exists as the seam
  (`core/include/imos/session/peer_session.h`).

## 3. Deliverables

### 3.1 `core/profiles/running_order_model.{h,cpp}` — device-side shadow RO store

The optional-but-default model of §4.6, pure sans-I/O state:

```cpp
class RunningOrderModel {
public:
    // Apply inbound construction messages transactionally: all-or-nothing
    // per message; on failure nothing mutates and the error carries the
    // NACK statusDescription.
    Result<void> apply(const codec::Message& m);  // roCreate/roReplace/
                                                  // roMetadataReplace/roDelete/
                                                  // roElementAction/roReadyToAir
    // Queries for the host: current playlist, story/item lookup, readiness.
    // Per-element status tracking feeding roElementStat emission.
    Result<codec::Message> elementStat(/* element, ids, new status */);
    // Unknown-ID condition → the resync driver (below) takes over.
};
```

Semantics pinned by tests (the corner cases master-plan risk R-10 worries
about): MOVE to self, SWAP adjacent, INSERT at head/tail via empty target,
REPLACE of a story that changes its item set, DELETE of the story
containing the target of a queued later action (transactionality), slug
and metadata preservation through `roMetadataReplace`, and unknown-tag
retention on stored stories/items (they re-serialize verbatim into
`roList` answers on the `incs` side and status reports).

### 3.2 `core/profiles/profile2.{h,cpp}` — responders over the model

- **Device side** `Profile2Responder`: routes the construction family into
  a `RunningOrderModel` (or a raw `IRunningOrderSink` for hosts that opt
  out — interface per §4.6 with one callback per message), answers `roAck`
  (ACK, or NACK with per-element statuses from the model's error), emits
  host-initiated `roElementStat`, and owns the **resync driver**: on
  unknown-ID it pushes a `ResyncNeeded` event, sends `roReq` via
  `enqueueReply`-correct sequencing, replaces the model's RO from the
  `roList` reply, then replays nothing (spec: rebuild, don't replay) and
  clears the resyncing state.
- **NCS side**: answers the device's `roReq` from the authoring store with
  a `roList` (terminal reply), ingests `roElementStat`, acks.

### 3.3 `incs` RO authoring + diff (`incs/` grows)

```cpp
class RunningOrderAuthor {  // incs::
    // Desired-state editing API (create RO, add/move/replace stories & items)
    // commit(): diff desired vs. last-acknowledged state → minimal
    // spec-correct sequence of roCreate / roReplace / roMetadataReplace /
    // roElementAction / roDelete, enqueued in order on the ro channel.
};
```

Diff rules pinned by tests: prefer `roElementAction` MOVE over
DELETE+INSERT always (mandatory for story-send-linked ROs per §3.2 — the
P4 plan builds on this); wholesale `roReplace` only when the delta exceeds
a configurable threshold; `roMetadataReplace` for header-only changes;
unacked-commit coalescing forbidden (in-flight-of-one already serializes).

### 3.4 Bidirectional `roElementStat`

Device emits on status change (PLAY/STOP/READY etc. from the host);
NCS-originated `roElementStat` (2.8.5+ semantics) flows to the device
model. Both directions ack.

## 4. Tests (`tests/profiles/profile2_test.cpp`, plus model unit tests)

- **Model unit layer** (`tests/profiles/running_order_model_test.cpp`):
  drive every `roElementAction*.xml` fixture and the construction fixtures
  through `apply()`; assert playlist state, transactional failure paths,
  unknown-tag retention. Property-style shuffle test: random valid action
  sequences applied to model vs. an independent naive list re-derivation.
- **Conformance walk** over `TwoRoleFixture` (then ws40-parameterized):
  author an RO in `incs`, `commit()`, device model converges; mutate
  (insert/move/swap/replace/delete stories and items), converge after each
  ack; `roReadyToAir`; status round-trip both directions.
- **Diff quality**: specific edits must produce specific minimal message
  sequences (MOVE not delete+insert; metadata-only edit →
  `roMetadataReplace` only).
- **Resync**: drop the `roCreate` frame via the loopback's `pump()`
  controls so the device later sees `roElementAction` for an unknown
  `roID` → exactly one `ResyncNeeded` event, `roReq`/`roList` rebuild,
  subsequent actions apply cleanly. NACK branch: `roReq` for an RO the NCS
  deleted → NACK `roAck` handled without loop.
- **Store-backed resume (M3 exit criterion)**: crash the device session
  with a `roElementAction` unacked and construction messages queued;
  rebuild from `FileMessageStore`; sequence resumes exactly-once, model
  converges.
- **Legacy-13 receive**: inject `roStoryInsert`/`roStat` wire bytes in 4.0
  mode → normalized `roElementAction`/`roElementStat` reach the model with
  a diagnostic; outbound send has no API (compile-time absence).
- Negative: out-of-order/wrong-state messages, oversized RO (parse-limit
  rejection), `roAck` NACK with per-element statuses propagated to the
  authoring API's commit result.

## 5. Exit criteria (completes M3, per amended §13)

- P1+P2 conformance green, in-memory and ws40 loopback, on `linux` /
  `linux-asan` / `linux-fuzz` presets; `wasm` builds clean.
- Store-backed resume test green.
- mosromgr-oracle compatibility is **not** gated here (that's M8), but the
  model's public queries must already expose enough state to diff against
  it later — keep the API shaped for that.
- Format/SPDX clean; M2 baseline tests unregressed.

## 6. Out of scope

`roStorySend` and story bodies (P4); `roReqAll`/`roListAll` (P4 — note:
**not** P2 despite the name); item control (P5); story actions initiated
by the MOS (P7); 2.8 port semantics (M6).
