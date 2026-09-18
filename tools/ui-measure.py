#!/usr/bin/env python3
"""Pixel ruler for the UI: turn "looks off by a bit" into numbers.

Four ways to interrogate one screenshot, all of them "where is the ink inside this window":

    bbox  <image> <x0> <y0> <x1> <y1>     bounding box of everything that differs from the
                                          window's background, with the left/right insets
    lines <image> <x0> <y0> <x1> <y1>     the same, segmented into text lines (see each line's
                                          insets, height and width)
    runs  <image> <y> <x0> <x1>           ink runs along one row (left edges, centring, gaps)
    color <image> <x0> <y0> <x1> <y1> --rgb r,g,b
                                          bounding box, centre and pixel count of a colour
                                          (badges, fills, status dots)

The background defaults to the page background; pass `--bg card` (or `page`, `header`,
`border`) when you are inside a card, and `--bg 12,34,56` for anything else.  Those names are
the app's own palette (`kBackground` / `kCard` / `kHeader` / `kBorder` in `source/ui/app.cpp`)
-- keep them in step when the palette changes.  `--tol` is the per-channel tolerance; the
default 12 covers JPEG artefacts on a solid fill.

Why a tool instead of eyeballing: every UI bug fixed in this project that was described as
"looks a bit off" turned into an exact number here first (a label 6 px off centre, a card
whose bottom padding was 22 px instead of 12, a status dot that read as a button at 44 px).

Captures come from `tools/device-tests.py` (or `src/sys-agent/client/sysagent.py screen
capture`), and this reads them straight off the disk.
"""

import argparse
import sys

from PIL import Image

PALETTE = {
    "page": (0xF6, 0xF1, 0xE3),
    "card": (0xFF, 0xFD, 0xF7),
    "header": (0x2F, 0xBF, 0xA8),
    "border": (0xD8, 0xCF, 0xB6),
}


def parse_color(text, default):
    if text is None:
        return default
    if text in PALETTE:
        return PALETTE[text]
    parts = text.split(",")
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("expected r,g,b or one of %s" % ", ".join(PALETTE))
    return tuple(int(value) for value in parts)


def differs(pixel, background, tolerance):
    return any(abs(pixel[i] - background[i]) > tolerance for i in range(3))


def window(image, args):
    pixels = image.load()
    for y in range(args.y0, args.y1):
        left = right = None
        for x in range(args.x0, args.x1):
            if differs(pixels[x, y], args.background, args.tolerance):
                if left is None:
                    left = x
                right = x
        yield y, left, right


def command_bbox(image, args):
    print("== %s %dx%d  window x %d..%d y %d..%d  bg=%s tol=%d"
          % (args.image, image.width, image.height, args.x0, args.x1, args.y0, args.y1,
             args.background, args.tolerance))
    min_x = min_y = max_x = max_y = None
    for y, left, right in window(image, args):
        if left is None:
            continue
        min_y = y if min_y is None else min_y
        max_y = y
        min_x = left if min_x is None else min(min_x, left)
        max_x = right if max_x is None else max(max_x, right)
    if min_x is None:
        print("  (no ink in this window)")
        return 0
    print("  ink x %4d..%4d (w=%3d)  y %3d..%3d (h=%3d)  insets L=%d R=%d T=%d B=%d"
          % (min_x, max_x, max_x - min_x + 1, min_y, max_y, max_y - min_y + 1,
             min_x - args.x0, args.x1 - 1 - max_x, min_y - args.y0, args.y1 - 1 - max_y))
    return 0


def command_lines(image, args):
    print("== %s  window x %d..%d y %d..%d  bg=%s tol=%d"
          % (args.image, args.x0, args.x1, args.y0, args.y1, args.background, args.tolerance))
    start = None
    left = right = None
    lines = 0
    for y, row_left, row_right in window(image, args):
        if row_left is not None:
            if start is None:
                start, left, right = y, row_left, row_right
            else:
                left = min(left, row_left)
                right = max(right, row_right)
            continue
        if start is not None:
            lines += 1
            print("  line %2d  y %3d..%3d (h=%2d)  x %4d..%4d  inset L=%3d R=%4d  width=%3d"
                  % (lines, start, y - 1, y - start, left, right, left - args.x0,
                     args.x1 - 1 - right, right - left + 1))
            start = None
    print("  %d line(s)" % lines)
    return 0


def command_runs(image, args):
    pixels = image.load()
    runs = []
    start = None
    for x in range(args.x0, args.x1):
        ink = differs(pixels[x, args.y], args.background, args.tolerance)
        if ink and start is None:
            start = x
        elif not ink and start is not None:
            runs.append((start, x - 1))
            start = None
    if start is not None:
        runs.append((start, args.x1 - 1))
    print("== %s  row y=%d  x %d..%d  bg=%s tol=%d"
          % (args.image, args.y, args.x0, args.x1, args.background, args.tolerance))
    if not runs:
        print("  (no ink on this row)")
        return 0
    for index, (left, right) in enumerate(runs, 1):
        print("  run %2d  x %4d..%4d (w=%3d)  centre %.1f" % (index, left, right,
                                                              right - left + 1,
                                                              (left + right) / 2))
    print("  %d run(s), first ink at x=%d, last at x=%d" % (len(runs), runs[0][0], runs[-1][1]))
    return 0


def command_color(image, args):
    pixels = image.load()
    target = args.rgb
    min_x = min_y = max_x = max_y = None
    count = 0
    for y in range(args.y0, args.y1):
        for x in range(args.x0, args.x1):
            if not differs(pixels[x, y], target, args.tolerance):
                count += 1
                min_x = x if min_x is None else min(min_x, x)
                max_x = x if max_x is None else max(max_x, x)
                min_y = y if min_y is None else min(min_y, y)
                max_y = y if max_y is None else max(max_y, y)
    print("== %s  window x %d..%d y %d..%d  target=%s tol=%d"
          % (args.image, args.x0, args.x1, args.y0, args.y1, target, args.tolerance))
    if count == 0:
        print("  no pixels within tolerance")
        return 1
    print("  bbox x %4d..%4d (w=%3d)  y %3d..%3d (h=%3d)  centre (%.1f, %.1f)  pixels=%d"
          % (min_x, max_x, max_x - min_x + 1, min_y, max_y, max_y - min_y + 1,
             (min_x + max_x) / 2, (min_y + max_y) / 2, count))
    return 0


def add_window(parser, default_bg):
    parser.add_argument("image")
    parser.add_argument("x0", type=int)
    parser.add_argument("y0", type=int)
    parser.add_argument("x1", type=int)
    parser.add_argument("y1", type=int)
    parser.add_argument("--bg", default=default_bg,
                        help="page|card|header|border or r,g,b (default: %(default)s)")
    parser.add_argument("--tol", dest="tolerance", type=int, default=12)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)
    for name, handler, default_bg in (("bbox", command_bbox, "page"),
                                      ("lines", command_lines, "card"),
                                      ("color", command_color, "card")):
        sub = subparsers.add_parser(name, help=handler.__doc__ or name)
        add_window(sub, default_bg)
        if name == "color":
            sub.add_argument("--rgb", required=True, help="r,g,b to look for")
            # Colour matching is looser than "is this ink?": anti-aliased edges of a badge or
            # fill are still that colour for measurement purposes.
            sub.set_defaults(tolerance=24)
        sub.set_defaults(handler=handler)
    sub = subparsers.add_parser("runs", help="ink runs along one row")
    sub.add_argument("image")
    sub.add_argument("y", type=int)
    sub.add_argument("x0", type=int)
    sub.add_argument("x1", type=int)
    sub.add_argument("--bg", default="page",
                     help="page|card|header|border or r,g,b (default: %(default)s)")
    sub.add_argument("--tol", dest="tolerance", type=int, default=12)
    sub.set_defaults(handler=command_runs)

    args = parser.parse_args(argv)
    args.background = parse_color(args.bg, PALETTE["page"])
    if getattr(args, "rgb", None) is not None:
        args.rgb = parse_color(args.rgb, None)
    image = Image.open(args.image).convert("RGB")
    return args.handler(image, args)


if __name__ == "__main__":
    sys.exit(main())
