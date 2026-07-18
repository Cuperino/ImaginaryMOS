<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Profile 0 — Basic device communication {#profile0}

[TOC]

Profile 0 is the handshake every relationship rests on: **who are you** (machine
info) and **are you still there** (heartbeat / keepAlive). It is mandatory, and
most of it runs itself once the session is up.

Messages: `reqMachInfo`, `listMachInfo`, `heartbeat`, `keepAlive`. Channel:
machine info is valid on both **Mom** (10540) and **Ro** (10541).

## What runs automatically {#p0_auto}

Heartbeat and keepAlive are **session machinery** — @ref imos::PeerSession
handles cadence, echo, and loop avoidance from the injected clock. You configure
the intervals in @ref imos::SessionConfig and never touch them again:

```cpp
session.keepAliveIntervalMillis = 30'000; // idle keepAlive; clamped to the 30 s
                                          // spec floor; 0 disables entirely
session.heartbeatIntervalMillis = 0;      // 0 = answer heartbeats, never originate
```

Likewise, an inbound `reqMachInfo` is answered for you: the facade's Profile 0
responder builds `listMachInfo` from the @ref imos::profiles::MachineInfo you
supplied in the config, echoes the request's `messageID`, and sends it on the
requesting channel. You do not write any code for the *answering* side.

## Declaring who you are {#p0_declare}

The one thing you must get right is the @ref imos::profiles::MachineInfo in your
config, because its `profiles[]` array is how the peer learns what you can do.
Set the flags for exactly the profiles you implement:

```cpp
imos::profiles::MachineInfo info;
info.manufacturer = "Acme Broadcast";
info.model        = "VTR-7";
info.swRev        = "1.0.0";
info.id           = session.mosID;                 // your MOS (or NCS) ID
info.deviceType   = imos::profiles::DeviceType::Mos;
info.profiles     = {/*0*/true, /*1*/true, /*2*/true, /*3*/false,
                     /*4*/true, /*5*/true, /*6*/false, /*7*/true};
```

> Some facade features **override** what you put here so the advertisement stays
> honest: attaching an @ref imos::profiles::IObjectSource sets the Profile 1
> flag (and Profile 3 if the source implements it), and a valid Profile 6 config
> sets the Profile 6 flag. Declare the rest yourself.

## Asking the peer who *it* is {#p0_query}

Either role can request the peer's machine info. The reply is captured
asynchronously; read it after it arrives.

**Device asking the NCS** (or the mirror — the API is identical on both roles):

```cpp
imos::Result<imos::Seq> seq = device.requestMachineInfo(imos::Channel::Mom);
if (!seq) { /* enqueue failed — see seq.error() */ }

// ...later loop iterations, after the listMachInfo comes back...
if (const imos::profiles::PeerMachineInfo* peer = device.peerMachineInfo()) {
    const auto& mi = peer->info;
    log("peer is %s %s, MOS rev %s", mi.manufacturer.c_str(),
        mi.model.c_str(), mi.mosRev.c_str());

    if (mi.profiles[4]) {
        // The peer implements Profile 4 — safe to enumerate running orders.
    }
}
```

@ref imos::Device::requestMachineInfo (and @ref incs::Ncs::requestMachineInfo)
default to the **Mom** channel per spec §4.1.3, but accept **Ro** too; pass the
channel explicitly if your topology only brings one up. `peerMachineInfo()`
returns `nullptr` until a `listMachInfo` has been received and parsed.

## Capability negotiation pattern {#p0_negotiate}

The idiomatic bring-up sequence is: connect → `requestMachineInfo()` → wait for
`peerMachineInfo()` → gate optional workflows on the peer's `profiles[]` flags.
The library never *blocks* a message you send just because the peer didn't
advertise support (the strict/lenient duality is a per-profile choice — see, for
example, Profile 7's capability policy), but reading the flags first lets your
host avoid pointless round-trips against a peer that can't answer.

## Where results surface {#p0_events}

Machine-info replies are absorbed by the Profile 0 responder into
`peerMachineInfo()`; they do **not** appear in `nextEvent()`. What *does* appear
there are the transport-level and reliability events (channel faults after
retries, protocol errors) — Profile 0's contribution to your loop is simply that
`peerMachineInfo()` becomes non-null once negotiation completes.
