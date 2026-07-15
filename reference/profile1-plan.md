# Profile 1 Plan — Basic Object Based Workflow

**Milestone:** M3 · **Work order:** 1 of [1, 2, 4, 5, 7, 6, 3] (first; nothing before it)
**Prerequisites:** M0–M2 complete (codec, session, store, ws40 transports, Profile 0)
**Spec grounding:** master plan §3.2 row 1, §3.3, §4.6; spec copy `reference/MOS Protocol 4.0 (Current).html`

Profile 1 lets a MOS device describe its media objects to the NCS and keep
that description in sync: the device pushes `mosObj` descriptions as objects
are created/changed/deleted, and the NCS can ask for one object or for the
whole catalog. Support for Profile 0 plus at least one other profile is the
spec's minimum compatibility bar — Profile 1 is that "one other" for most
devices, which is why it leads the work order.

## 1. Scope

### 1.1 Messages (verify against `tools/codegen/annotations.json` — already encoded there)

| Message      | Channel | Expected response      | Direction (primary) |
|--------------|---------|------------------------|---------------------|
| `mosAck`     | mom     | — (terminal ack)       | both                |
| `mosObj`     | mom     | `mosAck`               | MOS → NCS           |
| `mosReqObj`  | mom     | `mosObj` or `mosAck`   | NCS → MOS           |
| `mosReqAll`  | mom     | `mosAck`               | NCS → MOS           |
| `mosListAll` | mom     | `mosAck`               | MOS → NCS           |

All Profile 1 traffic rides the **mom** channel (legacy port 10540); routing
is already automatic via the generated `MessageInfo` registry.

### 1.2 Workflow (spec narrative, both roles)

- **Push:** the MOS announces object lifecycle by sending unsolicited
  `mosObj` (created / updated / deleted per its status field); the NCS
  ingests and answers `mosAck`.
- **Single fetch:** the NCS sends `mosReqObj` for one `objID`. If the MOS
  has it, the response is the full `mosObj`; if not, a NACK `mosAck` with a
  `statusDescription`. The spec keeps `mosReqObj` as a mandatory diagnostic
  even though rare in production.
- **Full sync (`mosReqAll`):** two synchronization methods keyed on
  `<pause>`:
  - `pause == 0` → the MOS acks, then replies with the entire catalog
    batched inside **one** `mosListAll`.
  - `pause > 0` → the MOS acks, then streams **individual `mosObj`
    messages spaced by `pause` seconds**, interleavable with live traffic
    (this is the spec's mechanism for not starving the channel during a
    large sync).
- **Resync (object side, §3.3):** on receiving a message referencing an
  unknown `objID`, the NCS-side machinery issues `mosReqObj` (Profile 3
  later adds `mosReqObjList` as the recommended bulk mechanism).

## 2. Current foundations

Already available — do not rebuild:

- Typed views for all five messages plus `ObjPaths`/`MosExternalMetadata`
  etc. in `core/include/imos/codec/gen/views.h` (`MosObj`, `MosReqObj`,
  `MosReqAll` with `pause()`, `MosListAll` with `mosObj(i)` accessors,
  `MosAck` with `objID/objRev/status/statusDescription`). Never hand-edit
  `*/gen/` — regen via `tools/codegen/gen_messages.py` (no regen expected
  this round; annotations already carry P1).
- `PeerSession` (`core/include/imos/session/peer_session.h`): per-channel
  in-flight-of-one, ack/retry, dedupe, store-backed queue, and the
  `SessionEventType::{MessageReceived, AckReceived, …}` event stream.
- The Profile 0 pattern to copy: `core/profiles/profile0.{h,cpp}` — build/
  parse helpers plus a `Profile0Responder` that consumes `SessionEvent`s and
  returns `true` when it handled one.
- `codec::Message` retains unknown subtrees and opaque `mosPayload`
  losslessly, so objects round-trip verbatim by construction.

Missing (this round provides): any Profile 1 logic, a session API for
*terminal replies*, a reusable two-role conformance harness, and real
`imos`/`incs` entry points (both libraries are version-stubs today).

## 3. Deliverables

### 3.1 Session layer: terminal-reply enqueue

The generated registry says `mosObj` expects a `mosAck` — correct for an
unsolicited push. But the *same* `mosObj` sent as the answer to a
`mosReqObj` completes that exchange and must **not** itself wait for an ack
(the requester correlates it by the echoed `messageID`, exactly like
`listMachInfo` answering `reqMachInfo`). `PeerSession` already has this
mechanism privately (`enqueueInternal(channel, message, isReply=true)`,
used by `enqueueHeartbeatReply`); expose it:

```cpp
/// Queue a response that completes the peer's in-flight exchange: sent in
/// sequence but never awaits an ack of its own. Echo the request's
/// messageID before calling.
Result<Seq> PeerSession::enqueueReply(Channel channel, codec::Message message);
```

Used by every responder from now on (NACK/ACK `mosAck`s, the reqObj
`mosObj`, `mosListAll`? — no: `mosListAll` in *both* modes is spec'd with a
`mosAck` response, see the annotations; only genuinely terminal responses go
through `enqueueReply`).

### 3.2 `core/profiles/profile1.{h,cpp}` — role-agnostic services

Follow the Profile 0 shape (helpers + responder), namespace
`imos::profiles`.

**Device side (`imos` role):**

```cpp
/// Sans-I/O object provider implemented by the host device. Objects are
/// exchanged as verbatim <mosObj> subtrees (codec::Message or XmlNode
/// copies) so mosExternalMetadata/mosPayload and vendor tags pass through
/// untouched; typed access goes through codec::gen::MosObj.
class IObjectSource {
public:
    virtual std::optional<codec::Message> object(std::string_view objID) = 0;
    /// Stable snapshot enumeration for mosReqAll (batch and paced modes).
    virtual std::vector<std::string> objectIDs() = 0;
    virtual ~IObjectSource() = default;
};

class Profile1Responder {
public:
    Profile1Responder(IObjectSource* source, PeerSession* session, IClock* clock);
    bool handle(const SessionEvent& event);   // mosReqObj, mosReqAll
    void poll();                              // releases paced mosObj sends
    [[nodiscard]] std::optional<MonotonicTime> nextDeadline() const;
    // Host-driven origination:
    Result<Seq> announceObject(codec::Message mosObj);  // create/update/delete push
};
```

Paced `mosReqAll` (`pause > 0`) must stay sans-I/O: the responder records a
paced job (remaining IDs + next-release instant from the injected `IClock`)
and `poll()` enqueues the next `mosObj` when due; `nextDeadline()` feeds
host scheduling exactly like `PeerSession::nextDeadline()`. Objects that
vanish mid-stream are skipped. Batch mode builds one `mosListAll` from the
same snapshot.

**NCS side (`incs` role):**

```cpp
/// Maintains the NCS's mirror of one peer's object catalog (plan §4.6
/// "object directory", Profile 1 slice: ingest + requests; Profile 3 adds
/// search/create/action later).
class ObjectDirectory {
public:
    ObjectDirectory(PeerSession* session, IClock* clock);
    bool handle(const SessionEvent& event);
    Result<Seq> requestObject(std::string_view objID);   // mosReqObj
    Result<Seq> requestAll(int pauseSeconds);            // mosReqAll
    // Lookup API over ingested objects (verbatim messages + typed access).
};
```

Ingest rules: a `mosObj` arriving as `MessageReceived` is an unsolicited
push → ingest **and** reply `mosAck` via `enqueueReply`; a `mosObj` (or
NACK `mosAck`) arriving as `AckReceived` is the response to our
`mosReqObj` → ingest only. `mosListAll` → ingest all + ack. During a paced
sync the directory just sees ordinary pushes — no special mode.

**ACK policy:** ack immediately (plan §3.3 default), NACK with a
diagnostic `statusDescription` for unknown `objID` / malformed bodies.

### 3.3 Role facades: first real `imos` / `incs` code

§10 requires conformance to run "a real `imos` against a real `incs`".
Grow the stubs into minimal facades this round (they thicken every
subsequent round):

- `imos/`: `imos::Device` — owns a `PeerSession` per configured peer plus
  the Profile 0/1 responders; `attach(IObjectSource&)`, `poll()`,
  `nextEvent()`. Config struct per master plan §8.1 flavor, but only the
  fields these profiles need. No library threads (pull mode).
- `incs/`: `incs::Ncs` — mirror composition owning `ObjectDirectory` (and
  later the RO authoring API).
- Both keep using `imos_add_library()`; bump their CMake from version-stub
  to real sources; public headers follow `Result<T, Error>` / no-exceptions
  conventions (`docs/coding-standards.md`).

### 3.4 Shared conformance harness — `tests/support/`

New, reused by every later profile suite (today each test file hand-rolls
fakes):

- `LoopbackTransport` pair: two in-memory `ITransport`s cross-wired with
  explicit `pump()` so tests control delivery (drop/hold/duplicate frames
  for negative branches).
- `FakeClock`/`FakeRandom` (lift the ones inside
  `tests/session/session_test.cpp` into the shared header).
- `TwoRoleFixture`: a real `imos::Device` and `incs::Ncs` wired over the
  loopback pair + `InMemoryStore` (and a file-store variant for resume
  tests).
- A ws40 parameterization hook: the same scenarios re-run over
  `Ws40Client`/`Ws40Server` loopback (pattern already exists in
  `tests/transports/`).

## 4. Tests (`tests/profiles/profile1_test.cpp` + harness reuse)

Conformance walk (per master plan §10), each over in-memory pair first,
then ws40 loopback:

1. Push flow: `announceObject` → NCS ingests, acks; directory reflects
   create/update/delete.
2. `mosReqObj` hit → `mosObj` response ingested without re-ack; miss →
   NACK `mosAck` with `statusDescription` surfaced to the app.
3. `mosReqAll pause=0` → single `mosListAll` carrying the full snapshot
   (fixture-scale catalog), acked.
4. `mosReqAll pause=N` → fake-clock-driven spacing of exactly N seconds,
   **interleaved** with a live push mid-stream (spec's stated purpose);
   vanished object skipped.
5. Reliability: duplicate `mosObj` push (peer retry) suppressed by
   messageID dedupe with ack re-sent; retry-after-timeout of an unacked
   push; **store-backed resume** — kill the device session mid-sync,
   rebuild from `FileMessageStore`, pending `mosObj` delivered exactly once
   (M3 exit criterion).
6. Terminal-reply semantics: after answering `mosReqObj`, the device
   channel is idle (no phantom in-flight) — guards the `enqueueReply`
   contract.
7. Codec fidelity: gated fixtures `mosObj.xml`, `mosAck.xml`,
   `mosReqObj.xml`, `mosReqAll.xml` already round-trip in codec tests; the
   profile suite asserts pushed objects arrive byte-identical (unknown
   tags + `mosPayload` intact).

Negative/edge: unknown-objID resync hook fires `mosReqObj`; `mosReqAll`
with absent `<pause>` treated per spec default; NACK propagation.

## 5. Exit criteria (Profile 1 slice of M3)

- P1 conformance suite green on `linux`, `linux-asan`, and over ws40
  loopback; builds clean under the `wasm` preset.
- `enqueueReply` covered by session tests; no regression in the 163-test
  M2 baseline (`ctest --preset linux`).
- `clang-format --dry-run --Werror` clean on all new first-party files;
  SPDX headers everywhere.
- M3 itself completes only with Profile 2 (see `profile2-plan.md`).

## 6. Out of scope (lands later)

Profile 3's search/create/action extensions to `IObjectSource` and the
`aux` channel (see `profile3-plan.md`, last in the work order);
`mosReqObjList`-based resync; dialect quirks (M7); 2.8/3.x transports (M6).
