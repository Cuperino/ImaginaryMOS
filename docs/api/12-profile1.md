<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Profile 1 — Basic object-based workflow {#profile1}

[TOC]

Profile 1 is how a device describes its **media objects** to the NCS and keeps
that description in sync. The device pushes a `<mosObj>` whenever an object is
created, changed, or deleted; the NCS can ask for **one** object (`mosReqObj`)
or the **whole catalog** (`mosReqAll`). All Profile 1 traffic rides the **Mom**
channel.

- Device side: implement @ref imos::profiles::IObjectSource, `attach()` it,
  and push changes with @ref imos::Device::announceObject.
- NCS side: @ref incs::Ncs::requestObject / @ref incs::Ncs::requestAll, then
  read the mirror via @ref incs::Ncs::object and drain
  @ref incs::Ncs::nextDirectoryEvent.

## Building a `mosObj` {#p1_build}

Objects travel as **verbatim `<mosObj>` subtrees** so metadata and vendor tags
round-trip byte-clean. Construct one with `makeMessage` and the typed
`codec::view::MosObj` accessor:

```cpp
using imos::codec::makeMessage;
using imos::codec::MessageType;

imos::codec::Message makeClip(std::string_view objID, std::string_view status) {
    auto message = makeMessage(MessageType::MosObj);
    imos::codec::view::MosObj view{*message.body()};
    view.setObjID(objID);
    view.setObjSlug("Fire at the docks");
    view.setObjType("VIDEO");
    view.setStatus(status);          // "NEW" / "UPDATED" / "DELETED"
    return message;
}
```

The `<status>` is meaningful: a push carrying `DELETED` removes the mirror entry
on the NCS.

## Device side: serve and push {#p1_device}

### Implement the object source

@ref imos::profiles::IObjectSource is your catalog's read interface. Only two
methods are mandatory (the rest are Profile 3, defaulted to *Unsupported*):

```cpp
class MyCatalog : public imos::profiles::IObjectSource {
public:
    std::optional<imos::codec::Message> object(std::string_view objID) override {
        auto it = objects_.find(std::string{objID});
        return it == objects_.end() ? std::nullopt
                                     : std::optional{it->second};
    }
    std::vector<std::string> objectIDs() override {
        std::vector<std::string> ids;
        for (auto& [id, msg] : objects_) ids.push_back(id);
        return ids;   // a stable snapshot; drives mosReqAll
    }
    void put(std::string id, imos::codec::Message obj) {
        objects_.insert_or_assign(std::move(id), std::move(obj));
    }
private:
    std::map<std::string, imos::codec::Message> objects_;
};
```

Attach it once. The facade then answers `mosReqObj` and `mosReqAll` for you, and
**advertises Profile 1** in `listMachInfo` as a side effect of the attach:

```cpp
MyCatalog catalog;
catalog.put("W2", makeClip("W2", "UPDATED"));
device.attach(catalog);   // now serving Profile 1 (and P3 if the source supports it)
```

`mosReqAll` supports the spec's two delivery modes automatically: a batch
(`mosListAll`) or a **paced** stream that releases one `mosObj` per poll when a
pause is requested — that pacing is driven off the injected clock, sans-I/O, and
folds into `device.nextDeadline()`.

### Push unsolicited changes

When an object appears, changes, or is removed, announce it. The facade sends
the `mosObj`, assigns its `messageID`, and awaits the NCS's `mosAck`:

```cpp
imos::Result<imos::Seq> seq = device.announceObject(makeClip("P1", "NEW"));
```

The `mosAck` comes back as an ordinary @ref imos::SessionEvent
(`AckReceived`) through `device.nextEvent()` — that is where you learn the push
was accepted.

## NCS side: request and mirror {#p1_ncs}

The NCS keeps a **mirror** of the device's catalog. Ask for objects, then read
them back after they arrive, and watch the directory-event stream for what
changed.

```cpp
// One object:
ncs.requestObject("W2");

// The whole catalog, paced two seconds between objects (0 = batch):
ncs.requestAll(/*pauseSeconds=*/2);

// ...later, after pump()/poll() have run...
if (const imos::codec::Message* obj = ncs.object("W2")) {
    imos::codec::view::MosObj view{*const_cast<imos::codec::Message&>(*obj).body()};
    log("W2 slug: %s", std::string{view.objSlug()}.c_str());
}
```

Every change to the mirror surfaces as a @ref imos::profiles::DirectoryEvent —
drain them each loop:

```cpp
while (auto ev = ncs.nextDirectoryEvent()) {
    switch (ev->type) {
        using T = imos::profiles::DirectoryEventType;
        case T::ObjectUpdated:    onObject(ev->objID);  break; // push / reqObj hit / sync
        case T::ObjectDeleted:    onRemoved(ev->objID); break;
        case T::SyncAcknowledged: /* mosReqAll acked; the catalog stream follows */ break;
        case T::RequestFailed:    onMiss(ev->objID, ev->statusDescription); break;
        default: break; // ObjectCreated / ActionCompleted / SchemasListed are Profile 3
    }
}
```

> **Unsolicited pushes are ingested and re-acked; requested objects are ingested
> only.** The directory distinguishes a `mosObj` that arrives as a peer-initiated
> push (ack it) from one that answers your own `mosReqObj` (don't re-ack). You
> don't manage that — you just read the resulting `DirectoryEvent`.

## From here

Profile 3 (@ref profile3) extends this exact surface: the same
@ref imos::profiles::IObjectSource grows optional methods for **creating**,
**modifying**, and **searching** objects, and the same
@ref imos::profiles::DirectoryEvent stream gains `ObjectCreated`,
`ActionCompleted`, and `SchemasListed`. If you only need publish/fetch/sync,
Profile 1 alone is complete.
