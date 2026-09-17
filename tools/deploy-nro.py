#!/usr/bin/env python3
"""把构建好的 NRO 推到 Switch 的 SD 卡上(sys-agent 内置 FTP,默认端口 6001)。

开发期迭代用:构建 -> 部署 -> 在相册/hbmenu 里启动 -> 回读日志。
不写任何游戏目录,也不重启任何东西。

用法:
    python3 tools/deploy-nro.py                    # 部署 acnh-manager.nro
    python3 tools/deploy-nro.py --fetch-log        # 只取回 spike.log / log.txt
    python3 tools/deploy-nro.py --host switch --port 6001
    python3 tools/deploy-nro.py --file build/other.nro --remote-name other.nro
"""

from __future__ import annotations

import argparse
import ftplib
import hashlib
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
REMOTE_DIR = "/switch/ACNH-Manager"
DEFAULT_NRO = REPO_ROOT / "acnh-manager.nro"
LOG_NAMES = ("spike.log", "spike-history.log", "log.txt")


def ensure_dir(ftp: ftplib.FTP, path: str) -> None:
    """逐级创建目录(已存在时忽略失败)。"""
    current = ""
    for part in (p for p in path.split("/") if p):
        current += "/" + part
        try:
            ftp.mkd(current)
        except ftplib.error_perm:
            pass


def deploy(ftp: ftplib.FTP, local: pathlib.Path, remote_name: str) -> None:
    ensure_dir(ftp, REMOTE_DIR)
    remote = f"{REMOTE_DIR}/{remote_name}"
    local_bytes = local.read_bytes()
    local_sha = hashlib.sha256(local_bytes).hexdigest()
    size = local.stat().st_size
    with local.open("rb") as handle:
        ftp.storbinary(f"STOR {remote}", handle)
    remote_size = ftp.size(remote)
    if remote_size != size:
        raise RuntimeError(f"size mismatch after upload: local={size} remote={remote_size}")
    # 回读校验:把远端文件读回来比对 sha256,确认"卡上的就是刚构建的这份"。
    readback = bytearray()
    ftp.retrbinary(f"RETR {remote}", readback.extend)
    remote_sha = hashlib.sha256(bytes(readback)).hexdigest()
    if remote_sha != local_sha:
        raise RuntimeError(f"sha256 mismatch after upload: local={local_sha[:16]} "
                           f"remote={remote_sha[:16]} (程序正在运行时会写不进去,请先退出)")
    print(f"deployed {local.name} ({size} B) -> {remote} (sha256 {local_sha[:16]}… verified)")


def fetch_logs(ftp: ftplib.FTP, out_dir: pathlib.Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for name in LOG_NAMES:
        target = out_dir / name
        payload = bytearray()
        try:
            ftp.retrbinary(f"RETR {REMOTE_DIR}/{name}", payload.extend)
        except ftplib.error_perm:
            print(f"log {name}: not present")
            continue
        # 先完整取回再落盘:取不到时不留下 0 字节残留文件。
        target.write_bytes(bytes(payload))
        print(f"fetched {REMOTE_DIR}/{name} -> {target} ({target.stat().st_size} B)")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="switch")
    parser.add_argument("--port", type=int, default=6001)
    parser.add_argument("--file", type=pathlib.Path, default=DEFAULT_NRO)
    parser.add_argument("--remote-name", default=None)
    parser.add_argument("--fetch-log", action="store_true")
    parser.add_argument("--push-file", type=pathlib.Path,
                        help="把任意本地文件推到 SD(--remote 指定相对路径,如 payload/subsdk9)")
    parser.add_argument("--remote", default=None, help="配合 --push-file 的远端相对路径")
    parser.add_argument("--log-dir", type=pathlib.Path,
                        default=REPO_ROOT / "build" / "scratch")
    args = parser.parse_args(argv)

    ftp = ftplib.FTP()
    try:
        ftp.connect(args.host, args.port, timeout=15)
        ftp.login()
    except OSError as exc:
        print(f"error: cannot reach {args.host}:{args.port} ({exc})", file=sys.stderr)
        return 1

    try:
        if args.push_file is not None:
            if args.remote is None:
                print("error: --push-file requires --remote", file=sys.stderr)
                return 1
            if not args.push_file.is_file():
                print(f"error: {args.push_file} not found", file=sys.stderr)
                return 1
            remote = f"{REMOTE_DIR}/{args.remote.lstrip('/')}"
            ensure_dir(ftp, remote.rsplit("/", 1)[0])
            with args.push_file.open("rb") as handle:
                ftp.storbinary(f"STOR {remote}", handle)
            size = args.push_file.stat().st_size
            if ftp.size(remote) != size:
                print(f"error: size mismatch after upload: {remote}", file=sys.stderr)
                return 1
            print(f"pushed {args.push_file} -> {remote} ({size} B, verified)")
        elif args.fetch_log:
            fetch_logs(ftp, args.log_dir)
        else:
            if not args.file.is_file():
                print(f"error: {args.file} not found (build first)", file=sys.stderr)
                return 1
            deploy(ftp, args.file, args.remote_name or args.file.name)
    finally:
        ftp.quit()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
