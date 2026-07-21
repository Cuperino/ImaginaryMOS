#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Javier O. Cordero Pérez
# SPDX-License-Identifier: Apache-2.0
"""Fetch the official MOS spec HTML documents on demand.

The MOS 4.0, 3.8.4, and 2.8.5 specification HTML documents are the verbatim
source of truth for the generated fixtures (``extract_fixtures.py``) and the
3.x WSDL (``extract_wsdl.py``), but they are large (~6.7 MB) and are *not*
vendored in the repository. They are downloaded from mosprotocol.com on
demand and cached under ``reference/`` (gitignored).

mosprotocol.com serves a broken TLS chain (it omits the intermediate CA), so
certificate verification cannot succeed and is disabled for these downloads.
Integrity is guaranteed instead by a SHA-256 pin over the bytes as served: if
an upstream document ever changes, the fetch fails loudly rather than silently
feeding altered content into the fixture pipeline.

Usage:
    fetch_spec.py <4.0|3.8.4|2.8.5|all> [dest_dir]

Other modules import ``ensure`` / ``ensure_path`` to download a spec only when
its local copy is missing.
"""
from __future__ import annotations

import hashlib
import ssl
import sys
import urllib.request
from dataclasses import dataclass
from pathlib import Path

REFERENCE_DIR = Path(__file__).resolve().parent.parent / "reference"


@dataclass(frozen=True)
class SpecDoc:
    version: str
    filename: str
    url: str
    # SHA-256 of the document as served. TLS verification is disabled (broken
    # upstream chain), so this pin is what protects integrity.
    sha256: str


SPECS: tuple[SpecDoc, ...] = (
    SpecDoc(
        version="4.0",
        filename="MOS Protocol 4.0 (Current).html",
        url="https://mosprotocol.com/wp-content/MOS-Protocol-Documents/"
        "MOSProtocolVersion40/index.html",
        sha256="58e1e68a3cfd0f3d9ea0395e5b614ca1c0ddcec72988706fd25d7f143994f1d1",
    ),
    SpecDoc(
        version="3.8.4",
        filename="MOS Protocol 3.8.4 (Current).html",
        url="https://mosprotocol.com/wp-content/MOS-Protocol-Documents/"
        "MOS-Protocol-3.8.4-Current.htm",
        sha256="a0f9151db627ea62adfd8f2bd1cdd490e2f0466e25a50cd0b5900522ec157c07",
    ),
    SpecDoc(
        version="2.8.5",
        filename="MOS Protocol 2.8.5 (Final).html",
        url="https://mosprotocol.com/wp-content/MOS-Protocol-Documents/"
        "MOS_Protocol_Version_2.8.5_Final.htm",
        sha256="0b6c35049e7ad8715b34ae2504a50bc8f6ab70b9da2f12ec4ef6082bf21ecece",
    ),
)

_BY_VERSION = {spec.version: spec for spec in SPECS}
_BY_FILENAME = {spec.filename: spec for spec in SPECS}


def _download(spec: SpecDoc) -> bytes:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    req = urllib.request.Request(
        spec.url, headers={"User-Agent": "imaginarymos-fetch-spec"}
    )
    with urllib.request.urlopen(req, context=ctx, timeout=120) as resp:
        return resp.read()


def _verify(spec: SpecDoc, data: bytes) -> None:
    got = hashlib.sha256(data).hexdigest()
    if got != spec.sha256:
        raise SystemExit(
            f"error: {spec.filename} fetched from {spec.url}\n"
            f"       sha256 mismatch: expected {spec.sha256}, got {got}\n"
            f"       The upstream document changed. Confirm the new content is "
            f"legitimate, then update the pin in tools/fetch_spec.py."
        )


def ensure(spec: SpecDoc, dest_dir: Path = REFERENCE_DIR) -> Path:
    """Return the local path to *spec*, downloading and verifying it if absent."""
    dest = dest_dir / spec.filename
    if dest.exists():
        return dest
    print(
        f"fetch_spec: downloading {spec.filename} from {spec.url}", file=sys.stderr
    )
    data = _download(spec)
    _verify(spec, data)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    return dest


def ensure_path(path: Path) -> Path:
    """Ensure the spec identified by *path*'s filename exists at *path*.

    Used by the extraction scripts, which are invoked with an explicit
    ``reference/`` path: when the file is missing it is downloaded to exactly
    that path.
    """
    if path.exists():
        return path
    spec = _BY_FILENAME.get(path.name)
    if spec is None:
        raise SystemExit(
            f"error: {path} does not exist and is not a known MOS spec "
            f"document (known: {', '.join(sorted(_BY_FILENAME))})"
        )
    return ensure(spec, path.parent)


def main(argv: list[str]) -> int:
    args = argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print("usage: fetch_spec.py <4.0|3.8.4|2.8.5|all> [dest_dir]")
        return 2 if not args else 0
    which, *rest = args
    dest_dir = Path(rest[0]) if rest else REFERENCE_DIR
    if which == "all":
        targets: tuple[SpecDoc, ...] = SPECS
    elif which in _BY_VERSION:
        targets = (_BY_VERSION[which],)
    else:
        raise SystemExit(
            f"error: unknown spec '{which}' (want 4.0, 3.8.4, 2.8.5, or all)"
        )
    for spec in targets:
        print(ensure(spec, dest_dir))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
