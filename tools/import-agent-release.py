#!/usr/bin/env python3
"""把 acnh-agent 的发布产物导入本仓库(发布门控)。

门控(全部通过才落盘,任何一条不过就拒绝并说明原因):
  1. dist/version.json 的 dirty == false;
  2. buildFlags == 2(只含语义钩子位;DEV / RPC server / 其他实验位一律拒绝);
  3. sha256(dist/acnh-agent.nso) == version.json.sha256;
  4. 派生的 main.npdm 通过 acnh-agent 的 make-minimal-npdm.py --verify;
  5. profile 的 (titleId, buildId) 与 --content-id/--build-id 一致。

通过后写入:
  packaging/agent-lock.json              发布锁(版本/commit/开关/哈希/构建指纹)
  source/payload/<agentVersion>/...      内嵌 payload(随 NRO 编译时打进 romfs)
  source/payload/<agentVersion>/manifest.json  内嵌发布清单

用法:
    python3 tools/import-agent-release.py \
        --nso ../acnh-agent/dist/acnh-agent.nso \
        --npdm ../acnh-agent/dist/acnh-3.0.3-frame-hook-self/main.npdm \
        --version-json ../acnh-agent/dist/version.json \
        --original-npdm ../acnh-agent/research/acnh-3.0.3/exefs/*/Program\\ #0/0/main.npdm
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
WORKSPACE = REPO_ROOT.parent.parent
AGENT = WORKSPACE / "src" / "acnh-agent"

DEFAULT_NSO = AGENT / "dist" / "acnh-agent.nso"
DEFAULT_NPDM = AGENT / "dist" / "acnh-3.0.3-frame-hook-self" / "main.npdm"
DEFAULT_VERSION_JSON = AGENT / "dist" / "version.json"
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
    parser.add_argument("--nso", type=pathlib.Path, default=DEFAULT_NSO)
    parser.add_argument("--npdm", type=pathlib.Path, default=DEFAULT_NPDM)
    parser.add_argument("--version-json", type=pathlib.Path, default=DEFAULT_VERSION_JSON)
    parser.add_argument("--original-npdm", type=pathlib.Path, default=None,
                        help="原始 ACNH main.npdm(用于重放校验派生 NPDM)")
    parser.add_argument("--npdm-tool", type=pathlib.Path, default=DEFAULT_NPDM_TOOL)
    parser.add_argument("--profiles", type=pathlib.Path, default=DEFAULT_PROFILES)
    parser.add_argument("--content-id", default=DEFAULT_CONTENT_ID)
    parser.add_argument("--build-id", default=DEFAULT_BUILD_ID)
    parser.add_argument("--app-min-version", default="0.1.0")
    parser.add_argument("--dry-run", action="store_true", help="只跑门控,不落盘")
    args = parser.parse_args(argv)

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
        result = subprocess.run([sys.executable, str(args.npdm_tool), "--verify",
                                 str(args.npdm)], capture_output=True, text=True)
        if result.returncode != 0:
            return fail("make-minimal-npdm.py --verify failed:\n" +
                        (result.stdout + result.stderr).strip())

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

    payload_dir = REPO_ROOT / "source" / "payload" / agent_version
    payload_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.nso, payload_dir / "subsdk9")
    shutil.copyfile(args.npdm, payload_dir / "main.npdm")
    sidecar = payload_dir / "acnh-agent.version"
    sidecar.write_bytes(args.version_json.read_bytes())

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
                        "size": sidecar.stat().st_size,
                        "sha256": sha256_of(sidecar),
                        "restart": "none",
                    },
                ],
            }
        ],
    }
    (payload_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    print(f"imported agent {agent_version} (commit {version.get('commit')}, "
          f"buildFlags {version.get('buildFlags')})")
    print(f"  lock:    {lock_path.relative_to(REPO_ROOT)}")
    print(f"  payload: {payload_dir.relative_to(REPO_ROOT)}")
    print(f"  nso sha256={nso_sha[:16]}…  npdm sha256={npdm_sha[:16]}…")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
