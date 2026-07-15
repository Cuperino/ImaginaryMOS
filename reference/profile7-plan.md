# Profile 7 Plan — MOS RO / Content List Modification

**Milestone:** M4 (completes it) · **Work order:** 5 of [1, 2, 4, 5, 7, 6, 3] (after P5; P6 and P3 follow)
**Prerequisites:** Profile 4 (`roStorySend` body model and codec helpers — `roReqStoryAction` wraps them); Profile 2 (authoring store on the NCS side)
**Spec grounding:** master plan §3.2 row 7; spec copy `reference/MOS Protocol 4.0 (Current).html`

Profile 7 inverts the usual authorship direction: the **MOS requests story
create/update/move/delete on the NCS**, wrapping `roStorySend` payloads.
It's one message, but its edges (lease locking, attribution, NCS-side
authority) are called out by §10 as their own conformance item ("P7 lease
handling").

## 1. Scope

### 1.1 Message (ro channel; already in `tools/codegen/annotations.json`)

| Message            | Expected response | Direction | Notes |
|--------------------|-------------------|-----------|-------|
| `roReqStoryAction` | `roAck`           | MOS → NCS | attributes `operation`, `leaseLock`, `username`; body wraps one `roStorySend` |

Generated view `RoReqStoryAction` is complete: `attrOperation()`,
`attrLeaseLock()`, `attrUsername()`, `roStorySend()` child. Five gated
fixtures (`roReqStoryAction*.xml`, one per operation/shape) define the
canonical forms — extract the exact `operation` vocabulary and the
`leaseLock` value format from the spec HTML + fixtures during
implementation rather than assuming (the fixtures are the source of
truth for e.g. how MOVE expresses its target).

### 1.2 Semantics

- The wrapped `roStorySend` carries the story content for CREATE/UPDATE
  (reusing P4's body model verbatim — including `Read1stMEMasBody`
  passthrough) and the story coordinates for MOVE/DELETE.
- **`leaseLock`**: the MOS asks the NCS to hold an edit lease on the story
  so a device-driven edit isn't clobbered mid-flight; the NCS grants or
  refuses via the `roAck` (NACK with diagnostic on conflict/expiry).
  Lease *policy* (duration, renewal) is NCS-side; the device side only
  requests and reports.
- **`username`** attributes the change for newsroom audit trails — carried
  verbatim, never synthesized.
- The NCS remains authoritative: an accepted action mutates the authoring
  store, which then flows to *all* devices through the normal P2/P4
  channels (roElementAction / roStorySend). The requesting device must
  not locally apply its own request — it waits for the echo like everyone
  else (prevents divergence; test pins this).
- Capability is advertised via the **Profile 7 flag in `listMachInfo`**
  (`profiles::MachineInfo::profiles[7]`, wired since M2); the NCS may NACK
  requests from peers that didn't advertise it.

## 2. Current foundations

- `RoReqStoryAction` view; five gated fixtures.
- P4's `story_body` codec helpers and `roStorySend` build path (device
  side gets a *builder* role here for the first time).
- P2's `RunningOrderAuthor`/authoring store — the apply target.
- `MachineInfo::profiles` from M2's Profile 0.

## 3. Deliverables

### 3.1 `core/profiles/profile7.{h,cpp}`

```cpp
namespace imos::profiles {

enum class StoryAction : std::uint8_t { Create, Update, Move, Delete /* per spec vocab */ };

/// Device-side origination API.
class Profile7Requester {
public:
    Profile7Requester(PeerSession* session);
    /// Builds roReqStoryAction wrapping a roStorySend payload; leaseLock/
    /// username optional per spec. Completion (roAck) surfaces through the
    /// session's AckReceived event with the ack's statusDescription.
    Result<Seq> requestStoryAction(StoryAction op, /* story coords + body or
                                   payload */, std::optional<LeaseRequest>,
                                   std::string_view username);
};

/// NCS-side authority: validates, consults the lease table, applies to the
/// authoring store, answers roAck.
class Profile7Responder {
public:
    Profile7Responder(/* authoring store */, ILeasePolicy* leases, PeerSession*);
    bool handle(const SessionEvent& event);
};

/// Pluggable NCS lease policy (sans-I/O: expiry driven by injected clock
/// time passed into decide()); default: single-holder, fixed TTL.
class ILeasePolicy { /* acquire/renew/release/decide */ };
}
```

### 3.2 Facade growth

`imos::Device::requestStoryAction(...)` (§4.6 names this helper);
`incs::Ncs` gains the lease policy hook and an audit event (operation,
username, verdict) so simulator scenarios can assert attribution.

## 4. Tests (`tests/profiles/profile7_test.cpp`)

- **Round-trip per operation**: build → wire → NCS apply for each of the
  five fixture shapes; the built messages byte-match the gated fixtures
  (modulo declared whitespace policy); authoring store reflects the
  change; the device receives the resulting P2/P4 echo and only then
  updates its model (no local self-apply — regression test).
- **Lease handling (§10 conformance item)**: grant → concurrent second
  device's conflicting request NACKed with diagnostic; expiry via fake
  clock → renewal path; release on completion; NACK propagation into the
  requester's result.
- **Capability gating**: peer without the P7 flag in its `listMachInfo` →
  NCS NACKs (configurable strictness — default warn+accept or reject?
  pick **reject when strict-mode, accept-with-diagnostic otherwise**, in
  line with the strict/lenient duality of §4.1).
- **Payload fidelity**: UPDATE wrapping a `Read1stMEMasBody` NewsML story
  → payload bytes preserved end-to-end (reuses P4's fixture).
- Negative: unknown operation attribute → NACK; wrapped body failing parse
  limits → rejected before profile logic; ws40 loopback parameterization.

## 5. Exit criteria (completes M4, per amended §13)

- P4+P5+P7 conformance suites green (in-memory + ws40) on `linux` /
  `linux-asan` / `linux-fuzz`; `wasm` builds clean.
- NewsML-payload preservation tests green (M4 exit criterion, shared with
  P4).
- Lease conformance tests green; fixtures gate clean.
- Format/SPDX clean; all prior suites unregressed.

## 6. Out of scope

NCS-side newsroom UI/workflow around leases (host concern); Profile 6's
FQID enforcement on wrapped story items (lands next, `profile6-plan.md`);
`incs-sim` lease-conflict scenario verbs (M8 tooling, though the API here
must not preclude them).
