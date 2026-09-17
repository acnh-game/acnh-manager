#!/usr/bin/env python3
"""Push a built NRO to the Switch SD card (sys-agent's built-in FTP, default port 6001).

For the development loop: build -> deploy -> launch from the album/hbmenu -> read the log back.
It never writes a game directory and never reboots anything.

Usage:
    python3 tools/deploy-nro.py                    # deploy acnh-manager.nro
    python3 tools/deploy-nro.py --fetch-log        # only fetch spike.log / log.txt
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
    """Create each directory level (an existing level is not an error)."""
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
    # Read-back check: pull the file back and compare sha256, so "on the card is exactly
    readback = bytearray()
    ftp.retrbinary(f"RETR {remote}", readback.extend)
    remote_sha = hashlib.sha256(bytes(readback)).hexdigest()
    if remote_sha != local_sha:
        raise RuntimeError(f"sha256 mismatch after upload: local={local_sha[:16]} "
                           f"remote={remote_sha[:16]} (a running instance keeps the file busy; exit it first)")
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
        # Fetch fully before writing to disk: a failed fetch leaves no 0-byte leftover.
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
                        help="push any local file to the SD card (--remote names the relative path, e.g. payload/subsdk9)")
    parser.add_argument("--remote", default=None, help="relative remote path used with --push-file")
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
