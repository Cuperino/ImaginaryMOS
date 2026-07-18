<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Profile 3 — Advanced object-based workflow {#profile3}

[TOC]

Profile 3 extends Profile 1 with the full object lifecycle plus search. It is
the first workflow that lights up **all three channels at once**:

- **Mom** — `mosObjCreate` / `mosReqObjAction`: the NCS asks the device to
  create a placeholder, or to modify/delete an existing object.
- **Aux** — `mosReqSearchableSchema` and `mosReqObjList` / `mosObjList`: object
  **search** with a spec-subset XPath evaluator, `generalSearch`, sorting, and
  paging.
- **Ro** — `mosItemReplace`: the device replaces item data it owns inside an NCS
  story, position-preservingly.

Profile 3 builds on the same @ref imos::profiles::IObjectSource and
@ref imos::profiles::DirectoryEvent surfaces as @ref profile1 — read that guide
first.

## Enabling Profile 3 on the device {#p3_enable}

The device opts in by overriding the **optional-by-capability** methods on its
object source and returning `true` from `supportsProfile3()`. The facade reads
that to advertise Profile 3 in `listMachInfo`:

```cpp
class MyCatalog : public imos::profiles::IObjectSource {
public:
    // ... object() / objectIDs() as in Profile 1 ...

    bool supportsProfile3() const noexcept override { return true; }

    imos::Result<std::string> createObject(
        const imos::profiles::ObjectCreateRequest& req) override {
        std::string objID = allocateID();
        store(objID, req);           // create the placeholder
        return objID;                // acked back in statusDescription (see below)
    }
    imos::Result<void> actOnObject(imos::profiles::ObjAction op, std::string_view objID,
                                   const imos::codec::Message& body) override {
        return apply(op, objID, body);   // UPDATE / DELETE
    }
    imos::Result<imos::profiles::SchemaList> searchableSchemas() override {
        return imos::profiles::SchemaList{"http://schemas.example.com/news"};
    }
    // queryObjects() is optional: return Unsupported and the library evaluates
    // the query for you over object()/objectIDs().
};
```

When a create succeeds, the facade returns the new `objID` **in the `mosAck`'s
`statusDescription`** (a spec-mandated overload of that field), then pushes the
full `mosObj` (status `NEW`) through the Profile 1 path. Modify/delete are acked
and followed by a `mosObj` push with status `UPDATED` / `DELETED`.

## NCS side: create, modify, delete {#p3_create}

Build the create request with @ref imos::profiles::makeMosObjCreate and send it
via @ref incs::Ncs::createObject. The result arrives as directory events:

```cpp
ncs.createObject(
    imos::profiles::makeMosObjCreate(/*objSlug*/ "Fire at the docks",
                                     /*objType*/ "VIDEO", /*objTB*/ "59.94"));

// Modify / delete an existing object:
ncs.requestObjAction(imos::profiles::ObjAction::Update, "W2", metadataMessage);
ncs.requestObjAction(imos::profiles::ObjAction::Delete, "W2");
```

Drain @ref incs::Ncs::nextDirectoryEvent for the resolutions — Profile 3 adds
three event types on top of Profile 1's:

```cpp
while (auto ev = ncs.nextDirectoryEvent()) {
    using T = imos::profiles::DirectoryEventType;
    switch (ev->type) {
        case T::ObjectCreated:   onCreated(ev->objID); break;   // objID from statusDescription
        case T::ActionCompleted: onActed(ev->objID);   break;   // UPDATE / DELETE acked
        case T::ObjectUpdated:   onObject(ev->objID);  break;   // the follow-up mosObj push
        case T::RequestFailed:   onNack(ev->objID, ev->statusDescription); break;
        default: break;
    }
}
```

The `ObjectCreated` event carries the new `objID`; the device's follow-up
`mosObj` push then mirrors into the directory under that same `objID` as an
`ObjectUpdated`.

## NCS side: search {#p3_search}

Search runs on the **Aux** channel. First learn what schemas the device lets you
query under, then issue a @ref imos::profiles::ObjectQuery. The simplest useful
query is a `generalSearch` with a paging window:

```cpp
ncs.requestSearchableSchemas();
// ...after SchemasListed arrives: ncs.searchableSchemas() is populated...

imos::profiles::ObjectQuery query;
query.queryID         = "q-1";
query.generalSearch   = "fire docks";   // "fire and docks" / "fire dog" also parse
query.listReturnStart = 1;              // 1-based inclusive window
query.listReturnEnd   = 20;
ncs.queryObjects(query);
```

Each returned `mosObjList` page ingests its objects into the mirror **and**
queues a @ref imos::profiles::DirectoryQueryPage. Page onward by re-issuing with
the next window and the **same `queryID`** (the spec's cached-search shortcut):

```cpp
while (auto page = ncs.nextQueryPage()) {
    for (auto& objID : page->objIDs) show(objID);
    if (page->listReturnEnd < page->listReturnTotal) {
        query.listReturnStart = page->listReturnEnd + 1;
        query.listReturnEnd   = page->listReturnEnd + 20;   // same queryID
        ncs.queryObjects(query);
    }
}
```

Field-level queries (`<searchGroup>`/`<searchField>` with XPath predicates and
sort ranks) are expressed through `query.groups`; fields AND within a group and
groups OR with each other, matching the spec semantics. The evaluator is a
hand-rolled spec subset — see `reference/profile3-plan.md` for the grammar.

### Directory resync via search

@ref incs::Ncs::resyncDirectory re-mirrors the whole catalog. When the peer
advertises Profile 3 it prefers a paged match-all `mosReqObjList` (the
spec-recommended mechanism); otherwise it falls back to Profile 1's `mosReqAll`.
Opt into automatic resync when a `mosReqObj` misses with
@ref incs::Ncs::setDirectoryResyncOnMiss.

## Item replacement (Ro channel) {#p3_replace}

`mosItemReplace` lets the **device** update item data it owns inside an NCS
story — without moving the item. The device originates it via
@ref imos::Device::replaceItem "replaceItem":

```cpp
// item is a verbatim <item> subtree carrying the target <itemID>.
imos::Result<imos::Seq> seq = device.replaceItem("RO-EVENING", "S1", item);

while (auto r = device.nextItemReplaceResult()) {
    if (r->ok) onReplaced(r->roID, r->storyID);
    else       onReplaceNack(r->roID);   // escalates to the §3.3 resync driver
}
```

On the NCS, the replace is merged **position-preservingly** into the story
(present tags replace in place, new tags append, null-valued tags remove,
MEM blocks replace on first matching `mosSchema`), acked, and surfaced through
@ref incs::Ncs::nextItemReplace so the host can re-publish downstream copies:

```cpp
while (auto ingest = ncs.nextItemReplace()) {
    if (ingest->ok) republishDownstream(ingest->roID, ingest->storyID, ingest->itemID);
}
```

The merge folds into both the authoring desired state **and** the device
projection, so a later `commit()` never diff-reverts the replaced data. An OK
also folds into the device's own shadow model; a NACK escalates the RO to the
shared resync driver (see @ref p2_resync).
