#!/usr/bin/env python3
"""Extract the MOS 3.x web-service WSDL from the 3.8.4 spec HTML.

Source of truth is the official spec (reference/'MOS Protocol 3.8.4
(Current).html'). The document is not vendored; if it is missing it is
downloaded on demand by tools/fetch_spec.py. mosprotocol.com serves a broken
TLS chain, so that fetch disables certificate verification and pins the
document by SHA-256 instead (same rule as the 4.0 fixtures). The full WSDL is
embedded in the spec's chapter 8 — note the
quirk of record (reference/m6-plan.md §2.1): the 3.8.4 document ships a WSDL
whose own comment says "MOS.WSDL version 3.8.2 September 22, 2006"; 3.8.2
*is* the current bindings artifact for the 3.8.4 compatibility target.

Produces, under the output directory:
  wsdl/MOSWebService-3.8.2.wsdl   the WSDL, verbatim modulo HTML markup
  wsdl/manifest.json              pinned bindings summary (operations,
                                  namespace, soapAction style) the ws3
                                  transport tests assert against

Fidelity rules match extract_fixtures.py: only HTML un-escaping, NBSP→space,
and markup stripping are applied; nothing is invented or corrected. The
extraction fails (non-zero exit) if the result is not well-formed XML or the
pinned surface (24 operations, SOAP 1.1 document/literal) is not found —
that gate is what "3.8.4 bindings pinned" (plan §13 M6) means in CI.
"""
from __future__ import annotations

import html
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

SPEC = Path(sys.argv[1])
OUT = Path(sys.argv[2])

if not SPEC.exists():
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import fetch_spec

    fetch_spec.ensure_path(SPEC)

WSDL_NS = "http://schemas.xmlsoap.org/wsdl/"
SOAP_NS = "http://schemas.xmlsoap.org/wsdl/soap/"
XSD_NS = "http://www.w3.org/2001/XMLSchema"
TARGET_NS = "http://mosprotocol.com/webservices/"


def extract_wsdl_text(doc: str) -> str:
    """The raw HTML slice from the WSDL's leading comment to its close tag,
    stripped of markup and un-escaped."""
    anchor = doc.find("MOS.WSDL version")
    if anchor < 0:
        sys.exit("error: 'MOS.WSDL version' comment not found in the spec HTML")
    start = doc.rfind("&lt;!--", 0, anchor)
    end = doc.find("&lt;/wsdl:definitions&gt;", anchor)
    if start < 0 or end < 0:
        sys.exit("error: WSDL boundaries not found in the spec HTML")
    raw = doc[start : end + len("&lt;/wsdl:definitions&gt;")]
    text = re.sub(r"<[^>]+>", "", raw)
    text = html.unescape(text)
    return text.replace(" ", " ")


def pinned_surface(root: ET.Element) -> dict:
    """Verify and summarize the bindings the M6 plan pins (§2.1)."""
    port_type = root.find(f"{{{WSDL_NS}}}portType")
    binding = root.find(f"{{{WSDL_NS}}}binding")
    service = root.find(f"{{{WSDL_NS}}}service")
    if port_type is None or binding is None or service is None:
        sys.exit("error: WSDL is missing portType/binding/service")

    operations = [op.get("name") for op in port_type.findall(f"{{{WSDL_NS}}}operation")]
    if len(operations) != 24:
        sys.exit(f"error: expected the pinned 24 operations, found {len(operations)}")

    soap_binding = binding.find(f"{{{SOAP_NS}}}binding")
    if soap_binding is None or soap_binding.get("style") != "document":
        sys.exit("error: expected a SOAP 1.1 document-style binding")

    actions = {}
    for op in binding.findall(f"{{{WSDL_NS}}}operation"):
        soap_op = op.find(f"{{{SOAP_NS}}}operation")
        if soap_op is None or soap_op.get("style") != "document":
            sys.exit(f"error: operation {op.get('name')} is not document-style")
        actions[op.get("name")] = soap_op.get("soapAction")
        for direction in ("input", "output"):
            body = op.find(f"{{{WSDL_NS}}}{direction}/{{{SOAP_NS}}}body")
            if body is None or body.get("use") != "literal":
                sys.exit(f"error: operation {op.get('name')} {direction} is not literal")
    for name, action in actions.items():
        if action != TARGET_NS.rstrip("/") + "/" + name:
            sys.exit(f"error: unexpected soapAction for {name}: {action}")

    schema = root.find(f"{{{WSDL_NS}}}types/{{{XSD_NS}}}schema")
    if schema is None or schema.get("targetNamespace") != TARGET_NS:
        sys.exit("error: schema targetNamespace is not the pinned namespace")

    return {
        "source": SPEC.name,
        "wsdlVersionComment": "MOS.WSDL version 3.8.2 September 22, 2006",
        "namespace": TARGET_NS,
        "soap": "1.1 document/literal over http",
        "service": service.get("name"),
        "operations": sorted(operations),
    }


def main() -> None:
    doc = SPEC.read_text(encoding="utf-8", errors="replace")
    text = extract_wsdl_text(doc)

    declaration = re.search(r"<\?xml[^>]*\?>", text)
    if declaration is None:
        sys.exit("error: no XML declaration in the extracted WSDL")
    body = text.replace(declaration.group(0), "", 1).strip()
    try:
        root = ET.fromstring(body)
    except ET.ParseError as error:
        sys.exit(f"error: extracted WSDL is not well-formed XML: {error}")
    if root.tag != f"{{{WSDL_NS}}}definitions":
        sys.exit(f"error: unexpected root element {root.tag}")

    manifest = pinned_surface(root)

    out_dir = OUT / "wsdl"
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "MOSWebService-3.8.2.wsdl").write_text(text.strip() + "\n", encoding="utf-8")
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"wsdl: 1 file, {len(manifest['operations'])} operations pinned")


if __name__ == "__main__":
    main()
