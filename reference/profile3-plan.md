# Profile 3 Plan — Advanced Object Based Workflow

**Milestone:** M5 (completes it, and the M3–M5 arc) · **Work order:** 7 of [1, 2, 4, 5, 7, 6, 3] (last, by design — the largest independent object-side surface)
**Prerequisites:** Profile 1 (`IObjectSource`, `ObjectDirectory`, mom-channel machinery); Profile 2's authoring store (for `mosItemReplace`); the P1 harness
**Spec grounding:** master plan §3.2 row 3; spec copy `reference/MOS Protocol 4.0 (Current).html`

Profile 3 upgrades the object workflow from "describe and enumerate" to
"create, modify, and search": the NCS can ask the device to create
placeholders and act on objects, the device can replace its own item data
inside NCS stories, and a schema-driven search API replaces brute-force
`mosReqAll` as the recommended resync mechanism. This round also brings the
**aux channel** into real use for the first time.

## 1. Scope

### 1.1 Messages (channels per `tools/codegen/annotations.json` — note the three-way split)

| Message                   | Channel | Expected response         | Direction | Notes |
|---------------------------|---------|---------------------------|-----------|-------|
| `mosObjCreate`            | mom     | `mosAck`                  | NCS → MOS | create object/placeholder |
| `mosReqObjAction`         | mom     | `mosAck`                  | NCS → MOS | operation attr: NEW/UPDATE/DELETE per spec |
| `mosItemReplace`          | **ro**  | `roAck`                   | MOS → NCS | replace item data inside an NCS story |
| `mosReqSearchableSchema`  | **aux** | `mosListSearchableSchema` | NCS → MOS | |
| `mosListSearchableSchema` | aux     | — (terminal reply)        | MOS → NCS | schema URLs per `mosSchema` |
| `mosReqObjList`           | aux     | `mosObjList`              | NCS → MOS | the search query |
| `mosObjList`              | aux     | — (terminal reply)        | MOS → NCS | paged results |

First real **aux** traffic (legacy query port 10542): `PeerSession` and the
ws40 transports have carried `channel=aux` since M1/M2, but no suite has
exercised three concurrently active channels with independent in-flight
sequencing — this round's conformance must.

### 1.2 Workflows

- **Placeholder create (`mosObjCreate`)**: NCS asks; on success the ACK
  `mosAck` returns the **new `objID` in `statusDescription`**
  (spec-mandated overload of that field), and the MOS then emits a full
  `mosObj` push **on the mom channel** — a deliberate cross-message,
  same-channel-family flow the tests must pin (ack first, push after).
- **Object action (`mosReqObjAction`)**: NCS modifies/deletes MOS-owned
  objects (typically ones it created as placeholders); body mirrors
  `mosObjCreate` fields plus `objID` attribute; verify the exact
  `operation` vocabulary against the spec HTML + fixtures
  (`mosReqObjAction-3.xml` is gated; two others are illustrative).
- **`mosItemReplace`**: the MOS replaces item data *it owns* inside an NCS
  story — `roID`/`storyID` coordinates plus a full `item` (P2's authoring
  store applies it; **position immutable** — a replace never moves the
  item; test pins that ordering survives).
- **Search (`mosReqObjList`)**: query with `queryID`, paging
  (`listReturnStart`/`End`), `generalSearch` free text, and
  `searchGroup`s of `searchField`s whose `XPath` attributes use the
  spec's **XPath subset**; fields within a group are **ANDed**, groups
  are **ORed**. `mosObjList` answers with paging metadata
  (`listReturnTotal`/`Status`) and a `list` of `mosObj`s. The spec
  recommends this over `mosReqAll` for resync — wire it into the P1/P2
  resync surface as the preferred bulk mechanism when the peer advertises
  Profile 3.
- **Searchable schema**: `mosReqSearchableSchema`/`mosListSearchableSchema`
  declare which `mosSchema`s object metadata can be queried under.

### 1.3 XPath-subset evaluation — decision of record needed at implementation start

The pinned libxml2 build is minimized (master plan §4.1: "no …xpath
modules beyond what `mosReqObjList` translation needs"). First task of the
round: check whether the current `3rdparty/` libxml2 configuration
compiles the XPath module in. Default decision — **hand-rolled subset
evaluator** over the retained `XmlNode` DOM (the spec subset is small:
path steps + predicates on field values; extract its exact grammar from
the spec HTML into a doc comment), keeping libxml2 minimal and the WASM
footprint down. Only fall back to enabling libxml2 XPath if the subset
turns out materially larger than expected; that choice must be recorded in
this file when made.

## 2. Current foundations

- Generated views for all seven messages (`MosObjCreate`,
  `MosReqObjAction`, `MosItemReplace`, `MosReqObjList` + `SearchGroup` +
  `SearchField` (`attrXPath`, sort attrs), `MosObjList` + `List`,
  `MosReqSearchableSchema`, `MosListSearchableSchema`).
- Fixtures: gated — `mosReqSearchableSchema.xml`,
  `mosListSearchableSchema.xml`, `mosReqObjAction-3.xml`,
  `mosItemReplace.xml`, `mosItemReplace-2.xml`; illustrative (not gated,
  leniency probes only) — `mosObjCreate`, `mosReqObjList`, `mosObjList`,
  `mosReqObjAction`(-2).
- P1's `IObjectSource`/`ObjectDirectory` and `Profile1Responder`; P2's
  authoring store; P6's FQID classifier (foreign-object references in
  search results classify like any other).

## 3. Deliverables

### 3.1 `IObjectSource` extension (P1 header grows; P3 methods optional-by-capability)

```cpp
// Added to imos::profiles::IObjectSource (defaults return Unsupported so
// P1-only devices keep compiling; Profile 3 flag in listMachInfo is set
// only when the host overrides these):
virtual Result<std::string> createObject(const ObjectCreateRequest&);   // → new objID
virtual Result<void> actOnObject(ObjAction op, std::string_view objID,
                                 const codec::Message& body);
virtual Result<SchemaList> searchableSchemas();
virtual Result<ObjectQueryPage> queryObjects(const ObjectQuery&);       // evaluated search
```

Plus `ObjectQuery`: the parsed form of `mosReqObjList` (paging, general
search, OR-of-AND groups with parsed subset-XPath field predicates), and
a library-provided `evaluateQuery(ObjectQuery, /* object corpus */)`
helper so simple hosts get correct semantics for free while database
-backed hosts translate `ObjectQuery` themselves (the plan §3.2 phrase
"what `mosReqObjList` translation needs").

### 3.2 `core/profiles/profile3.{h,cpp}`

- `Profile3Responder` (device): handles `mosObjCreate` /
  `mosReqObjAction` on mom and the search family on aux; the create flow
  sends ACK-with-objID (`enqueueReply`), then `announceObject()` (P1
  path) for the follow-up `mosObj` push; search answers are terminal
  replies with paging.
- NCS side, extending `ObjectDirectory`: `createObject()` (returns the
  objID from `statusDescription`, then correlates the follow-up `mosObj`
  push), `requestObjAction()`, `searchableSchemas()`,
  `query(ObjectQuery)` with page iteration; **resync upgrade** — when the
  peer's `listMachInfo` advertises P3, directory resync prefers
  `mosReqObjList` (scoped query) over P1's `mosReqAll`.
- `mosItemReplace` origination on the device (`replaceItem(roID, storyID,
  item)`) and NCS-side application into the P2 authoring store (position
  -preserving), acked on ro.

### 3.3 Facade growth (final M3–M5 shape)

`imos::Device` reaches the §8.1 silhouette for profiles 0–7:
`attach(IObjectSource&)` now covers P1+P3, and `MachineInfo::profiles` is
derived from what's actually attached/overridden rather than hand-set.
`incs::Ncs` closes out its object directory API. After this round the
role facades are feature-complete for 4.0 single-transport operation.

## 4. Tests (`tests/profiles/profile3_test.cpp` + evaluator unit tests)

- **Query evaluator table** (pure unit): subset-XPath predicate parsing
  (grammar cases from the spec), AND-within-group / OR-across-groups
  truth table, paging boundaries (start/end/total, empty page, past-end),
  `generalSearch` matching, deterministic sort per `sortByOrder`/
  `sortType` attributes.
- **Create flow**: `createObject` → ACK carries objID in
  `statusDescription` → subsequent `mosObj` push arrives on **mom** and
  the directory correlates it to the placeholder; NACK branch (device
  refuses) leaves no phantom directory entry.
- **Action flow**: modify/delete round-trips against `mosReqObjAction-3.xml`
  canonical form; delete triggers the P1 delete push to other observers.
- **`mosItemReplace`**: device-originated replace lands in the NCS
  authoring store with byte-preserved item payload and **unchanged
  position** (regression test with a 3-item story, replace the middle);
  NACK on unknown story routes into the resync surface.
- **Three-channel independence**: a paced P1 sync on mom + a P2 mutation
  stream on ro + a paged search on aux, concurrently, over ws40 loopback —
  per-channel in-flight-of-one preserved, no cross-channel head-of-line
  blocking (the §4.4 rule finally exercised in full).
- **Resync preference**: with P3 advertised, unknown-objID resync issues a
  scoped `mosReqObjList`; without it, falls back to P1 behavior.
- Leniency probes with the illustrative fixtures; parse-limit rejection of
  oversized search results; fuzz corpus gains `mosReqObjList` bodies (the
  subset-XPath parser is a new input surface — add it to the XML-ingest
  fuzz seeds).

## 5. Exit criteria (completes M5 and the M3–M5 arc, per amended §13)

- P3+P6 conformance green (in-memory + ws40, three channels) on `linux` /
  `linux-asan` / `linux-fuzz`; `wasm` builds clean.
- **Coverage gate: ≥ 90 % line coverage on `core/`** (`linux-coverage`
  preset), codec message-type coverage at 100 % — this is the M5 gate the
  whole arc has been building toward; wire it into CI as a failing check.
- Conformance matrix artifact (profile × message × direction × transport)
  now reports Profiles 0–7 complete on the 4.0 WS transport with no holes.
- Format/SPDX clean; every prior suite unregressed.

## 6. Out of scope

The `mosReqObjList` → 2.8/3.x transport re-parameterization (M6 re-runs
all suites per transport); dialect-specific schema registries and
`mosPayload` passthrough guarantees per vendor (M7); `incs-sim` search
personalities (M8); web-control/plug-in messages (unscheduled per OQ-6).
