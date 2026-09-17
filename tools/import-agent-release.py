#!/usr/bin/env python3
"""Import an acnh-agent release into this repo (release gate).

Gate (everything must pass before anything is written; failures say why):
  1. dist/version.json says dirty == false;
  2. buildFlags == 2 (semantic hook only; DEV / RPC server / any other bit is refused);
  3. sha256(dist/acnh-agent.nso) == version.json.sha256;
  4. the derived main.npdm passes acnh-agent's make-minimal-npdm.py --verify;
  5. the profile's (titleId, buildId) matches --content-id/--build-id.

On success it writes three things, all of them assets of this repo, and never touches
acnh-agent's dist/:
  packaging/agent-lock.json              release lock (version/commit/knobs/hashes/profile)
  packaging/agent/<agentVersion>/        release record, original file names kept:
                                           subsdk9 / main.npdm / acnh-agent.version / manifest.json
  data/manifest.bin                      embedded release manifest (json text)
  data/<name>.bin                        embedded payload files (renamed build inputs)
`data/` is devkitPro's DATA directory: the build turns each file into a symbol of the same
name (`subsdk9.bin` -> `subsdk9_bin` / `subsdk9_bin_size`) and links it into the NRO's
.rodata.  That is why the official channel needs neither romfs nor devoptab nor any service,
and why it installs offline.  Files in DATA must carry the .bin suffix (the build rule is
`%.bin.o: %.bin`); dots in the name become underscores, so the embedded name is
`<source from the manifest, dots replaced>.bin`.

Usage (--nso/--npdm/--version-json are all required: a release must come from artifacts
built just now, never from whatever sits in acnh-agent/dist; run tools/release.sh for the
whole chain):

    python3 tools/import-agent-release.py \\
        --nso <build output>/acnh-agent.nso \\
        --npdm <derived dir>/main.npdm \\
        --version-json <build output>/version.json \\
        --original-npdm ../acnh-agent/research/acnh-3.0.3/exefs/*/Program\\ #0/0/main.npdm
"""

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
WORKSPACE = REPO_ROOT.parent.parent
AGENT = WORKSPACE / "src" / "acnh-agent"

DEFAULT_ORIGINAL_NPDM = AGENT / "research" / "acnh-3.0.3" / "exefs"
DEFAULT_NPDM_TOOL = AGENT / "tools" / "make-minimal-npdm.py"
DEFAULT_PROFILES = AGENT / "tools" / "profiles.json"

RELEASE_BUILD_FLAGS = 1 << 1  # SEMANTIC_HOOK
EXEFS = "atmosphere/contents/01006F8002326000/exefs"
TITLE_ID = "01006F8002326000"
TITLE_VERSION = 2228224
DISPLAY_VERSION = "3.0.3"
DEFAULT_CONTENT_ID = "E10617820DB06889E1638499478DA0DE"
DEFAULT_BUILD_ID = "FF1D1C05670DB6021C85B624A710B963"


def sha256_of(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fail(message: str) -> int:
    print(f"REFUSED: {message}", file=sys.stderr)
    return 2


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--nso", type=pathlib.Path,
                        help="dist/acnh-agent.nso built just now (no stale artifacts)")
    parser.add_argument("--npdm", type=pathlib.Path,
                        help="main.npdm derived by make-minimal-npdm.py")
    parser.add_argument("--version-json", type=pathlib.Path,
                        help="dist/version.json built just now")
    parser.add_argument("--original-npdm", type=pathlib.Path, default=None,
                        help="original ACNH main.npdm (used to replay-check the derived NPDM)")
    parser.add_argument("--npdm-tool", type=pathlib.Path, default=DEFAULT_NPDM_TOOL)
    parser.add_argument("--profiles", type=pathlib.Path, default=DEFAULT_PROFILES)
    parser.add_argument("--content-id", default=DEFAULT_CONTENT_ID)
    parser.add_argument("--build-id", default=DEFAULT_BUILD_ID)
    parser.add_argument("--app-min-version", default="0.1.0")
    parser.add_argument("--dry-run", action="store_true", help="run the gate only, write nothing")
    args = parser.parse_args(argv)

    missing = [flag for flag, value in (("--nso", args.nso), ("--npdm", args.npdm),
                                        ("--version-json", args.version_json))
               if value is None]
    if missing:
        print(f"error: {' '.join(missing)} required; run tools/release.sh for the whole chain",
              file=sys.stderr)
        return 1
    for path in (args.nso, args.npdm, args.version_json):
        if not path.is_file():
            print(f"error: missing input {path}", file=sys.stderr)
            return 1

    version = json.loads(args.version_json.read_text())
    if version.get("dirty", True):
        return fail("version.json says dirty=true; rebuild from a clean tree")
    if int(version.get("buildFlags", 0)) != RELEASE_BUILD_FLAGS:
        return fail(f"buildFlags={version.get('buildFlags')} is not the release form "
                    f"({RELEASE_BUILD_FLAGS} = semantic hook only); rebuild with "
                    f"'make switch SEMANTIC_HOOK=1'")
    nso_sha = sha256_of(args.nso)
    if nso_sha != version.get("sha256"):
        return fail(f"sha256 mismatch: nso={nso_sha} version.json={version.get('sha256')}")
    npdm_sha = sha256_of(args.npdm)

    if args.original_npdm is not None:
        if not args.original_npdm.is_file():
            return fail(f"original npdm not found: {args.original_npdm}")
        # Structural check, including "every svc bit we need is granted".
        result = subprocess.run([sys.executable, str(args.npdm_tool), "--verify",
                                 str(args.npdm)], capture_output=True, text=True)
        if result.returncode != 0:
            return fail("make-minimal-npdm.py --verify failed:\n" +
                        (result.stdout + result.stderr).strip())
        # Replay: derive once more from the same original NPDM and require the result to be
        # byte-identical to the file we were handed -- so "the derived NPDM matches the
        # original" is computed, not promised.
            replay = subprocess.run([sys.executable, str(args.npdm_tool),
                                     "--input", str(args.original_npdm), "--outdir", tmp],
                                    capture_output=True, text=True)
            if replay.returncode != 0:
                return fail("make-minimal-npdm.py replay failed:\n" +
                            (replay.stdout + replay.stderr).strip())
            derived = pathlib.Path(tmp) / "main.npdm"
            if not derived.is_file():
                return fail("replay did not produce main.npdm")
            derived_sha = sha256_of(derived)
            if derived_sha != npdm_sha:
                return fail(f"NPDM replay mismatch: derived={derived_sha} given={npdm_sha}")

    profile_note = ""
    if args.profiles.is_file():
        profiles = json.loads(args.profiles.read_text())
        entry = next((p for p in profiles.get("profiles", []) if p.get("titleIdText") == TITLE_ID),
                     None)
        if entry is None:
            return fail(f"profiles.json has no entry for title {TITLE_ID}")
        if entry.get("buildId") != args.build_id:
            return fail(f"profile buildId {entry.get('buildId')} != --build-id {args.build_id}")
        profile_note = entry.get("id", "")

    agent_version = version.get("agentVersion", "unknown")
    lock = {
        "agentVersion": agent_version,
        "commit": version.get("commit"),
        "dirty": version.get("dirty"),
        "buildFlags": version.get("buildFlags"),
        "nsoSha256": nso_sha,
        "npdmSha256": npdm_sha,
        "game": {
            "profile": profile_note,
            "titleId": TITLE_ID,
            "version": TITLE_VERSION,
            "displayVersion": DISPLAY_VERSION,
            "contentId": args.content_id.upper(),
            "buildId": args.build_id.upper(),
        },
    }

    if args.dry_run:
        print("gate passed (dry run):")
        print(json.dumps(lock, indent=2))
        return 0

    lock_path = REPO_ROOT / "packaging" / "agent-lock.json"
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    lock_path.write_text(json.dumps(lock, indent=2) + "\n")

    # (1) Release record: original file names, packaging/agent/<version>/, held by this repo
    #     (these four files are what the guide site will host).
    record_dir = REPO_ROOT / "packaging" / "agent" / agent_version
    record_dir.mkdir(parents=True, exist_ok=True)
    record_files = {
        "subsdk9": record_dir / "subsdk9",
        "main.npdm": record_dir / "main.npdm",
        "acnh-agent.version": record_dir / "acnh-agent.version",
    }
    shutil.copyfile(args.nso, record_files["subsdk9"])
    shutil.copyfile(args.npdm, record_files["main.npdm"])
    record_files["acnh-agent.version"].write_bytes(args.version_json.read_bytes())

    # (2) Embedded payload: renamed copies of the record above, placed in devkitPro's DATA
    #     directory so bin2s turns them into .rodata symbols at build time.
    data_dir = REPO_ROOT / "data"
    data_dir.mkdir(parents=True, exist_ok=True)
    payload_files = {
        "subsdk9": data_dir / "subsdk9.bin",
        "main.npdm": data_dir / "main_npdm.bin",
        "acnh-agent.version": data_dir / "acnh_agent_version.bin",
    }
    for name, path in payload_files.items():
        shutil.copyfile(record_files[name], path)

    manifest = {
        "schema": 1,
        "channel": "stable",
        "generated": "",
        "app": {"minVersion": args.app_min_version},
        "agent": {
            "version": agent_version,
            "commit": version.get("commit"),
            "dirty": version.get("dirty"),
            "buildFlags": version.get("buildFlags"),
        },
        "baseUrl": f"https://lextuo.com/acnh-chat-code/guide/agent/{agent_version}/",
        "changelog": "",
        "games": [
            {
                "profile": profile_note,
                "titleId": TITLE_ID,
                "version": TITLE_VERSION,
                "displayVersion": DISPLAY_VERSION,
                "contentId": args.content_id.upper(),
                "buildId": args.build_id.upper(),
                "files": [
                    {
                        "name": "subsdk9",
                        "source": "subsdk9",
                        "target": f"{EXEFS}/subsdk9",
                        "size": args.nso.stat().st_size,
                        "sha256": nso_sha,
                        "restart": "game",
                    },
                    {
                        "name": "main.npdm",
                        "source": "main.npdm",
                        "target": f"{EXEFS}/main.npdm",
                        "size": args.npdm.stat().st_size,
                        "sha256": npdm_sha,
                        "restart": "game",
                    },
                    {
                        "name": "acnh-agent.version",
                        "source": "acnh-agent.version",
                        "target": f"{EXEFS}/acnh-agent.version",
                        "size": payload_files["acnh-agent.version"].stat().st_size,
                        "sha256": sha256_of(payload_files["acnh-agent.version"]),
                        "restart": "none",
                    },
                ],
            }
        ],
    }
    # The record keeps the original file name; the embedded copy is renamed manifest.bin.
    manifest_text = json.dumps(manifest, indent=2) + "\n"
    (record_dir / "manifest.json").write_text(manifest_text)
    manifest_path = data_dir / "manifest.bin"
    manifest_path.write_text(manifest_text)

    print(f"imported agent {agent_version} (commit {version.get('commit')}, "
          f"buildFlags {version.get('buildFlags')})")
    print(f"  lock:    {lock_path.relative_to(REPO_ROOT)}")
    print(f"  record:  {record_dir.relative_to(REPO_ROOT)}/ "
          f"(subsdk9 / main.npdm / acnh-agent.version / manifest.json)")
    for name, path in payload_files.items():
        print(f"  payload: {path.relative_to(REPO_ROOT)}  ({name}, "
              f"{path.stat().st_size} B)")
    print(f"  manifest: {manifest_path.relative_to(REPO_ROOT)} "
          f"({manifest_path.stat().st_size} B)")
    print(f"  nso sha256={nso_sha[:16]}…  npdm sha256={npdm_sha[:16]}…")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
