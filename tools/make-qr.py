#!/usr/bin/env python3
"""Turn the mini-app's code image into the module matrix the app draws.

Why a matrix and not the picture: the app has no image decoder and no business growing one.  A
QR code is a grid of black/white squares, so the build step recovers that grid and the UI paints
it with the rectangle filler it already has -- crisp at any size, and a 176-byte asset instead of
a 90 KB JPEG.

The source is the published code of the 《森友物码册》 WeChat mini-app
(`src/acnh-chat-code-miniapp/resources/小程序码.jpg`, a standard QR with the mini-app logo in the
middle).  Recovery: find the dense black block (the code, not the "微信扫一扫" caption), read the
module pitch from the top-left finder pattern's first row (7 modules wide), sample each cell, and
erase the cells the logo plate covers -- exactly the area the decoder cannot read in the original
either, so the result scans like the original does.

Needs Pillow + numpy, via the Codex-bundled runtime (see load_workspace_dependencies):
    <runtime>/python3 tools/make-qr.py
It also writes a reconstruction to build/scratch/qr-check.png for eyeballing; that file is
scratch material and never committed.
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = (REPO_ROOT.parent / "acnh-chat-code-miniapp" / "resources" / "小程序码.jpg")
DEFAULT_OUTPUT = REPO_ROOT / "data" / "qr_miniapp.bin"
CHECK_PNG = REPO_ROOT / "build" / "scratch" / "qr-check.png"


def contiguous(values) -> list[tuple[int, int]]:
    spans: list[tuple[int, int]] = []
    start = None
    for i, v in enumerate(values):
        if v and start is None:
            start = i
        elif not v and start is not None:
            spans.append((start, i - 1))
            start = None
    if start is not None:
        spans.append((start, len(values) - 1))
    return spans


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=pathlib.Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=pathlib.Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args(argv)

    try:
        import numpy as np
        from PIL import Image
    except ImportError:
        print("error: this tool needs Pillow + numpy; run it with the Codex runtime python "
              "(see load_workspace_dependencies)", file=sys.stderr)
        return 1
    if not args.source.is_file():
        print(f"error: {args.source} not found (the mini-app's code image)", file=sys.stderr)
        return 1

    image = Image.open(args.source)
    gray = image.convert("L")
    rgb = image.convert("RGB")
    pixels = np.asarray(gray, dtype=np.int16)
    colours = np.asarray(rgb, dtype=np.int16)
    black = pixels < 128
    # "Coloured" pixels are the logo: the code itself is pure black/white, JPEG noise aside.
    saturation = colours.max(axis=2) - colours.min(axis=2)
    coloured = saturation > 48

    # 1) The code is the densest horizontal block of black; the caption underneath is thin.
    bands = contiguous(black.any(axis=1))
    if not bands:
        print("error: no black pixels found", file=sys.stderr)
        return 1
    top, bottom = max(bands, key=lambda b: int(black[b[0]:b[1] + 1].sum()))
    columns = np.where(black[top:bottom + 1].any(axis=0))[0]
    left, right = int(columns[0]), int(columns[-1])

    # 2) Module pitch from the top-left finder pattern (7 modules wide on its first row).
    row = black[top]
    run = 0
    while left + run <= right and row[left + run]:
        run += 1
    if run < 7:
        print(f"error: finder pattern too small ({run} px)", file=sys.stderr)
        return 1
    pitch = run / 7.0
    size = round((right - left + 1) / pitch)
    if size < 21 or (size - 21) % 4 != 0 or abs(size * pitch - (right - left + 1)) > pitch:
        print(f"error: recovered {size} modules at pitch {pitch:.2f}px, which is not a QR size",
              file=sys.stderr)
        return 1

    # 3) Sample each cell from its middle (robust against JPEG ringing at the edges).
    matrix = np.zeros((size, size), dtype=bool)
    radius = max(1, int(pitch / 4))
    for r in range(size):
        for c in range(size):
            y = int(top + (r + 0.5) * pitch)
            x = int(left + (c + 0.5) * pitch)
            cell_black = black[y - radius:y + radius + 1, x - radius:x + radius + 1]
            cell_colour = coloured[y - radius:y + radius + 1, x - radius:x + radius + 1]
            if cell_colour.mean() > 0.25:
                matrix[r, c] = False      # the logo plate: the decoder cannot read it either
            else:
                matrix[r, c] = cell_black.mean() > 0.5

    # 4) Erase the whole plate (a circle around the logo), not just the pixels it covers.
    #    Only look inside the code: the caption's green logo/text is saturated too, and letting
    #    it into the bounding box erases half the code (measured: a huge white hole).
    inside = np.zeros_like(coloured)
    inside[top:bottom + 1, left:right + 1] = True
    ys, xs = np.where(coloured & inside)
    if len(ys):
        cy = (ys.min() + ys.max()) / 2.0
        cx = (xs.min() + xs.max()) / 2.0
        rad = max(ys.max() - ys.min(), xs.max() - xs.min()) / 2.0 + pitch
        for r in range(size):
            for c in range(size):
                y = top + (r + 0.5) * pitch
                x = left + (c + 0.5) * pitch
                if (y - cy) ** 2 + (x - cx) ** 2 <= rad * rad:
                    matrix[r, c] = False

    # 5) Asset: little-endian u16 size x u16 size, then rows bit-packed, MSB first.
    packed = bytearray()
    for r in range(size):
        bits = 0
        count = 0
        for c in range(size):
            bits = (bits << 1) | (1 if matrix[r, c] else 0)
            count += 1
            if count == 8:
                packed.append(bits)
                bits = 0
                count = 0
        if count:
            packed.append(bits << (8 - count))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(struct.pack("<HH", size, size) + bytes(packed))

    # 6) A picture of what the app will draw, for eyeballing (scratch material).
    scale = 8
    check = Image.new("RGB", (size * scale, size * scale), (255, 255, 255))
    px = check.load()
    for r in range(size):
        for c in range(size):
            if matrix[r, c]:
                for y in range(r * scale, (r + 1) * scale):
                    for x in range(c * scale, (c + 1) * scale):
                        px[x, y] = (0, 0, 0)
    CHECK_PNG.parent.mkdir(parents=True, exist_ok=True)
    check.save(CHECK_PNG)

    print(f"source: {args.source.name} {image.size}")
    print(f"code:   {size}x{size} modules (version {(size - 17) // 4}), pitch {pitch:.2f} px, "
          f"origin ({left},{top})")
    print(f"wrote {args.output.relative_to(REPO_ROOT)} ({args.output.stat().st_size} B) and "
          f"{CHECK_PNG.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
