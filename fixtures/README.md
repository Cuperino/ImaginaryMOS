<!--
SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
SPDX-License-Identifier: Apache-2.0
-->

# Spec fixtures

Test fixtures extracted **verbatim** from the project's local copies of the
official specifications: `reference/MOS Protocol 4.0 (Current).html` (the MOS
Protocol v4.0.1 document published at
<https://mosprotocol.com/wp-content/MOS-Protocol-Documents/MOSProtocolVersion40/>)
and, for the M6 3.x compatibility target, `reference/MOS Protocol 3.8.4
(Current).html`.

Everything here is generated, never hand-authored. Regenerate with:

```sh
python3 tools/extract_fixtures.py "reference/MOS Protocol 4.0 (Current).html" fixtures
python3 tools/extract_wsdl.py "reference/MOS Protocol 3.8.4 (Current).html" fixtures
```

## Contents

| Path | Source | Notes |
|---|---|---|
| `schema/mos4.dtd` | spec chapter 8 | Consolidated DTD: 155 `<!ELEMENT>` + 18 `<!ATTLIST>`. Includes the thirteen messages v4.0 removes (`roStoryInsert`, `roStat`, …) so the codec can *parse* legacy traffic (plan §3.2). |
| `schema/mos4.xsd` | spec chapter 8 | The MOS 4.0 XSD schema. |
| `spec-examples/*.xml` | spec chapters 3 & 5 | One file per `<mos>`-rooted example message, named by payload message type + spec section. |
| `spec-examples/manifest.json` | generated | Catalogue: `file`, `message`, `section`, `wellformed`, and (when not well-formed) `reason`. |
| `wsdl/MOSWebService-3.8.2.wsdl` | 3.8.4 spec chapter 8 | The MOS web-service WSDL, verbatim modulo HTML markup. Quirk of record: the 3.8.4 document ships a WSDL self-versioned **3.8.2** — that *is* the current bindings artifact (reference/m6-plan.md §2.1). |
| `wsdl/manifest.json` | generated | Pinned bindings summary (24 operations, namespace, SOAP 1.1 document/literal) that the ws3 tests and the conformance matrix assert against. |

## Fidelity and whitespace policy

Content is byte-faithful to the spec. The only transformations applied are HTML
un-escaping (the source is a calibre HTML export) and collapsing runs of blank
lines introduced by the HTML wrapper (plan §4.5 whitespace policy). Nothing is
"corrected".

## Well-formed vs. illustrative

Of 71 extracted examples, **59 are standalone well-formed XML** and are the
round-trip corpus for the codec. **12 are `*.illustrative.xml`**: examples the
spec itself prints in a form that is not standalone well-formed. These are kept
verbatim and flagged with a `reason` in the manifest, because they document real
protocol shapes the codec must tolerate:

- **Opaque `<mosPayload>` foreign XML** — mixed-content and embedded `<?xml?>`
  declarations (e.g. `roStorySend-2`, `mosObj-2`). The payload is meant to ride
  through untouched (plan §4.5 "opaque foreign payloads").
- **Raw `<`/`>` inside XPath attributes** — `mosReqObjList` / `mosObjList`
  search fields such as `XPath="/Video/Length [.>60 AND <120]"`.
- **A spec typo** — `keepAlive-2` prints `</keepAlive>` with no opening tag.
- **Extraction limits** — `ncsItem`, `roStorySend-3`: the calibre export mangled
  an auto-hyperlinked file path; a clean equivalent (`roStorySend.xml`) exists.

The `fixtures-verify` target (see `tests/`) enforces well-formedness on the 59
and asserts the DTD and XSD load, which is the M0 "fixtures verified against
spec" gate. Full XSD validation of every example is an M1 codec concern.
