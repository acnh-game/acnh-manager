#!/usr/bin/env python3
"""Derive the icons and the store banner from their master images.

Outputs (both committed; they are small):
    assets/icon.jpg   256x256 JPEG -- embedded into the NACP, shown by hbmenu/album
    assets/icon.png   256x256 PNG  -- Homebrew App Store listing icon (uploaded when packaging)
    assets/screen.png 848x208 PNG  -- store banner (details page), derived from screen-org.png

The master images are assets/icon-org.png and assets/screen-org.png (the design sources,
committed, single source of truth): replace them and re-run this tool.  History: the first
version reused the WeChat mini-app icon; a dedicated design replaced it before release
(--source points back).

The banner is **cropped, never scaled to fit**: 848x208 is what the store client draws the
banner into (Sphaira: `banner_vec(70, ..., 848.f, 208.f)`), so feeding it a taller picture would
stretch it.  The crop keeps the full width and takes a band from the middle, which is where the
design puts its subject.

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
DEFAULT_BANNER_SOURCE = REPO_ROOT / "assets" / "screen-org.png"
BANNER_SIZE = (848, 208)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path, default=DEFAULT_SOURCE)
    parser.add_argument("--banner-source", type=pathlib.Path, default=DEFAULT_BANNER_SOURCE)
    parser.add_argument("--skip-banner", action="store_true",
                        help="only regenerate the icons (no assets/screen-org.png yet)")
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

    if args.skip_banner or not args.banner_source.is_file():
        print(f"banner: skipped ({args.banner_source} not found)" if not args.skip_banner
              else "banner: skipped (--skip-banner)")
        return 0
    width, height = BANNER_SIZE
    with Image.open(args.banner_source) as image:
        print(f"banner source: {args.banner_source} {image.size} {image.mode}")
        band = round(image.width * height / width)
        if band > image.height:
            print(f"error: {args.banner_source} is {image.width}x{image.height}; a {width}:{height} "
                  f"crop needs {image.width}x{band}. Give the design more width or less height.",
                  file=sys.stderr)
            return 1
        top = (image.height - band) // 2
        banner = image.convert("RGB").crop((0, top, image.width, top + band))
        banner = banner.resize(BANNER_SIZE, Image.LANCZOS)
        banner_path = out_dir / "screen.png"
        banner.save(banner_path, "PNG", optimize=True)
    print(f"wrote {banner_path.relative_to(REPO_ROOT)} ({banner_path.stat().st_size} B, "
          f"cropped y{top}..{top + band} of {args.banner_source.name})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
