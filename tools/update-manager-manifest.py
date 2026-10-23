#!/usr/bin/env python3
"""Add the ACNH-Manager self-update entry to the served release manifest.

The embedded agent manifest intentionally omits this object: the NRO cannot contain a hash of
itself.  The signed copy fetched from Gitee may carry it, and Update Center uses that one request
for both Game Agent and Manager updates.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "agent-manifest.json"
RAW_ROOT = "https://gitee.com/acnh-game/acnh-manager/raw/main/"
MAX_NRO_BYTES = 32 * 1024 * 1024


def sha256_of(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def app_version() -> str:
    for line in (REPO_ROOT / "Makefile").read_text().splitlines():
        if line.startswith("APP_VERSION") and ":=" in line:
            return line.split(":=", 1)[1].strip()
    raise SystemExit("error: APP_VERSION not found in Makefile")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--nro", type=pathlib.Path, default=None)
    parser.add_argument("--version", default=None)
    parser.add_argument("--url", default=None)
    args = parser.parse_args(argv)

    version = args.version or app_version()
    nro = args.nro or (REPO_ROOT / "packaging" / "nro" / f"acnh-manager-{version}.nro")
    url = args.url or (RAW_ROOT + f"packaging/nro/acnh-manager-{version}.nro")
    if not nro.is_file():
        raise SystemExit(f"error: NRO not found: {nro}")
    size = nro.stat().st_size
    if size <= 0 or size > MAX_NRO_BYTES:
        raise SystemExit(f"error: NRO size {size} is outside 1..{MAX_NRO_BYTES}")
    if not url.startswith("https://"):
        raise SystemExit("error: manager URL must use https")

    manifest = json.loads(args.manifest.read_text())
    manifest["manager"] = {
        "version": version,
        "url": url,
        "size": size,
        "sha256": sha256_of(nro),
        "changelog": "",
    }
    args.manifest.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"manager {version}: {nro} ({size} B)")
    print(f"manifest: {args.manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
