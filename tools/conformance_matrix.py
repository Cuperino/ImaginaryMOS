# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
"""Machine-readable conformance matrix (plan §10; M5 exit gate + M6 axes).

Derives the message inventory from tools/codegen/annotations.json and
reports, for every MOS 4.0 message of Profiles 0-7:

  * fixture   -- a spec-example exists (gated or illustrative): this is the
                 "codec message-type coverage at 100%" gate;
  * suites    -- the test sources that exercise the message by name (the
                 in-memory conformance layer; name-match heuristic);
  * ws40      -- the message's profile has a 4.0-WebSocket loopback suite
                 (Profile 6 defines no wire messages and is complete by
                 definition on the transport axis).

M6 (legacy transports; reference/m6-plan.md) adds:

  * legacy-13 -- every removed-in-4.0 message is exercised by the 2.8-mode
                 wire battery (tests/transports/tcp28_transport_test.cpp)
                 as well as the codec normalization suite;
  * tcp28     -- the 2.8 TCP loopback suite re-runs every wire profile
                 (P0-P5, P7; P6 defines no messages);
  * ws3       -- the 3.8.2 WSDL fixture pins exactly the 24 operations,
                 every operation is a known message type, and the ws3
                 loopback suite re-runs every wire profile.

Any hole fails the check (exit 1), which is how CI enforces the M5+M6
gates: Profiles 0-7 complete on the 4.0 WS transport, the legacy thirteen
normalized in 2.8 mode, and the 3.8.4 bindings pinned, with no holes.

Usage: python3 tools/conformance_matrix.py [--output matrix.json]
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROFILES = range(0, 8)


def load_messages() -> dict[str, dict]:
    annotations = json.loads((ROOT / "tools/codegen/annotations.json").read_text())
    return {
        name: info
        for name, info in annotations["messages"].items()
        if info.get("profile") in PROFILES and not info.get("legacyRemoved")
    }


def load_legacy_messages() -> list[str]:
    annotations = json.loads((ROOT / "tools/codegen/annotations.json").read_text())
    return sorted(
        name for name, info in annotations["messages"].items() if info.get("legacyRemoved")
    )


def fixture_files(name: str) -> list[str]:
    pattern = re.compile(rf"^{re.escape(name)}(-\d+)?(\.illustrative)?\.xml$")
    return sorted(
        p.name for p in (ROOT / "fixtures/spec-examples").iterdir() if pattern.match(p.name)
    )


def test_sources() -> dict[Path, str]:
    sources = {}
    for path in sorted((ROOT / "tests").rglob("*.cpp")):
        sources[path] = path.read_text(encoding="utf-8", errors="replace").lower()
    return sources


def build_matrix() -> tuple[dict, list[str]]:
    messages = load_messages()
    sources = test_sources()
    ws40_text = "".join(
        text
        for path, text in sources.items()
        if "transports" in path.parts and "ws40" in path.name
    )
    ws40_profiles = {n for n in PROFILES if f"profile{n}" in ws40_text}
    ws40_profiles.add(6)  # no wire messages: complete by definition

    tcp28_text = "".join(
        text
        for path, text in sources.items()
        if "transports" in path.parts and "tcp28" in path.name
    )
    tcp28_profiles = {n for n in PROFILES if f"profile{n}" in tcp28_text}
    ws3_text = "".join(
        text
        for path, text in sources.items()
        if "transports" in path.parts and "ws3" in path.name
    )

    rows = []
    holes = []
    for name, info in sorted(messages.items(), key=lambda kv: (kv[1]["profile"], kv[0])):
        fixtures = fixture_files(name)
        suites = sorted(
            str(path.relative_to(ROOT))
            for path, text in sources.items()
            if name.lower() in text
        )
        ws40 = info["profile"] in ws40_profiles
        rows.append(
            {
                "message": name,
                "profile": info["profile"],
                "channel": info["channel"],
                "responses": info.get("responses", []),
                "fixtures": fixtures,
                "suites": suites,
                "ws40Transport": ws40,
            }
        )
        if not fixtures:
            holes.append(f"{name}: no spec-example fixture (message-type coverage)")
        if not suites:
            holes.append(f"{name}: not exercised by any test suite")
        if not ws40:
            holes.append(f"{name}: profile {info['profile']} has no 4.0-WS loopback suite")
    # --- M6: the legacy thirteen, normalized in 2.8 mode ------------------
    legacy_rows = []
    for name in load_legacy_messages():
        wire_battery = name.lower() in tcp28_text
        codec_suite = any(
            name.lower() in text for path, text in sources.items() if "codec" in path.parts
        )
        legacy_rows.append(
            {"message": name, "tcp28WireBattery": wire_battery, "codecSuite": codec_suite}
        )
        if not wire_battery:
            holes.append(f"{name}: missing from the 2.8-mode wire normalization battery")
        if not codec_suite:
            holes.append(f"{name}: missing from the codec normalization suite")

    # --- M6: legacy-transport re-runs of every wire profile ----------------
    # (P6 defines no wire messages; it is complete by definition, as on
    # the ws40 axis.)
    ws3_profiles = {n for n in PROFILES if f"profile{n}" in ws3_text}
    for required in (0, 1, 2, 3, 4, 5, 7):
        if required not in tcp28_profiles:
            holes.append(f"profile {required}: no 2.8-TCP loopback suite")
        if required not in ws3_profiles:
            holes.append(f"profile {required}: no 3.x-SOAP loopback suite")

    # --- M6: 3.8.4 bindings pinned -----------------------------------------
    wsdl_manifest_path = ROOT / "fixtures/wsdl/manifest.json"
    ws3_operations = []
    if not wsdl_manifest_path.exists():
        holes.append("fixtures/wsdl/manifest.json missing (run tools/extract_wsdl.py)")
    else:
        wsdl_manifest = json.loads(wsdl_manifest_path.read_text())
        ws3_operations = wsdl_manifest.get("operations", [])
        if len(ws3_operations) != 24:
            holes.append(f"WSDL manifest pins {len(ws3_operations)} operations, expected 24")
        known = set(load_messages())
        for operation in ws3_operations:
            if operation not in known:
                holes.append(f"WSDL operation {operation} is not a known 4.0 message")

    matrix = {
        "spec": "MOS Protocol 4.0 (+2.8.5 TCP, 3.8.4 WebService compat)",
        "profiles": {
            str(n): ("complete" if n in ws40_profiles else "hole") for n in PROFILES
        },
        "messages": rows,
        "legacyThirteen": legacy_rows,
        "transports": {
            "ws40Profiles": sorted(ws40_profiles),
            "tcp28Profiles": sorted(tcp28_profiles),
            "ws3Profiles": sorted(ws3_profiles),
            "ws3Operations": ws3_operations,
        },
    }
    return matrix, holes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, help="write the matrix JSON here")
    args = parser.parse_args()

    matrix, holes = build_matrix()
    payload = json.dumps(matrix, indent=2) + "\n"
    if args.output:
        args.output.write_text(payload)
    else:
        sys.stdout.write(payload)

    if holes:
        for hole in holes:
            print(f"HOLE: {hole}", file=sys.stderr)
        return 1
    print(
        f"conformance matrix: {len(matrix['messages'])} messages, "
        "every wire profile complete on ws40 + tcp28 + ws3, legacy-13 "
        "normalized in 2.8 mode, 3.8.4 bindings pinned, no holes",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
