# Profile 5 Plan — Item Control

**Milestone:** M4 · **Work order:** 4 of [1, 2, 4, 5, 7, 6, 3] (after P4, before P7)
**Prerequisites:** Profile 2 (RO/story/item state in `RunningOrderModel`, ro-channel harness); P4 not strictly required but assumed present per the work order
**Spec grounding:** master plan §3.2 row 5; spec copy `reference/MOS Protocol 4.0 (Current).html`

Profile 5 is small and sharply scoped: two messages that let the newsroom
(or a prompter acting through the NCS) drive playout — timed event
notifications (`roItemCue`) and imperative transport control (`roCtrl`).

## 1. Scope

### 1.1 Messages (both on the **ro** channel; already in `tools/codegen/annotations.json`)

| Message     | Expected response | Direction | Notes |
|-------------|-------------------|-----------|-------|
| `roItemCue` | `roAck`           | either — often prompter-originated, addressed to the parent MOS | timed *notification*, not a command |
| `roCtrl`    | `roAck`           | NCS → MOS | READY / EXECUTE / PAUSE / STOP / SIGNAL |

### 1.2 Semantics to honor precisely

- **`roItemCue`** carries `roEventType` and `roEventTime` plus the
  `mosID`/`roID`/`storyID`/`itemID` coordinates (generated `RoItemCue`
  view is complete). It is a notification of something that *will/did
  happen at a time*, not a request for action. The spec's deliberately
  counterintuitive detail: when a prompter originates the cue and
  addresses it to the item's **parent MOS**, the envelope's `ncsID` slot
  identifies the *originating* party per the spec's assignment rules —
  the plan must implement exactly what the spec HTML says for that case
  and pin it with a test copied from the spec's own example
  (`roItemCue.xml`, `roItemCue-2.xml` fixtures are gated). Verify the
  exact assignment against the spec text during implementation — do not
  trust memory or this summary.
- **`roCtrl`** issues one of READY / EXECUTE / PAUSE / STOP / SIGNAL
  (`command` element in the generated `RoCtrl` view) at **RO, story, or
  item scope, selected by leaving lower-level IDs empty**: `roID` alone →
  whole RO; `roID`+`storyID` → story; all three → item. Empty-vs-omitted
  element conventions matter here (dialects stray; keep the codec's
  lenient read: empty element ≡ scope boundary).

## 2. Current foundations

- Generated views `RoItemCue` and `RoCtrl` (complete field coverage,
  including `mosExternalMetadata` riders).
- Gated fixtures: `roCtrl.xml`, `roItemCue.xml`, `roItemCue-2.xml`.
- `RunningOrderModel` (P2) resolves scope targets and knows element
  existence — `roCtrl` for an unknown target is the NACK/resync branch.
- Harness + facades from P1/P2.

## 3. Deliverables

### 3.1 `core/profiles/profile5.{h,cpp}`

```cpp
namespace imos::profiles {

enum class CtrlCommand : std::uint8_t { Ready, Execute, Pause, Stop, Signal };
enum class CtrlScope   : std::uint8_t { RunningOrder, Story, Item };

/// Decoded, scope-resolved control request handed to the device host.
struct CtrlRequest { CtrlScope scope; /* ids, command, raw message */ };
struct ItemCue     { /* ids, roEventType, roEventTime (parsed via
                        time_format.h), raw message */ };

/// Device-side handler interface (§4.6 IControlSink).
class IControlSink {
public:
    virtual Result<void> onCtrl(const CtrlRequest&) = 0;   // Result → ACK/NACK
    virtual void onItemCue(const ItemCue&) = 0;            // notification; always ack
    virtual ~IControlSink() = default;
};

class Profile5Responder {   // device side
public:
    Profile5Responder(IControlSink* sink, RunningOrderModel* model /*optional,
                      for target-existence validation*/, PeerSession* session);
    bool handle(const SessionEvent& event);
    /// Prompter-style origination toward the parent MOS/NCS:
    Result<Seq> sendItemCue(const ItemCue&);
};

// NCS side: thin origination helpers on incs — sendCtrl(scope, ids,
// command), sendItemCue(...) — plus ingest of device-originated cues.
}
```

Scope resolution rule implemented once, in the parse helper: trailing
empty/absent IDs select the scope; an inconsistent shape (e.g. `itemID`
present but `storyID` empty) is a NACK with a diagnostic
`statusDescription`, not a guess.

### 3.2 Facade growth

`imos::Device::attach(IControlSink&)`; `incs::Ncs::sendCtrl/sendItemCue`
and a cue-received event for prompter-originated traffic.

## 4. Tests (`tests/profiles/profile5_test.cpp`)

- **Scope matrix**: all five commands × three scopes over the two-role
  fixture; sink receives correctly resolved `CtrlRequest`; ACK flows;
  model-unknown target → NACK (and, when the unknown is an RO id, the P2
  resync driver fires — shared-path test).
- **Cue timing**: `roEventTime` parsed/round-tripped through
  `time_format.h` against the fixture values; cue is delivered as a
  notification (no state change in the model) and always acked.
- **Prompter origination**: device-side `sendItemCue` produces the exact
  envelope/ID assignment the spec example shows (byte-compare against the
  gated fixture, modulo declared whitespace policy).
- **Inconsistent scope shape** → NACK diagnostic; unknown `command` string
  → NACK (spec is a closed enum), unknown *extra tags* → ignored-and
  -retained (lenient rule).
- ws40 loopback parameterization as for every profile suite.

## 5. Exit criteria (Profile 5 slice of M4)

- P5 conformance green (in-memory + ws40) on `linux` / `linux-asan`;
  `wasm` builds clean; fixtures still gated by `fixtures-verify`.
- Format/SPDX clean; prior suites unregressed.
- M4 completes with P7 (`profile7-plan.md`).

## 6. Out of scope

Any playout engine behavior behind `IControlSink` (host's job); timed
*execution* of cues (the library delivers notifications; scheduling is
host policy); `roElementStat` status reporting loops (P2, already done).
