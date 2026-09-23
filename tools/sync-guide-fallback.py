#!/usr/bin/env python3
"""Publish this release into the guide site's fallback directory.

The guide (https://lextuo.com/acnh-chat-code/guide/) is the address players reach from the
mini-app, and it has to keep working when the app store is not an option and when the release
host (Gitee) is unreachable from where the player sits.  So the guide carries its *own* copies of
the four files, laid out the way they go on the SD card -- the same trick the cheat text file
already uses:

    public/switch/ACNH-Manager/acnh-manager.nro
    public/atmosphere/contents/01006F8002326000/exefs/{subsdk9,main.npdm,acnh-agent.version}
    public/agent-files.json        version / sizes / sha256 / paths, read by the page at runtime

Every copy is checked against the release lock and the record's manifest before it is written, so
"what the guide serves" and "what the app installs" cannot drift apart silently.  The previous
release's NRO is deleted from the guide, which keeps that repository from growing one 2.7 MB file
per version.

Only files are written: this tool never runs git.  Review and commit the guide repository
yourself (it is a separate repository with its own rules), then deploy it.

Usage:
    python3 tools/sync-guide-fallback.py                  # ../acnh-chat-code-guide
    python3 tools/sync-guide-fallback.py --guide-dir <path> [--dry-run]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import sys
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_GUIDE = REPO_ROOT.parent / "acnh-chat-code-guide"
DEFAULT_LOCK = REPO_ROOT / "packaging" / "agent-lock.json"
DEFAULT_NRO_DIR = REPO_ROOT / "packaging" / "nro"
AGENT_FILES_JSON = "agent-files.json"


def sha256_of(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def app_version() -> str:
    for line in (REPO_ROOT / "Makefile").read_text().splitlines():
        if line.startswith("APP_VERSION"):
            return line.split(":=")[1].strip()
    raise SystemExit("error: APP_VERSION not found in the Makefile")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--guide-dir", type=pathlib.Path, default=DEFAULT_GUIDE,
                        help="the guide repository (default: %(default)s)")
    parser.add_argument("--lock", type=pathlib.Path, default=DEFAULT_LOCK)
    parser.add_argument("--dry-run", action="store_true", help="report, write nothing")
    args = parser.parse_args(argv)

    public = args.guide_dir / "public"
    if not public.is_dir():
        print(f"error: {public} is not a directory (guide repo checked out?)", file=sys.stderr)
        return 1
    lock = json.loads(args.lock.read_text())
    agent_version = lock["agentVersion"]
    record = REPO_ROOT / "packaging" / "agent" / agent_version
    if not record.is_dir():
        print(f"error: the release record {record} is missing (run tools/release.sh first)",
              file=sys.stderr)
        return 1
    manifest = json.loads((record / "manifest.json").read_text())
    game = manifest["games"][0]

    # What ships, and what each file must hash to before it is allowed into the guide.
    nro_version = app_version()
    nro_src = DEFAULT_NRO_DIR / f"acnh-manager-{nro_version}.nro"
    if not nro_src.is_file():
        print(f"error: {nro_src} is missing (run tools/release.sh first)", file=sys.stderr)
        return 1
    # The NRO is the one file in the plan that the release record has no hash for, so it is checked
    # against the record itself: everything the app would install has to be inside it.  That is the
    # same rule `tools/verify-release.py --nro` applies, and it catches the realistic mix-up -- a
    # stale or hand-replaced NRO (easy to leave behind with --skip-nro) sitting at this path.
    nro_blob = nro_src.read_bytes()
    for name in ("manifest.json", "subsdk9", "main.npdm", "acnh-agent.version"):
        payload = (record / name).read_bytes()
        if payload not in nro_blob:
            print(f"error: {nro_src.name} does not contain the record's {name} "
                  f"({len(payload)} B) -- stale or wrong build?", file=sys.stderr)
            return 1
    payload_by_name = {entry["name"]: entry for entry in game["files"]}
    plan: list[tuple[pathlib.Path, pathlib.Path, str | None]] = [
        # Same name as on the SD card on purpose: the player downloads it straight into
        # `switch/ACNH-Manager/` with no renaming.  The version lives in agent-files.json (and on
        # the page) instead of in the file name.
        (nro_src, public / "switch" / "ACNH-Manager" / "acnh-manager.nro", None),
    ]
    for name in ("subsdk9", "main.npdm", "acnh-agent.version"):
        src = record / name
        if not src.is_file():
            print(f"error: {src} is missing from the release record", file=sys.stderr)
            return 1
        expected = payload_by_name.get(name, {}).get("sha256", "").lower()
        if not expected:
            print(f"error: the record's manifest does not list {name}", file=sys.stderr)
            return 1
        plan.append((src, public / "atmosphere" / "contents" / game["titleId"] / "exefs" / name,
                     expected))

    files_json = {
        "schema": 1,
        "generated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "game": {
            "titleId": game["titleId"],
            "displayVersion": game["displayVersion"],
            "buildId": game["buildId"],
            "contentId": game["contentId"],
        },
        "agent": {
            "version": agent_version,
            "commit": lock.get("commit"),
            "buildFlags": lock.get("buildFlags"),
        },
        "app": {"version": nro_version},
        "files": [],
    }

    changed: list[str] = []
    unchanged: list[str] = []
    for src, dst, expected in plan:
        digest = sha256_of(src)
        if expected is not None and digest != expected:
            print(f"error: {src.name} hashes {digest[:16]}… but the record says {expected[:16]}…",
                  file=sys.stderr)
            return 1
        files_json["files"].append({
            "name": dst.name,
            "path": dst.relative_to(public).as_posix(),
            "size": src.stat().st_size,
            "sha256": digest,
        })
        if dst.is_file() and sha256_of(dst) == digest:
            unchanged.append(dst.relative_to(public).as_posix())
            continue
        changed.append(dst.relative_to(public).as_posix())
        if not args.dry_run:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)

    # The published copy always carries the canonical name, so anything versioned left behind by
    # an earlier run (or a hand-made copy) is removed: the guide serves one release at a time.
    stale = [path for path in (public / "switch" / "ACNH-Manager").glob("acnh-manager-*.nro")] \
        if (public / "switch" / "ACNH-Manager").is_dir() else []
    json_path = public / AGENT_FILES_JSON
    # `generated` is a wall-clock stamp, so a fresh one would make this file differ on every run and
    # leave the guide repository dirty after a run that changed nothing.  Keep the stamp already on
    # disk unless something else in the payload moved.
    if json_path.is_file():
        try:
            previous = json.loads(json_path.read_text())
        except json.JSONDecodeError:
            previous = None
        if previous is not None:
            probe = dict(files_json)
            probe["generated"] = previous.get("generated")
            if probe == previous:
                files_json = probe
    json_text = json.dumps(files_json, indent=2) + "\n"
    if json_path.is_file() and json_path.read_text() == json_text:
        unchanged.append(AGENT_FILES_JSON)
    else:
        changed.append(AGENT_FILES_JSON)
        if not args.dry_run:
            json_path.write_text(json_text)

    print(f"guide: {public}")
    print(f"agent {agent_version} / app {nro_version} / game {game['displayVersion']}")
    for path in changed:
        print(f"  write   {path}")
    for path in unchanged:
        print(f"  same    {path}")
    for path in stale:
        print(f"  remove  {path.relative_to(public).as_posix()} (older release)")
        if not args.dry_run:
            path.unlink()
    if args.dry_run:
        print("dry run: nothing was written")
    else:
        print("done -- review the guide repository, commit it there, then deploy")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
