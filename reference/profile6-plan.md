# Profile 6 Plan — MOS Redirection

**Milestone:** M5 (opens it) · **Work order:** 6 of [1, 2, 4, 5, 7, 6, 3] (after P7, before P3 — the finale)
**Prerequisites:** none hard (no new messages); P1's object paths and P4's `storyItem` handling are the natural integration points
**Spec grounding:** master plan §3.2 row 6; spec copy `reference/MOS Protocol 4.0 (Current).html`

Profile 6 adds **no messages**. It is a naming convention plus device
behavior: the Fully Qualified MOS ID (FQID) lets a device recognize that an
object referenced in a running order lives on a *different* MOS, so it can
arrange an out-of-band transfer (the transfer itself is explicitly outside
MOS). Library work is therefore utilities + hooks + a conformance suite —
the smallest round, deliberately slotted between the M4 push and the P3
finale.

## 1. Scope

### 1.1 The FQID convention

```
<family>.<machine>.<location>.<enterprise>.mos
```

- `location` and `enterprise` are **optional** — so a bare
  `<family>.<machine>.mos` is valid, and parsing must disambiguate the
  2-, 3-, and 4-label forms (labels never contain unescaped dots; confirm
  the exact character rules against the spec HTML during implementation).
- Comparison semantics: two IDs are same-machine when
  `<machine>` (+`location`+`enterprise` where present) match; locality
  classification asks "is this object *mine* / reachable in my
  family/enterprise / foreign?".
- Claiming Profile 6 means the device uses FQIDs as its `mosID` and
  recognizes non-local `mosID`s inside `storyItem`/`item` references.

### 1.2 What the library must provide (master plan wording)

FQID **parse / compose / compare** utilities, **locality classification
hooks**, and **enforcement toggles**. Everything else (fetching the
foreign object) is host territory.

## 2. Current foundations

- `storyItem`/`item` views carry per-item `mosID`
  (`codec::gen::StoryItem::mosID()`), which is exactly where foreign
  references appear.
- `SessionConfig::mosID` and `MachineInfo::id` hold the device identity
  the classifier compares against.
- `MachineInfo::profiles[6]` advertises the capability (M2 plumbing).
- Nothing FQID-aware exists anywhere yet.

## 3. Deliverables

### 3.1 `core/include/imos/fqid.h` + `core/fqid.cpp` (core utility, not a profile responder)

```cpp
namespace imos {

/// Parsed Fully Qualified MOS ID (spec Profile 6 convention). Value type;
/// parsing is allocation-light and never throws (Result per conventions).
struct Fqid {
    std::string family, machine, location, enterprise;  // location/enterprise may be empty
    [[nodiscard]] std::string compose() const;          // canonical string form
};
Result<Fqid> parseFqid(std::string_view id);            // rejects non-".mos" / bad shapes

enum class Locality : std::uint8_t {
    Self,            ///< same machine (this device)
    SameEnterprise,  ///< machine differs; enterprise/location scope matches
    Foreign,         ///< resolvable FQID, different enterprise
    NotFqid,         ///< legacy/plain mosID — Profile 6 rules don't apply
};

/// Classify `objectMosID` relative to `self`. Pure function; the tie-break
/// rules for partially-qualified forms come from the spec text — pin them
/// with table-driven tests.
Locality classify(const Fqid& self, std::string_view objectMosID);
}
```

### 3.2 Hooks and enforcement toggles

- **Config**: `Profile6Config { bool requireFqidSelf; bool
  flagForeignReferences; bool rejectForeignReferences; }` — surfaced on
  `imos::Device` (and mirrored in `incs` for authoring-time validation).
  Defaults all-off: Profile 6 is opt-in and must not change behavior for
  non-claiming devices.
- **Classification hook**: when enabled, the P2/P4 ingest paths
  (`RunningOrderModel` story/item apply, `IStorySink` delivery) classify
  each referenced `mosID` and attach the `Locality` to the delivered
  view; `Foreign` references raise a distinct host event
  (`ForeignObjectReferenced` — the seam a host uses to start its
  out-of-band transfer) rather than an error, unless
  `rejectForeignReferences` turns them into NACKs.
- **Identity enforcement**: with `requireFqidSelf`, a non-FQID
  `SessionConfig::mosID` fails device construction with a clear
  diagnostic (catches misconfiguration at bring-up, not on air).

No new wire behavior: acks, sequencing, and codec are untouched.

## 4. Tests (`tests/profiles/profile6_test.cpp` + unit tests)

- **Parser table** (`tests/codec/fqid_test.cpp` or alongside): the 2/3/4
  -label forms, trailing-`.mos` requirement, empty labels, case handling
  and character-set edges per the spec text; compose∘parse identity;
  comparison/classification truth table (Self / SameEnterprise / Foreign
  / NotFqid), derived from the spec's own examples where it gives them.
- **Workflow conformance (§10 "P6 FQID classification")**: over the
  two-role fixture, an NCS sends an RO whose `storyItem`s reference (a)
  this device, (b) a sibling machine, (c) a foreign enterprise, (d) a
  legacy plain mosID; assert the delivered classifications, exactly one
  `ForeignObjectReferenced` event, and — with `rejectForeignReferences` —
  the NACK path with diagnostic.
- **Bring-up enforcement**: `requireFqidSelf` with a plain mosID →
  construction error; with a valid FQID → `listMachInfo` advertises
  profile 6.
- Fuzz seed: add FQID strings to the URL/parameter fuzz corpus (the
  parser is a new input surface, albeit small).

## 5. Exit criteria (Profile 6 slice of M5)

- P6 classification conformance green; parser/classifier unit tables
  green on `linux` / `linux-asan`; `wasm` builds clean.
- All-off defaults verified: full prior test suite passes with Profile 6
  code present but unconfigured.
- Format/SPDX clean. M5 (and the M3–M5 arc) completes with P3.

## 6. Out of scope

The object transfer itself (explicitly outside MOS per the spec); any
transfer-progress messaging; dialect-specific FQID leniencies (M7);
`mosID` FQID rules on the 2.8 transport (M6 revisits configuration
surface only).
