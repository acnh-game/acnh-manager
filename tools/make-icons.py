#!/usr/bin/env python3
"""Derive the two icons this project needs (homebrew NACP and store listing) from the master image.

Outputs (both committed; they are small):
    assets/icon.jpg   256x256 JPEG -- embedded into the NACP, shown by hbmenu/album
    assets/icon.png   256x256 PNG  -- Homebrew App Store listing icon (uploaded when packaging)

The master image is assets/icon-org.png (the design source, committed, single source of truth):
replace it and re-run this tool to change the icons.  History: the first version reused the
WeChat mini-app icon; a dedicated design replaced it before release (--source points back).

Needs Pillow, via the Codex-bundled runtime (see load_workspace_dependencies), for example:
    /Users/leo/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3 \
        tools/make-icons.py
"""

from __future__ import annotations

import argparse
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = REPO_ROOT / "assets" / "icon-org.png"
SIZE = 256


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path, default=DEFAULT_SOURCE)
    parser.add_argument("--size", type=int, default=SIZE)
    parser.add_argument("--jpeg-quality", type=int, default=90)
    args = parser.parse_args(argv)

    try:
        from PIL import Image
    except ImportError:
        print("error: Pillow not available; run this with the Codex runtime python "
              "(see load_workspace_dependencies)", file=sys.stderr)
        return 1

    if not args.source.is_file():
        print(f"error: source icon not found: {args.source}", file=sys.stderr)
        return 1

    out_dir = REPO_ROOT / "assets"
    out_dir.mkdir(exist_ok=True)
    with Image.open(args.source) as image:
        print(f"source: {args.source} {image.size} {image.mode}")
        rgb = image.convert("RGB").resize((args.size, args.size), Image.LANCZOS)
        jpg = out_dir / "icon.jpg"
        rgb.save(jpg, "JPEG", quality=args.jpeg_quality, optimize=True)
        png = out_dir / "icon.png"
        rgb.save(png, "PNG", optimize=True)
    for path in (jpg, png):
        print(f"wrote {path.relative_to(REPO_ROOT)} ({path.stat().st_size} B)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
