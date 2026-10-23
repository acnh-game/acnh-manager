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
  5. the generated desktop-forwarder NSP matches its source/hash record and is inside the NRO;
  6. the vendored third-party sources (picohttpparser, qrcodegen) are still the exact upstream
     bytes `source/third_party/README.md` pins -- they run on input from the local network, so
     a quiet local edit is exactly the thing that must not reach a release;
  7. the manifest the update check serves is signed by the key this app trusts: a manifest
     committed without (re-)signing would be rejected by every shipped App, which is exactly
     the kind of silent release breakage this tool exists to catch.  Uses openssl when it is
     on PATH; without it the check is reported as skipped, never as passed.

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
import shutil
import subprocess
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


def show(path: pathlib.Path) -> str:
    """Path for messages: relative to the repository when it is inside it.

    A relative `--data`/`--record` (or one outside the repo) used to blow up in
    `relative_to()` before any check ran -- the tool crashed instead of reporting."""
    resolved = path if path.is_absolute() else (pathlib.Path.cwd() / path)
    try:
        return str(resolved.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(resolved)


def source_tree_hash(repo: pathlib.Path) -> str:
    """Reproduce tools/build.sh's src:<8 hex>: it covers source/, data/ and assets/, not just C++
    source, because those are all linked into the NRO."""
    rels = []
    for name in ("source", "data", "assets"):
        root = repo / name
        rels += [p.relative_to(repo).as_posix()
                 for p in root.rglob("*") if p.is_file() and not p.is_symlink()]
    rels.sort()
    lines = "".join(f"{sha256_of(repo / rel)}  {rel}\n" for rel in rels)
    return hashlib.sha256(lines.encode()).hexdigest()[:8]


def app_version() -> str:
    for line in (REPO_ROOT / "Makefile").read_text().splitlines():
        if line.startswith("APP_VERSION") and ":=" in line:
            return line.split(":=", 1)[1].strip()
    return ""


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


def check_manifest_signature(report: Report) -> None:
    """The manifest served to the app must carry a signature this app's key accepts: a manifest
    committed without (re-)signing is rejected by every shipped App, so "the release is live but
    nobody can see it" would otherwise only show up on a console."""
    print("\n[7] served manifest signature")
    manifest = REPO_ROOT / "agent-manifest.json"
    signature = manifest.with_name(manifest.name + ".sig")  # the name the app asks for
    pubkey = REPO_ROOT / "data" / "agent_pubkey.bin"
    if not report.check(pubkey.is_file(),
                        f"{show(pubkey)} (the key compiled into the app) exists",
                        "generate it once with tools/make-signing-key.py"):
        return
    if not report.check(signature.is_file(),
                        f"{signature.name} exists next to the manifest it signs",
                        "run tools/sign-manifest.py --key <private key>"):
        return
    if shutil.which("openssl") is None:
        print("  skip openssl is not on PATH, so the signature was not actually verified")
        return
    result = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(pubkey),
                             "-signature", str(signature), str(manifest)],
                            capture_output=True)
    detail = (result.stdout + result.stderr).decode("utf-8", "replace").strip()
    report.check(result.returncode == 0, "the signature verifies with the embedded public key",
                 detail)


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

    print(f"lock {show(args.lock)}: agent {version} / "
          f"commit {lock.get('commit')} / buildFlags {lock.get('buildFlags')}")

    print("\n[1] release form")
    report.check(lock.get("dirty") is False, "agent.dirty == false")
    report.check(lock.get("buildFlags") == RELEASE_BUILD_FLAGS,
                 f"agent.buildFlags == {RELEASE_BUILD_FLAGS} (semantic hook only)",
                 f"actual {lock.get('buildFlags')}")
    # The lock pins the key this release's manifest is signed with.  Every shipped app only
    # trusts the key it was built with, so a change here is an identity change and has to be an
    # explicit line in the release diff -- not something that slips through unnoticed.
    embedded_pubkey = args.data / "agent_pubkey.bin"
    if report.check(embedded_pubkey.is_file(),
                    f"{show(embedded_pubkey)} exists (the key the app embeds)",
                    "generate it once with tools/make-signing-key.py"):
        report.check(lock.get("signingPublicKeySha256") == sha256_of(embedded_pubkey).lower(),
                     "the lock records this build's signing public key",
                     f"lock {lock.get('signingPublicKeySha256')} != data {sha256_of(embedded_pubkey)}"
                     " -- a key change breaks every shipped app, so it must be deliberate")

    print(f"\n[2] release record packaging/agent/{version}/")
    if not report.check(record_dir.is_dir(), f"{show(record_dir)} exists"):
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

    print(f"\n[3] embedded build input {show(args.data)}/")
    report.check((args.data / "manifest.bin").read_bytes() ==
                 (record_dir / "manifest.json").read_bytes(),
                 "manifest.bin equals the record's manifest.json")
    # The update check fetches the repo root copy over Gitee's raw endpoint, so a stale copy
    # there would quietly tell every player about the wrong release.  It may carry one extra
    # signed `manager` object for self-update; every agent/game field must still equal the record.
    latest = REPO_ROOT / "agent-manifest.json"
    latest_json = None
    latest_error = ""
    if latest.is_file():
        try:
            latest_json = json.loads(latest.read_text())
        except (OSError, json.JSONDecodeError) as error:
            latest_error = str(error)
    if report.check(isinstance(latest_json, dict),
                    "agent-manifest.json (read by the in-app update check) is a JSON object",
                    latest_error):
        served_without_manager = dict(latest_json)
        manager_entry = served_without_manager.pop("manager", None)
        report.check(served_without_manager == manifest,
                     "served manifest agent/game fields equal the release record")
        if manager_entry is not None:
            version = manager_entry.get("version")
            url = manager_entry.get("url", "")
            marker = "/raw/main/"
            report.check(isinstance(version, str) and version == app_version(),
                         "served manager version matches APP_VERSION")
            report.check(isinstance(url, str) and url.startswith("https://") and marker in url,
                         "served manager URL is an HTTPS Gitee raw path")
            if marker in url:
                nro = REPO_ROOT / url.split(marker, 1)[1]
                inside_repo = False
                try:
                    nro.resolve().relative_to(REPO_ROOT.resolve())
                    inside_repo = True
                except ValueError:
                    pass
                if (report.check(inside_repo, "served manager path stays inside the repository") and
                        report.check(nro.is_file(),
                                     f"served manager NRO exists: {show(nro)}")):
                    report.check(manager_entry.get("size") == nro.stat().st_size,
                                 "served manager size matches the NRO")
                    report.check(str(manager_entry.get("sha256", "")).lower() == sha256_of(nro),
                                 "served manager SHA-256 matches the NRO")
        else:
            print("  skip no manager self-update object in the served manifest")
    for name, data_name in PAYLOAD_MAP.items():
        path = args.data / data_name
        if not report.check(path.is_file(), f"{data_name} exists"):
            continue
        report.check(path.read_bytes() == (record_dir / name).read_bytes(),
                     f"{data_name} is byte-identical to the record's {name}")

    print(f"\n[4] embedded desktop forwarder {show(args.data)}/")
    forwarder = args.data / "forwarder_nsp.bin"
    forwarder_meta = args.data / "forwarder_nsp_meta.bin"
    if report.check(forwarder.is_file(), "forwarder_nsp.bin exists"):
        if report.check(forwarder_meta.is_file(), "forwarder_nsp_meta.bin exists"):
            metadata = json.loads(forwarder_meta.read_text())
            report.check(forwarder.stat().st_size == metadata.get("size"),
                         "the forwarder NSP size matches its source record")
            report.check(sha256_of(forwarder) == metadata.get("nspSha256"),
                         "the forwarder NSP hash matches its source record")
            source_dir = REPO_ROOT / "packaging" / "forwarder" / "hbl-3.0.0"
            source_record = source_dir / "SOURCE.json"
            if report.check(source_record.is_file(), "the hbl source record exists"):
                source = json.loads(source_record.read_text())
                report.check(metadata.get("loaderSource") == source,
                             "the forwarder provenance matches the checked-in hbl source")
                report.check(not (source_dir / "main").exists() and
                             not (source_dir / "main.npdm").exists(),
                             "no compiled hbl loader binary is checked in")
                source_files = {
                    "sourceMainSha256": source_dir / "source" / "main.c",
                    "sourceTrampolineSha256": source_dir / "source" / "trampoline.s",
                    "npdmConfigSha256": source_dir / "hbl.json",
                    "cmakeListsSha256": source_dir / "CMakeLists.txt",
                }
                for field, path in source_files.items():
                    report.check(path.is_file() and sha256_of(path) == source.get(field),
                                 f"{path.relative_to(REPO_ROOT)} matches the hbl source record")

    if args.nro is not None:
        print(f"\n[5] NRO embedded content {args.nro}")
        if not report.check(args.nro.is_file(), "the NRO exists"):
            return 1
        blob = args.nro.read_bytes()
        report.check((record_dir / "manifest.json").read_bytes() in blob,
                     "the NRO contains the embedded manifest")
        for name in PAYLOAD_MAP:
            payload = (record_dir / name).read_bytes()
            report.check(payload in blob, f"the NRO contains {name}'s raw bytes ({len(payload)} B)")
        # The public key is a build input like the payloads, but no stamp covers data/: without
        # this check, regenerating the key, re-signing the manifest and forgetting to rebuild
        # the NRO passes every local check while every player's app rejects the new signature.
        pubkey_bytes = (args.data / "agent_pubkey.bin").read_bytes()
        report.check(pubkey_bytes in blob,
                     f"the NRO embeds the current public key ({len(pubkey_bytes)} B)")
        if forwarder.is_file():
            report.check(forwarder.read_bytes() in blob,
                         f"the NRO embeds the forwarder NSP ({forwarder.stat().st_size} B)")

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

    check_third_party(report)
    check_manifest_signature(report)
    print()
    if report.failures:
        print(f"{report.checks} checks, {len(report.failures)} failed:"
              + ", ".join(report.failures))
        return 1
    print(f"all {report.checks} checks passed.")
    return 0


THIRD_PARTY_PINS = (
    (
        "picohttpparser/picohttpparser.h",
        "1fc9074dd12418b2b91e55ef3a8279bccf8d3577b790dea6f2f908ab2a157f1c",
    ),
    (
        "picohttpparser/picohttpparser.c",
        "6480689711996bc9a219e26df27ac32ebbb7dce2f24a63a316d041b08f646199",
    ),
    ("qrcodegen/qrcodegen.h", "e82df4bff37d18b5863b9e7486fe6bda1b6cda8c3b9ecebfec473907265cb589"),
    ("qrcodegen/qrcodegen.c", "6a2b9cc65176f2345dde260c74b6d352627e8a0a6385d086ae0e9c5d0913c70c"),
)


def check_third_party(report: Report) -> None:
    """The vendored libraries may only be replaced by a deliberate re-pin (source/third_party/
    README.md explains the update procedure); this check is what makes that stick."""
    print("\n[6] vendored third-party sources")
    root = REPO_ROOT / "source" / "third_party"
    for relative, expected in THIRD_PARTY_PINS:
        path = root / relative
        if not report.check(path.is_file(), f"{relative} exists"):
            continue
        report.check(sha256_of(path) == expected, f"{relative} is the pinned upstream revision")

    return None


if __name__ == "__main__":
    raise SystemExit(main())
