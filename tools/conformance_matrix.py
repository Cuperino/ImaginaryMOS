# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
"""Machine-readable conformance matrix (plan §10; M5 exit gate).

Derives the message inventory from tools/codegen/annotations.json and
reports, for every MOS 4.0 message of Profiles 0-7 (legacy-removed messages
are M6's 2.8-mode concern and excluded here):

  * fixture   -- a spec-example exists (gated or illustrative): this is the
                 "codec message-type coverage at 100%" gate;
  * suites    -- the test sources that exercise the message by name (the
                 in-memory conformance layer; name-match heuristic);
  * ws40      -- the message's profile has a 4.0-WebSocket loopback suite
                 (Profile 6 defines no wire messages and is complete by
                 definition on the transport axis).

Any hole fails the check (exit 1), which is how CI enforces the M5 gate:
Profiles 0-7 complete on the 4.0 WS transport with no holes.

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
    transports_text = "".join(
        text for path, text in sources.items() if "transports" in path.parts
    )
    ws40_profiles = {
        n for n in PROFILES if f"profile{n}" in transports_text
    }
    ws40_profiles.add(6)  # no wire messages: complete by definition

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
    matrix = {
        "spec": "MOS Protocol 4.0",
        "profiles": {
            str(n): ("complete" if n in ws40_profiles else "hole") for n in PROFILES
        },
        "messages": rows,
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
        "profiles 0-7 complete, no holes",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
