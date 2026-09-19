#!/usr/bin/env python3
"""Check that the release assets kept in this repo all agree with each other.

The chain has one link nobody watches: "is the payload inside the NRO in my hand really the
one the lock describes?".  This tool turns that into an exit code, so it can run before a
commit or in CI.

Checks:
  1. the lock is still a release form (dirty=false, buildFlags=2 = semantic hook only);
  2. packaging/agent/<version>/ matches the lock: subsdk9/main.npdm hashes, the contents of
     acnh-agent.version, and manifest.json against the files sitting next to it;
  3. data/ (the bin2s build input) is byte-identical to the release record;
  4. with --nro: the NRO really contains the embedded manifest and the three payloads, and
     its build stamp matches the *current* source tree -- that last part is what catches
     "source changed but the NRO was not rebuilt", which a payload-only check cannot see.

Usage:
    python3 tools/verify-release.py                          # repo assets only
    python3 tools/verify-release.py --nro acnh-manager.nro   # also check the NRO
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_LOCK = REPO_ROOT / "packaging" / "agent-lock.json"
DEFAULT_DATA = REPO_ROOT / "data"
RELEASE_BUILD_FLAGS = 1 << 1  # SEMANTIC_HOOK

# Release-record file name -> build-input file name (bin2s needs the .bin suffix).
PAYLOAD_MAP = {
    "subsdk9": "subsdk9.bin",
    "main.npdm": "main_npdm.bin",
    "acnh-agent.version": "acnh_agent_version.bin",
}

# The NRO build stamp looks like "2026-09-17T05:17:20Z+68b71a2-dirty+src:445e4876"
# (see tools/build.sh): UTC time + this repo's commit + [-dirty] + 8 hex chars of the
# source/ tree hash.
STAMP_RE = re.compile(
    rb"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z\+[0-9a-f]+(?:-dirty)?\+src:([0-9a-f]{8})")


def sha256_of(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_tree_hash(repo: pathlib.Path) -> str:
    """Reproduce tools/build.sh's src:<8 hex> component: sort the "sha256  path" lines of
    every file under source/ by path, then hash the concatenation."""
    root = repo / "source"
    rels = sorted(p.relative_to(repo).as_posix()
                  for p in root.rglob("*") if p.is_file() and not p.is_symlink())
    lines = "".join(f"{sha256_of(repo / rel)}  {rel}\n" for rel in rels)
    return hashlib.sha256(lines.encode()).hexdigest()[:8]


class Report:
    def __init__(self) -> None:
        self.checks = 0
        self.failures: list[str] = []

    def check(self, ok: bool, what: str, detail: str = "") -> bool:
        self.checks += 1
        if ok:
            print(f"  ok   {what}")
        else:
            print(f"  FAIL {what}" + (f" — {detail}" if detail else ""))
            self.failures.append(what)
        return ok


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lock", type=pathlib.Path, default=DEFAULT_LOCK)
    parser.add_argument("--data", type=pathlib.Path, default=DEFAULT_DATA,
                        help="embedded build-input directory (default: data/)")
    parser.add_argument("--record", type=pathlib.Path, default=None,
                        help="release-record directory (default: packaging/agent/<version from the lock>)")
    parser.add_argument("--nro", type=pathlib.Path, default=None,
                        help="also check that this NRO really embeds the same bytes")
    args = parser.parse_args(argv)

    report = Report()

    if not args.lock.is_file():
        print(f"error: no release lock at {args.lock} (run tools/release.sh first)", file=sys.stderr)
        return 2
    lock = json.loads(args.lock.read_text())
    version = lock["agentVersion"]
    record_dir = args.record or (REPO_ROOT / "packaging" / "agent" / version)

    print(f"lock {args.lock.relative_to(REPO_ROOT)}: agent {version} / "
          f"commit {lock.get('commit')} / buildFlags {lock.get('buildFlags')}")

    print("\n[1] release form")
    report.check(lock.get("dirty") is False, "agent.dirty == false")
    report.check(lock.get("buildFlags") == RELEASE_BUILD_FLAGS,
                 f"agent.buildFlags == {RELEASE_BUILD_FLAGS} (semantic hook only)",
                 f"actual {lock.get('buildFlags')}")

    print(f"\n[2] release record packaging/agent/{version}/")
    if not report.check(record_dir.is_dir(), f"{record_dir.relative_to(REPO_ROOT)} exists"):
        return 1
    nso_sha = sha256_of(record_dir / "subsdk9")
    npdm_sha = sha256_of(record_dir / "main.npdm")
    report.check(nso_sha == lock["nsoSha256"], "subsdk9 hash matches the lock",
                 f"{nso_sha[:16]}… != {lock['nsoSha256'][:16]}…")
    report.check(npdm_sha == lock["npdmSha256"], "main.npdm hash matches the lock",
                 f"{npdm_sha[:16]}… != {lock['npdmSha256'][:16]}…")
    sidecar = json.loads((record_dir / "acnh-agent.version").read_text())
    report.check(sidecar.get("agentVersion") == version, "acnh-agent.version version matches the lock")
    report.check(sidecar.get("commit") == lock.get("commit"), "acnh-agent.version commit matches the lock")
    report.check(sidecar.get("dirty") is False and
                 sidecar.get("buildFlags") == RELEASE_BUILD_FLAGS,
                 "acnh-agent.version is a release form")
    manifest = json.loads((record_dir / "manifest.json").read_text())
    report.check(manifest["agent"]["version"] == version, "manifest agent version matches the lock")
    report.check(manifest["agent"]["commit"] == lock.get("commit"), "manifest commit matches the lock")
    entries = {f["name"]: f for game in manifest["games"] for f in game["files"]}
    for name, entry in entries.items():
        path = record_dir / name
        if not report.check(path.is_file(), f"{name} referenced by the manifest exists in the record"):
            continue
        report.check(path.stat().st_size == entry["size"], f"{name} size matches the manifest")
        report.check(sha256_of(path).upper() == entry["sha256"].upper(),
                     f"{name} hash matches the manifest")

    print(f"\n[3] embedded build input {args.data.relative_to(REPO_ROOT)}/")
    report.check((args.data / "manifest.bin").read_bytes() ==
                 (record_dir / "manifest.json").read_bytes(),
                 "manifest.bin equals the record's manifest.json")
    # The update check fetches the repo root copy over GitLab's raw endpoint, so a stale copy
    # there would quietly tell every player about the wrong release.
    latest = REPO_ROOT / "agent-manifest.json"
    report.check(latest.is_file() and latest.read_bytes() == (record_dir / "manifest.json").read_bytes(),
                 "agent-manifest.json (read by the in-app update check) equals the record")
    for name, data_name in PAYLOAD_MAP.items():
        path = args.data / data_name
        if not report.check(path.is_file(), f"{data_name} exists"):
            continue
        report.check(path.read_bytes() == (record_dir / name).read_bytes(),
                     f"{data_name} is byte-identical to the record's {name}")

    if args.nro is not None:
        print(f"\n[4] NRO embedded content {args.nro}")
        if not report.check(args.nro.is_file(), "the NRO exists"):
            return 1
        blob = args.nro.read_bytes()
        report.check((record_dir / "manifest.json").read_bytes() in blob,
                     "the NRO contains the embedded manifest")
        for name in PAYLOAD_MAP:
            payload = (record_dir / name).read_bytes()
            report.check(payload in blob, f"the NRO contains {name}'s raw bytes ({len(payload)} B)")

        expected = source_tree_hash(REPO_ROOT)
        stamps = sorted({m.group(0).decode() for m in STAMP_RE.finditer(blob)})
        found = sorted({m.group(1).decode() for m in STAMP_RE.finditer(blob)})
        print(f"  NRO build stamp: {stamps or '(none)'}")
        print(f"  current source tree: src:{expected}")
        report.check(expected in found,
                     "the NRO was built from the current source tree (src: stamp matches)",
                     f"NRO={found or 'none'} current={expected} -- source changed but the NRO was not rebuilt")
        if any("-dirty" in stamp for stamp in stamps):
            print("  note the NRO stamp says -dirty: the repo was dirty at build time (not a release)")

    print()
    if report.failures:
        print(f"{report.checks} checks, {len(report.failures)} failed:"
              + ", ".join(report.failures))
        return 1
    print(f"all {report.checks} checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
