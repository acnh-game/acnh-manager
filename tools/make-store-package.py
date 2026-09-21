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
import time
import zipfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_NRO = REPO_ROOT / "acnh-manager.nro"
DEFAULT_OUT = REPO_ROOT / "build" / "scratch" / "store"
MANIFEST_INSTALL = "manifest.install"
# Screenshots live in assets/screenshots/ and are listed in this order; the store page shows
# them in the order the pkgbuild names them (status -> what a tap does -> how it ends, with the
# uninstall confirmation last).
SCREENSHOT_DIR = REPO_ROOT / "assets" / "screenshots"
SCREENSHOT_ORDER = ("status.png", "install.png", "done.png", "uninstall.png")


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
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(info["install_path"], nro.read_bytes())
        # Byte-for-byte what spinarak (the store's own builder) writes:
        #   * `manifest.install` lines are `<op>: <path relative to the SD root>` -- the client
        #     parses the path as `line.substr(3)`, so a missing ": " silently turns
        #     "switch/..." into "witch/...";
        #   * only the installed files are listed (no G lines for info.json/manifest.install);
        #   * info.json carries the nine metadata fields spinarak copies out of the pkgbuild.
        archive.writestr(MANIFEST_INSTALL, f"U: {info['install_path']}\n")
        archive.writestr(info["info_path"], json.dumps(info["info_json"], indent=1))
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
    # The store catalog has exactly one text per field and the client shows it verbatim
    # (no language negotiation: Sphaira renders `details`/`changelog` as-is and only
    # translates its own labels), so the body text is written twice, Chinese first.
    # `description` stays English on purpose: the store also searches it, and "ACNH" /
    # "manager" are the keywords people type.
    details_text = listing["details"]["zh"] + "\n\n" + listing["details"]["en"]
    changelog_text = listing["changelog"]["zh"] + "\n\n" + listing["changelog"]["en"]
    root = args.out
    package_dir = root / "packages" / name
    package_dir.mkdir(parents=True, exist_ok=True)

    info = {
        "install_path": f"switch/ACNH-Manager/{args.nro.name}",
        "info_path": "info.json",
        "title": listing["title"],
        "author": listing["author"],
        "description": listing["summary"]["en"],
        # Exactly the fields spinarak copies into the package's info.json.
        "info_json": {
            "title": listing["title"],
            "description": listing["summary"]["en"],
            "author": listing["author"],
            "version": version,
            "license": listing["license"],
            "url": listing["url"],
            "category": listing["category"],
            "details": details_text,
            "changelog": changelog_text,
        },
    }
    zip_path = build_zip(root, args.nro, name, version, info)

    icon_copy = package_dir / "icon.png"
    icon_copy.write_bytes(icon.read_bytes())
    if banner.is_file():
        (package_dir / "screen.png").write_bytes(banner.read_bytes())

    # Screenshots.  The pkgbuild names our source files; spinarak copies whatever it reads into
    # `screen<N>.png` in the *built* package (that is the name the client asks the CDN for), so
    # the local test repository needs those generated names too.  The store's own README says the
    # files spinarak adds while building may be left out of the PR -- they are listed below.
    screenshots = [name for name in SCREENSHOT_ORDER if (SCREENSHOT_DIR / name).is_file()]
    screenshots += sorted(p.name for p in SCREENSHOT_DIR.glob("*.png")
                          if p.name not in screenshots) if SCREENSHOT_DIR.is_dir() else []
    generated_screens: list[pathlib.Path] = []
    for index, shot in enumerate(screenshots, start=1):
        source = SCREENSHOT_DIR / shot
        (package_dir / shot).write_bytes(source.read_bytes())
        generated = package_dir / f"screen{index}.png"
        generated.write_bytes(source.read_bytes())
        generated_screens.append(generated)

    # pkgbuild.json for the official data repo.  The update asset points at the published NRO,
    # which -- like the manifest and the agent's three files -- is served straight out of the
    # repository by Gitee's raw endpoint (see docs/release-process.md 6).  The version is in the
    # file name on purpose: an update asset has to keep pointing at the NRO of *its* version,
    # not at whatever was published last.  tools/release.sh copies the built NRO into that path.
    release_url = (f"https://gitee.com/acnh-game/acnh-manager/raw/main/packaging/nro/"
                   f"acnh-manager-{version}.nro")
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
            "details": details_text,
            # What the store shows as the app to launch after installing (spinarak would guess
            # it from the manifest; stating it keeps the guesswork -- and its warning -- out).
            "binary": f"/{info['install_path']}",
        },
        "changelog": changelog_text,
        "assets": [
            {"url": release_url, "dest": f"/{info['install_path']}", "type": "update"},
            {"type": "icon", "url": "icon.png"},
        ] + [{"type": "screenshot", "url": shot} for shot in screenshots],
    }
    if banner.is_file():
        pkgbuild["assets"].append({"type": "banner", "url": "screen.png"})
    (package_dir / "pkgbuild.json").write_text(json.dumps(pkgbuild, indent=2) + "\n")

    # Local test repository: the same layout AND the same fields as the official CDN, so Sphaira
    # pointed at this directory exercises the real code path (a "replica" that differs from
    # spinarak's output is worse than no test at all -- the first version of this file wrote
    # `U <path>` instead of `U: <path>`, which the client parses as a path starting one
    # character in).  Field set and formats copied from spinarak.py (2026-09-21).
    def stamp(path: pathlib.Path) -> str:
        # spinarak: datetime.utcfromtimestamp(...).strftime("%d/%m/%Y")
        return time.strftime("%d/%m/%Y", time.gmtime(path.stat().st_mtime))

    # spinarak sums the sizes of the files the manifest names; here that is the NRO alone.
    extracted_bytes = args.nro.stat().st_size
    repo = {
        "packages": [
            {
                "name": name,
                "title": listing["title"],
                "description": listing["summary"]["en"],
                "author": listing["author"],
                "version": version,
                "license": listing["license"],
                "url": listing["url"],
                "category": listing["category"],
                "details": details_text,
                "changelog": changelog_text,
                "filesize": zip_path.stat().st_size // 1024,
                "extracted": extracted_bytes // 1024,
                "md5": md5_of(zip_path),
                "sha256": sha256_of(zip_path),
                "updated": stamp(package_dir / "pkgbuild.json"),
                "appCreated": stamp(args.nro),
                "binary": f"/{info['install_path']}",
                "screens": len(screenshots),
                "web_dls": -1,  # spinarak leaves the stats to the CDN pipeline
                "app_dls": -1,
            }
        ]
    }
    (root / "repo.json").write_text(json.dumps(repo, indent=1) + "\n")

    print(f"store package for {listing['title']} {version} -> {root}")
    print(f"  zip:        {(zip_path.relative_to(root))} ({zip_path.stat().st_size} B)")
    print(f"  pkgbuild:   {(package_dir / 'pkgbuild.json').relative_to(root)}")
    print(f"  repo.json:  {(root / 'repo.json').relative_to(root)}")
    print(f"  install:    {info['install_path']}")
    if screenshots:
        print(f"  screens:    {len(screenshots)} ({', '.join(screenshots)})")
    if generated_screens:
        # Upstream: "Spinarak, while building will add additional files to your package's folder
        # prior to zipping. These can be excluded in your PR."
        print("  note:       these exist only so the local test repo matches the CDN layout; "
              "leave them out of the PR:")
        print("              " + ", ".join(p.name for p in generated_screens))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
