#!/usr/bin/env python3
"""从设计主图生成自制程序与商店要用的两份图标。

产物(都提交进仓库,体积很小):
    assets/icon.jpg   256×256 JPEG —— elf2nro 嵌进 NACP,hbmenu/相册里显示的图标
    assets/icon.png   256×256 PNG  —— Homebrew App Store 商店条目图标(打包时上传)

主图是 `assets/icon-org.png`(设计稿,提交进仓库,作为唯一来源);换图标时替换主图后重跑本工具。
历史:第一版沿用微信小程序图标(`src/acnh-chat-code-miniapp/resources/app-icon/app-icon-1024.png`),
上架前改为专用设计稿;需要时可用 `--source` 指回小程序图标。

依赖 Pillow,用 Codex 自带运行时(路径以 load_workspace_dependencies 的返回为准),例如:
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
