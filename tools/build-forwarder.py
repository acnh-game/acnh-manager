#!/usr/bin/env python3
"""Build the hbmenu loader from source and regenerate the embedded forwarder NSP.

This is the source-controlled replacement for checking in the two Sphaira hbl build
artifacts.  It verifies the pinned source record, compiles the loader in the same
devkitPro container used for the NRO, then hands the fresh artifacts to
`make-forwarder-nsp.py` for NCA/NSP construction.
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
LOADER_DIR = REPO_ROOT / "packaging" / "forwarder" / "hbl-3.0.0"
BUILD_DIR = REPO_ROOT / "build" / "forwarder-loader"
IMAGE = "devkitpro/devkita64:20260219"
DEFAULT_KEYS = REPO_ROOT / "../../SDcard/switch/prod.keys"
DEFAULT_OUTPUT = REPO_ROOT / "data" / "forwarder_nsp.bin"
DEFAULT_METADATA = REPO_ROOT / "data" / "forwarder_nsp_meta.bin"


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def make_value(name: str) -> str:
    for line in (REPO_ROOT / "Makefile").read_text().splitlines():
        if ":=" in line:
            key, value = line.split(":=", 1)
            if key.strip() == name:
                return value.strip()
    raise SystemExit(f"{name} is missing from Makefile")


def verify_source_record() -> None:
    record = json.loads((LOADER_DIR / "SOURCE.json").read_text())
    if record.get("license") != "MIT":
        raise SystemExit("the hbl source record does not declare the expected MIT license")
    if record.get("buildImage") != IMAGE:
        raise SystemExit("the hbl source record names a different build image")
    expected = {
        "sourceMainSha256": LOADER_DIR / "source" / "main.c",
        "sourceTrampolineSha256": LOADER_DIR / "source" / "trampoline.s",
        "npdmConfigSha256": LOADER_DIR / "hbl.json",
        "cmakeListsSha256": LOADER_DIR / "CMakeLists.txt",
    }
    for field, path in expected.items():
        if not path.is_file():
            raise SystemExit(f"missing loader source: {path}")
        if sha256(path) != record.get(field):
            raise SystemExit(f"{path} does not match {path.parent / 'SOURCE.json'}:{field}")


def build_loader() -> tuple[pathlib.Path, pathlib.Path, pathlib.Path]:
    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    command = [
        "docker",
        "run",
        "--rm",
        "--platform",
        "linux/amd64",
        "-v",
        f"{REPO_ROOT}:/work",
        "-w",
        "/work",
        IMAGE,
        "bash",
        "-lc",
        "source /opt/devkitpro/switchvars.sh && "
        "cmake -Wno-dev -S packaging/forwarder/hbl-3.0.0 "
        "-B build/forwarder-loader "
        "-DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake "
        "-DCMAKE_BUILD_TYPE=MinSizeRel && "
        "cmake --build build/forwarder-loader --target hbl_nso hbl_npdm -j2",
    ]
    subprocess.run(command, check=True)
    main = BUILD_DIR / "exefs" / "main"
    npdm = BUILD_DIR / "exefs" / "main.npdm"
    if not main.is_file() or not npdm.is_file():
        raise SystemExit("the loader build did not produce exefs/main and main.npdm")
    nacp = BUILD_DIR / "acnh-manager.nacp"
    subprocess.run(
        [
            "docker",
            "run",
            "--rm",
            "--platform",
            "linux/amd64",
            "-v",
            f"{REPO_ROOT}:/work",
            "-w",
            "/work",
            IMAGE,
            "nacptool",
            "--create",
            make_value("APP_TITLE"),
            make_value("APP_AUTHOR"),
            make_value("APP_VERSION"),
            "build/forwarder-loader/acnh-manager.nacp",
        ],
        check=True,
    )
    if not nacp.is_file():
        raise SystemExit("nacptool did not produce the forwarder NACP")
    return main, npdm, nacp


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keys", type=pathlib.Path, default=DEFAULT_KEYS)
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--metadata", type=pathlib.Path, default=DEFAULT_METADATA)
    args = parser.parse_args()

    verify_source_record()
    loader_main, loader_npdm, nacp = build_loader()
    subprocess.run(
        [
            sys.executable,
            str(REPO_ROOT / "tools" / "make-forwarder-nsp.py"),
            "--loader-main",
            str(loader_main),
            "--loader-npdm",
            str(loader_npdm),
            "--nacp",
            str(nacp),
            "--icon",
            str(REPO_ROOT / "assets" / "icon.jpg"),
            "--keys",
            str(args.keys),
            "--source-record",
            str(LOADER_DIR / "SOURCE.json"),
            "--output",
            str(args.output),
            "--metadata",
            str(args.metadata),
        ],
        check=True,
    )
    print(f"loader main sha256: {sha256(loader_main)}")
    print(f"loader npdm sha256: {sha256(loader_npdm)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
