#!/usr/bin/env python3
"""Extract the MOS 4.0 DTD, XSD, and example messages from the spec HTML.

Source of truth is the official spec (reference/'MOS Protocol 4.0
(Current).html'), a calibre-exported HTML in which XML content is
entity-escaped and wrapped in <p>/<span>/<br> markup. The document is not
vendored; if it is missing it is downloaded on demand by tools/fetch_spec.py
(mosprotocol.com; TLS verification disabled + SHA-256 pin — see that module).

Produces, under the output directory:
  schema/mos4.dtd          consolidated DTD (spec chapter 8)
  schema/mos4.xsd          XSD schema (spec chapter 8)
  spec-examples/*.xml      one file per <mos>-rooted example message
  spec-examples/manifest.json   catalogue: file, message type, section, well-formedness

Fidelity rules:
  * Content comes verbatim from the spec; only HTML un-escaping and blank-line
    collapsing are applied (whitespace policy, plan §4.5).
  * Nothing is invented or "corrected". Examples that the spec itself prints in
    a non-well-formed way (raw <,> inside XPath attributes, a <mosPayload> that
    embeds its own <?xml?> declaration, the keepAlive close-tag typo, ...) are
    kept as-is and flagged wellformed:false in the manifest.
"""
from __future__ import annotations

import json
import re
import sys
from html.parser import HTMLParser
from pathlib import Path
from xml.dom import minidom

SPEC = Path(sys.argv[1])
OUT = Path(sys.argv[2])

if not SPEC.exists():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import fetch_spec

    fetch_spec.ensure_path(SPEC)

BLOCK_TAGS = {"p", "div", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6",
              "table", "ul", "ol", "blockquote"}
ENVELOPE = {"mosID", "ncsID", "messageID"}

# Documented errata for typos in the spec's *own* chapter-8 DTD that stop it
# loading as a valid DTD. Each is a minimal, transparent correction; the
# verbatim source stays the spec HTML in reference/. (pattern, repl, note)
DTD_ERRATA = [
    (re.compile(r"<!ELEMENT\s+keepalive\s*>"),
     "<!ELEMENT keepAlive EMPTY>",
     "spec ch.8 prints '<!ELEMENT keepalive>' (no content model, miscased); "
     "the mos content model and the XSD use 'keepAlive' as an EMPTY element"),
    (re.compile(r"<!ELEMENT\s+mosProfile\s+\(#PCDATA\)\s+<!ATTLIST"),
     "<!ELEMENT mosProfile (#PCDATA)>\n<!ATTLIST",
     "spec ch.8 prints '<!ELEMENT mosProfile (#PCDATA)' without its closing "
     "'>', merging it into the following <!ATTLIST> (a corrected duplicate "
     "also appears later in the spec DTD)"),
]


class TextExtractor(HTMLParser):
    """Render calibre HTML to plain text: <br> and block boundaries -> newline."""

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.parts: list[str] = []

    def handle_starttag(self, tag, attrs):
        if tag == "br" or tag in BLOCK_TAGS:
            self.parts.append("\n")

    def handle_startendtag(self, tag, attrs):
        if tag == "br":
            self.parts.append("\n")

    def handle_endtag(self, tag):
        if tag in BLOCK_TAGS:
            self.parts.append("\n")

    def handle_data(self, data):
        self.parts.append(data)

    def text(self) -> str:
        return "".join(self.parts)


def normalize(text: str) -> str:
    text = text.replace(" ", " ")               # nbsp -> space
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = "\n".join(line.rstrip() for line in text.split("\n"))
    return text


def collapse_blank(text: str) -> str:
    return re.sub(r"\n{2,}", "\n", text).strip("\n") + "\n"


def is_wellformed(xml: str) -> bool:
    try:
        minidom.parseString(xml)
        return True
    except Exception:  # noqa: BLE001
        return False


def find_examples(doc: str) -> list[tuple[int, str, bool]]:
    """Return (start_offset, body, terminated) for every <mos> example.

    Pairs each <mos> with its </mos> without crossing a following <mos>, so a
    spec example with a missing </mos> is captured on its own and flagged
    rather than swallowing the next example.
    """
    out: list[tuple[int, str, bool]] = []
    i = 0
    n = len(doc)
    while True:
        start = doc.find("<mos>", i)
        if start < 0:
            break
        close = doc.find("</mos>", start + 5)
        nxt = doc.find("<mos>", start + 5)
        if close < 0:
            body = doc[start:(nxt if nxt >= 0 else n)]
            out.append((start, body.strip(), False))
            i = nxt if nxt >= 0 else n
        elif nxt >= 0 and nxt < close:
            # current <mos> is unterminated before the next one opens
            out.append((start, doc[start:nxt].strip(), False))
            i = nxt
        else:
            out.append((start, doc[start:close + 6].strip(), True))
            i = close + 6
    return out


def heading_index(doc: str) -> list[tuple[int, str]]:
    """Offsets of numbered section headings, e.g. '3.1.1 mosAck'."""
    idx = []
    for m in re.finditer(r"(?m)^\s*(\d+(?:\.\d+){1,3})\.?\s+([A-Za-z][^\n]*)",
                          doc):
        idx.append((m.start(), m.group(1)))
    return idx


def section_for(offset: int, headings: list[tuple[int, str]]) -> str:
    sec = ""
    for pos, num in headings:
        if pos <= offset:
            sec = num
        else:
            break
    return sec


def illustrative_reason(body: str, terminated: bool) -> str:
    """Explain, from the text itself, why an example is not standalone XML.

    These are faithful extractions of examples the spec prints in a
    non-well-formed way; the reason is recorded so the fixtures are honest.
    """
    if not terminated:
        return "spec omits the closing </mos> (unterminated example)"
    if "</keepAlive>" in body and "<keepAlive" not in body:
        return "spec typo: closing </keepAlive> with no opening tag"
    if re.search(r'="[^"]*[<>][^"]*"', body):
        return "raw < or > inside an XPath attribute value (searchField)"
    if re.search(r"</obj(?:Proxy)?Path>\"?>\\?", body) or '</objPath>">' in body:
        return ("calibre export mangled an embedded file-path hyperlink; "
                "a clean equivalent example exists")
    if re.search(r".<\?xml", body, re.DOTALL):
        return "embeds a foreign <?xml?> declaration inside <mosPayload>"
    return "non-standalone spec fragment (mixed-content or opaque mosPayload)"


def payload_type(body: str) -> str:
    # first opened element under <mos> that is not an envelope header
    for m in re.finditer(r"<([A-Za-z][A-Za-z0-9_]*)", body):
        tag = m.group(1)
        if tag == "mos" or tag in ENVELOPE:
            continue
        return tag
    # fall back to a close tag (handles the keepAlive open-tag typo in the spec)
    for m in re.finditer(r"</([A-Za-z][A-Za-z0-9_]*)>", body):
        tag = m.group(1)
        if tag == "mos" or tag in ENVELOPE:
            continue
        return tag
    return "mos"


def main() -> int:
    parser = TextExtractor()
    parser.feed(SPEC.read_text(encoding="utf-8", errors="replace"))
    doc = normalize(parser.text())

    schema_dir = OUT / "schema"
    ex_dir = OUT / "spec-examples"
    schema_dir.mkdir(parents=True, exist_ok=True)
    ex_dir.mkdir(parents=True, exist_ok=True)

    # ---- XSD ------------------------------------------------------------
    xsd_m = re.search(r"<xsd:schema\b.*?</xsd:schema>", doc, re.DOTALL)
    xsd_ok = bool(xsd_m)
    if xsd_m:
        (schema_dir / "mos4.xsd").write_text(
            collapse_blank(xsd_m.group(0)), encoding="utf-8")

    # ---- DTD (chapter 8, before the XSD) --------------------------------
    # Use the *last* "8. MOS 4.0 DTD" heading before the XSD (the first one is
    # the Table-of-Contents link, whose region would wrongly swallow every
    # inline <!ELEMENT> structural outline in chapters 3-5).
    xsd_start = xsd_m.start() if xsd_m else len(doc)
    h8s = [m for m in re.finditer(r"(?m)^\s*8\.\s*MOS 4\.0 DTD", doc)
           if m.start() < xsd_start]
    region = doc[h8s[-1].end():xsd_start] if h8s else doc[:xsd_start]
    decls = re.findall(r"<!(?:ELEMENT|ATTLIST|ENTITY)\b.*?>", region, re.DOTALL)
    if decls:
        body = "\n".join(re.sub(r"\s+", " ", d).strip() for d in decls)
        applied = []
        for pat, repl, note in DTD_ERRATA:
            body, n = pat.subn(repl, body)
            if n:
                applied.append(note)
        errata = "".join(f"     - {a}\n" for a in applied) or "     (none)\n"
        header = ("<!-- MOS Protocol 4.0 DTD\n"
                  "     Extracted from chapter 8 of the MOS v4.0.1 specification\n"
                  "     (verbatim source: reference/MOS Protocol 4.0 (Current).html).\n"
                  "     Regenerate with tools/extract_fixtures.py; do not edit by hand.\n"
                  "     Spec DTD errata applied to make it a loadable DTD:\n"
                  f"{errata}"
                  "-->\n")
        (schema_dir / "mos4.dtd").write_text(header + body + "\n",
                                             encoding="utf-8")

    # ---- Examples -------------------------------------------------------
    headings = heading_index(doc)
    used: dict[str, int] = {}
    manifest = []
    ok = bad = 0
    for start, body, terminated in find_examples(doc):
        body = collapse_blank(body).rstrip("\n")
        kind = payload_type(body)
        section = section_for(start, headings)
        base = re.sub(r"[^A-Za-z0-9]+", "-", kind).strip("-") or "mos"
        used[base] = used.get(base, 0) + 1
        stem = base if used[base] == 1 else f"{base}-{used[base]}"
        wf = terminated and is_wellformed(body)
        fname = f"{stem}.xml" if wf else f"{stem}.illustrative.xml"
        (ex_dir / fname).write_text(body + "\n", encoding="utf-8")
        entry = {"file": fname, "message": kind, "section": section,
                 "wellformed": wf}
        if not wf:
            entry["reason"] = illustrative_reason(body, terminated)
        manifest.append(entry)
        ok += wf
        bad += not wf

    manifest.sort(key=lambda e: (e["message"], e["file"]))
    (ex_dir / "manifest.json").write_text(
        json.dumps({"count": len(manifest), "wellformed": ok,
                    "illustrative": bad, "examples": manifest}, indent=2) + "\n",
        encoding="utf-8")

    print(f"XSD written           : {xsd_ok} ({doc.count('<xsd:element')} xsd:element in doc)")
    print(f"DTD declarations      : {len(decls)}")
    print(f"Examples              : {len(manifest)} (well-formed {ok}, illustrative {bad})")
    print(f"Distinct message types: {len(used)}")
    return 0 if (decls and xsd_ok) else 1


if __name__ == "__main__":
    raise SystemExit(main())
