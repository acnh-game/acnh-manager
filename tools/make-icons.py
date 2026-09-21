#!/usr/bin/env python3
"""Derive the icons and the store banner from their master images.

Outputs (both committed; they are small):
    assets/icon.jpg   256x256 JPEG -- embedded into the NACP, shown by hbmenu/album
    assets/icon.png   256x150 PNG  -- store icon, see "why not square" below
    assets/screen.png 848x208 PNG  -- store banner (details page), derived from screen-org.png

Why the store icon is 256x150 and not square: the store client draws the details-page icon at
its *native* size into a 256x150 box and clips whatever overflows (Sphaira, ui/menus/appstore.cpp:
`icon_vec(968, ..., 256, 150)` plus a DrawIcon that centres without scaling).  A square icon
therefore loses its top and bottom ~53 px there -- measured on hardware 2026-09-21.  Every other
package in the store ships a 256x150 icon, so this one is composed the same way: a centred band
of the master, scaled to 150 px tall, with the leftover width filled with the artwork's own sky
colour.  The NACP/hbmenu icon stays square: that one is what the console's homebrew menu shows.

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
# The store icon: the box the client draws it into, and how much of the master to keep.  900 of
# the master's 1254 rows keeps the parcel (including the arrow's tip) whole; the remaining width
# is padded, so nothing important ends up outside the 256x150 window.
STORE_ICON_SIZE = (256, 150)
STORE_ICON_BAND = 900


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
        master = image.convert("RGB")
        rgb = master.resize((args.size, args.size), Image.LANCZOS)
        jpg = out_dir / "icon.jpg"
        rgb.save(jpg, "JPEG", quality=args.jpeg_quality, optimize=True)
        # Store icon: centred band of the master -> STORE_ICON_SIZE, padded with the sky colour.
        width, height = STORE_ICON_SIZE
        band = min(STORE_ICON_BAND, master.height)
        top = (master.height - band) // 2
        art = master.crop((0, top, master.width, top + band))
        art = art.resize((round(master.width * height / band), height), Image.LANCZOS)
        if art.width > width:
            art = art.crop(((art.width - width) // 2, 0, (art.width - width) // 2 + width, height))
        store_icon = Image.new("RGB", STORE_ICON_SIZE,
                               master.getpixel((master.width // 2, 2)))  # the sky at the top
        store_icon.paste(art, ((width - art.width) // 2, 0))
        png = out_dir / "icon.png"
        store_icon.save(png, "PNG", optimize=True)
        print(f"store icon: kept rows {top}..{top + band} of {master.height}, "
              f"padded {(width - art.width) // 2} px each side")
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
