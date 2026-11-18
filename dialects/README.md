<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# dialects

What real peers do differently from the letter of the MOS spec, kept as data
(plan §7): `imos::DialectProfile` (`core/include/imos/dialect.h`) holds the
settings, and each named preset turns on the ones its peer needs. An NCS sets
its device's dialect in `incs::NcsConfig::dialect`.

| Preset | Peer | Status |
|---|---|---|
| `baseline` | the spec as written | every setting off |
| `sofie` | Sofie TV Automation's `mos-connection` | observed against 5.0.0 |
| iNEWS, ENPS, Octopus | — | M7 proper |

Each setting says in its comment what was seen, in which peer and version,
and how. When the peer is fixed, the setting can go.

## sofie: the delta

Observed in Atlantic Newsroom's interop lab (`tests/interop` there), where
`mos-connection` 5.0.0 plays a MOS 2.8.5 device over TCP against an NCS
built on `incs`: running orders built and edited story by story and item by
item, a story body sent with each change (Profile 4), the device's element
status, its `roReq`, an object it announces (Profile 1), and the device
restarted with nothing kept.

| What `mos-connection` does | Effect | Setting |
|---|---|---|
| Reads a `<storyBody>` whose only child is one `<p>` as no body at all: its XML reader makes a lone element an object, not a list. Two paragraphs, or one `<storyItem>`, read whole. | A one-paragraph script arrives empty. | `padLoneParagraph`: a lone `<p>` is sent with an empty `<p/>` after it. |
| Dials the NCS's query port too, besides the lower and upper ports, and keeps redialling it. | Refused connections and log noise; nothing is lost (the query client uses no heartbeats, and the spec's NCS dials the device's query port instead). Sofie can switch it off (`dontUseQueryPort`). | none |
| Reports itself connected only after heartbeats are answered on both the lower and upper ports, and redials on a timer of several seconds. | A device that starts before the NCS is listening takes seconds to send anything. | none |
| Writes fractional seconds with a comma (`2026-11-18T01:09:44,324`), as the 2.8.5 time format allows. | None: `parseSpecTime` accepts it. | none |
| Acks `mosReqAll` with `<objID>0</objID><objRev>0</objRev>`. | None. | none |

Everything else in the lab - every `roElementAction` the author sends, the
`roMetadataReplace` of a retitled running order, UCS-2 BE text including
U+FFFD for a character outside the BMP, `roDelete`, and a full resend after
the device restarts - behaves as the spec says.

Residual limitation, stated plainly: a preset encodes behaviour observed in
public code and a lab, not a vendor's certification.
