#!/usr/bin/env python3
"""Build the Homebrew App Store submission files plus a local test repository.

Outputs (written to `build/scratch/store/` by default):
    packages/<name>/pkgbuild.json   metadata for the official data repo (switch-hbas-repo)
    packages/<name>/icon.png        store icon (copied from assets/icon.png)
    packages/<name>/screen.png      banner (only when assets/screen.png exists)
    zips/<name>.zip                 NRO + info.json + manifest.install
    repo.json                       local test repository (official CDN layout; usable as a custom shop source in Sphaira)

Usage:
    python3 tools/make-store-package.py            # uses the already-built acnh-manager.nro
    python3 tools/make-store-package.py --nro build/acnh-manager.nro --out /tmp/store
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import zipfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_NRO = REPO_ROOT / "acnh-manager.nro"
DEFAULT_OUT = REPO_ROOT / "build" / "scratch" / "store"
MANIFEST_INSTALL = "manifest.install"


def md5_of(path: pathlib.Path) -> str:
    digest = hashlib.md5()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sha256_of(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_zip(root: pathlib.Path, nro: pathlib.Path, name: str, version: str,
              info: dict) -> pathlib.Path:
    zips = root / "zips"
    zips.mkdir(parents=True, exist_ok=True)
    zip_path = zips / f"{name}.zip"
    nro_remote = f"switch/ACNH-Manager/{nro.name}"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(info["install_path"], nro.read_bytes())
        # Sphaira/hb-appstore only understands lines starting with E/U/G; U means overwrite.
        archive.writestr(MANIFEST_INSTALL, f"U {info['install_path']}\nG info.json\nG manifest.install\n")
        archive.writestr(info["info_path"], json.dumps(
            {"name": name, "title": info["title"], "author": info["author"],
             "version": version, "description": info["description"]}, indent=2) + "\n")
    return zip_path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--nro", type=pathlib.Path, default=DEFAULT_NRO)
    parser.add_argument("--out", type=pathlib.Path, default=DEFAULT_OUT)
    parser.add_argument("--listing", type=pathlib.Path,
                        default=REPO_ROOT / "packaging" / "listing.json")
    parser.add_argument("--version", default=None, help="defaults to APP_VERSION from the Makefile")
    args = parser.parse_args(argv)

    if not args.nro.is_file():
        print(f"error: {args.nro} not found (build first)")
        return 1
    listing = json.loads(args.listing.read_text())
    version = args.version
    if version is None:
        for line in (REPO_ROOT / "Makefile").read_text().splitlines():
            if line.startswith("APP_VERSION"):
                version = line.split(":=")[1].strip()
                break
    version = version or "0.0.0"

    icon = REPO_ROOT / "assets" / "icon.png"
    if not icon.is_file():
        print(f"error: {icon} not found (run tools/make-icons.py)")
        return 1
    banner = REPO_ROOT / "assets" / "screen.png"

    name = listing["name"]
    root = args.out
    package_dir = root / "packages" / name
    package_dir.mkdir(parents=True, exist_ok=True)

    info = {
        "install_path": f"switch/ACNH-Manager/{args.nro.name}",
        "info_path": "info.json",
        "title": listing["title"],
        "author": listing["author"],
        "description": listing["summary"]["en"],
    }
    zip_path = build_zip(root, args.nro, name, version, info)

    icon_copy = package_dir / "icon.png"
    icon_copy.write_bytes(icon.read_bytes())
    if banner.is_file():
        (package_dir / "screen.png").write_bytes(banner.read_bytes())

    # pkgbuild.json for the official data repo: the update asset points at our release download
    # (the project is hosted on GitLab, tag first; the permalink is the release's asset link).
    release_url = (f"https://gitlab.com/acnh-game/acnh-manager/-/releases/"
                   f"v{version}/downloads/{args.nro.name}")
    pkgbuild = {
        "package": name,
        "info": {
            "title": listing["title"],
            "author": listing["author"],
            "category": listing["category"],
            "version": version,
            "url": listing["url"],
            "license": listing["license"],
            "description": listing["summary"]["en"],
            "details": listing["details"]["en"],
        },
        "changelog": listing["changelog"]["en"],
        "assets": [
            {"url": release_url, "dest": f"/{info['install_path']}", "type": "update"},
            {"type": "icon", "url": "icon.png"},
        ],
    }
    if banner.is_file():
        pkgbuild["assets"].append({"type": "banner", "url": "screen.png"})
    (package_dir / "pkgbuild.json").write_text(json.dumps(pkgbuild, indent=2) + "\n")

    # Local test repository: same layout as the official CDN, usable as a custom shop source.
    repo = {
        "packages": [
            {
                "name": name,
                "title": listing["title"],
                "author": listing["author"],
                "category": listing["category"],
                "version": version,
                "description": listing["summary"]["en"],
                "details": listing["details"]["en"],
                "changelog": listing["changelog"]["en"],
                "license": listing["license"],
                "url": listing["url"],
                "binary": f"/{info['install_path']}",
                "updated": "01/01/2026",
                "app_dls": 0,
                "screens": 0,
                "extracted": args.nro.stat().st_size // 1024,
                "filesize": zip_path.stat().st_size // 1024,
                "md5": md5_of(zip_path),
                "sha256": sha256_of(zip_path),
            }
        ]
    }
    (root / "repo.json").write_text(json.dumps(repo, indent=2) + "\n")

    print(f"store package for {listing['title']} {version} -> {root}")
    print(f"  zip:        {(zip_path.relative_to(root))} ({zip_path.stat().st_size} B)")
    print(f"  pkgbuild:   {(package_dir / 'pkgbuild.json').relative_to(root)}")
    print(f"  repo.json:  {(root / 'repo.json').relative_to(root)}")
    print(f"  install:    {info['install_path']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
