#!/usr/bin/env python3
"""Decode a compressed content (NSZ/XCZ) the naive way: one zstd block at a time.

Why it exists: `tools/ncz-probe.sh` runs the app's own decoder, so it answers "what would the
install do".  This tool answers the other half of the question -- "what does the file actually
say" -- without sharing a single line of code with it.  It parses the container's own tables,
splits the payload the way the block table says, decompresses each block with the `zstd` command
line (or python-zstandard when it is installed) and prints the number of decoded bytes and their
sha256.  Two tools agreeing on both numbers is what makes a decoding rule credible before a
console is involved; a disagreement points at the reader, not at the file.

It also prints the block table itself (block size, count, which blocks are compressed and which
are stored raw), which is usually where the argument ends.

It only reads: no keys, no temporary files.  `--limit N` stops after N blocks, which is enough to
see whether the first blocks decode at all.

Usage:
    python3 tools/ncz-blocks.py <.nsz|.xcz> [--entry N] [--limit N]
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import shutil
import struct
import subprocess
import sys

NCZ_HEADER_SIZE = 0x4000
NCZ_SECTION_MAGIC = 0x4E544345535A434E
NCZ_BLOCK_MAGIC = 0x4B434F4C425A434E
NCZ_SECTION_SIZE = 64


def u32(data: bytes, at: int) -> int:
    return struct.unpack_from("<I", data, at)[0]


def u64(data: bytes, at: int) -> int:
    return struct.unpack_from("<Q", data, at)[0]


def hfs0_entries(fd, offset: int) -> list[tuple[str, int, int]]:
    """The entries of one HFS0 (an XCI's partitions, or the contents inside `secure`)."""
    fd.seek(offset)
    head = fd.read(0x10)
    if head[:4] != b"HFS0":
        raise SystemExit(f"no HFS0 at 0x{offset:x}")
    count = u32(head, 4)
    strings = u32(head, 8)
    table = offset + 0x10
    data_area = table + count * 0x40 + strings
    entries = []
    for index in range(count):
        fd.seek(table + index * 0x40)
        entry = fd.read(0x40)
        name_at = u32(entry, 0x10)
        fd.seek(table + count * 0x40 + name_at)
        name = b""
        while True:
            char = fd.read(1)
            if char in (b"", b"\0"):
                break
            name += char
        entries.append((name.decode(), data_area + u64(entry, 0), u64(entry, 8)))
    return entries


def pfs0_entries(fd) -> list[tuple[str, int, int]]:
    fd.seek(0)
    head = fd.read(0x10)
    if head[:4] != b"PFS0":
        raise SystemExit("not a PFS0 container")
    count = u32(head, 4)
    strings = u32(head, 8)
    table = 0x10
    data_area = table + count * 0x18 + strings
    entries = []
    for index in range(count):
        fd.seek(table + index * 0x18)
        entry = fd.read(0x18)
        name_at = u32(entry, 0x10)
        fd.seek(table + count * 0x18 + name_at)
        name = b""
        while True:
            char = fd.read(1)
            if char in (b"", b"\0"):
                break
            name += char
        entries.append((name.decode(), data_area + u64(entry, 0), u64(entry, 8)))
    return entries


def contents(fd) -> list[tuple[str, int, int]]:
    """Every file of an NSP/NSZ, or of an XCI/XCZ's secure partition."""
    fd.seek(0x100)
    if fd.read(4) == b"HEAD":
        fd.seek(0x130)
        root = hfs0_entries(fd, u64(fd.read(8), 0))
        secure = [entry for entry in root if entry[0] == "secure"]
        if not secure:
            raise SystemExit("the gamecard image has no secure partition")
        return hfs0_entries(fd, secure[0][1])
    return pfs0_entries(fd)


def decompress(raw: bytes) -> bytes:
    """One block.  python-zstandard when it is installed (fast), the command line otherwise."""
    try:
        import zstandard  # type: ignore
    except ImportError:
        zstandard = None
    if zstandard is not None:
        return zstandard.ZstdDecompressor().decompressobj().decompress(raw)
    if shutil.which("zstd") is None:
        raise SystemExit("neither python-zstandard nor the zstd command line is available")
    done = subprocess.run(["zstd", "-d", "-c"], input=raw, capture_output=True)
    if done.returncode != 0:
        raise SystemExit(f"zstd refused a block: {done.stderr[:200]!r}")
    return done.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("container", type=pathlib.Path)
    parser.add_argument("--entry", type=int, default=-1,
                        help="index of the content to decode (default: the biggest .ncz)")
    parser.add_argument("--limit", type=int, default=0, help="stop after this many blocks")
    args = parser.parse_args()

    size = args.container.stat().st_size
    with args.container.open("rb") as fd:
        files = contents(fd)
        packed = [item for item in files if item[0].endswith(".ncz")]
        for index, (name, offset, length) in enumerate(files):
            print(f"  [{index}] {name} offset=0x{offset:x} size={length}"
                  f"{' (ncz)' if name.endswith('.ncz') else ''}")
        if not packed:
            print("no .ncz entry: nothing is compressed")
            return 1
        if args.entry >= 0:
            if args.entry >= len(files):
                raise SystemExit(f"no content {args.entry}")
            name, offset, length = files[args.entry]
            if not name.endswith(".ncz"):
                raise SystemExit(f"content {args.entry} is not compressed")
        else:
            name, offset, length = max(packed, key=lambda item: item[2])
        print(f"decoding {name} (offset=0x{offset:x} packed={length})")

        fd.seek(offset + NCZ_HEADER_SIZE)
        magic, sections = struct.unpack("<QQ", fd.read(16))
        if magic != NCZ_SECTION_MAGIC:
            raise SystemExit("this .ncz has no section table")
        section_table = []
        for _ in range(sections):
            row = fd.read(NCZ_SECTION_SIZE)
            section_table.append((u64(row, 0), u64(row, 8), u64(row, 16)))
        nca_size = max(start + span for start, span, _ in section_table)
        print(f"  sections={sections} nca size={nca_size}")

        block_header = fd.read(24)
        block_sizes: list[int] = []
        block_size = 0
        decompressed_size = 0
        if u64(block_header, 0) == NCZ_BLOCK_MAGIC:
            version, kind, exponent = block_header[8], block_header[9], block_header[11]
            total = u32(block_header, 12)
            decompressed_size = u64(block_header, 16)
            if version != 2 or kind != 1:
                raise SystemExit(f"block table version {version} type {kind} is not supported")
            block_size = 1 << exponent
            block_sizes = [u32(fd.read(4), 0) for _ in range(total)]
            print(f"  block table: {total} blocks of {block_size} decompressed"
                  f" ({decompressed_size} bytes in total),"
                  f" first sizes {block_sizes[:4]}...{block_sizes[-2:]}")
        if block_size == 0:
            # No block table: the payload really is one stream, and its length is the rest of the
            # entry.
            payload_at = fd.tell()
            payload_size = offset + length - payload_at
            print(f"  no block table: one stream of {payload_size} bytes")
            fd.seek(payload_at)
            decoded = decompress(fd.read(payload_size))
            print(f"decoded {len(decoded)} bytes sha256={hashlib.sha256(decoded).hexdigest()}")
            return 0

        payload_at = fd.tell()
        expected_total = sum(block_sizes)
        print(f"  payload at 0x{payload_at:x}, blocks take {expected_total} bytes,"
              f" entry ends at 0x{offset + length:x}"
              f" (delta {offset + length - payload_at - expected_total})")

        limit = args.limit if args.limit > 0 else len(block_sizes)
        digest = hashlib.sha256()
        decoded_total = 0
        raw_blocks = 0
        block_at = payload_at
        for index, stored in enumerate(block_sizes[:limit]):
            if index == len(block_sizes) - 1:
                remainder = decompressed_size % block_size
                wants = remainder if remainder else block_size
            else:
                wants = block_size
            fd.seek(block_at)
            raw = fd.read(stored)
            block_at += stored
            if stored < wants:
                out = decompress(raw)
            else:
                out = raw
                raw_blocks += 1
            if len(out) != wants:
                raise SystemExit(f"block {index}: decoded {len(out)} bytes, table says {wants}")
            digest.update(out)
            decoded_total += len(out)
            if index < 4 or index >= len(block_sizes) - 2:
                print(f"  block {index}: {stored} -> {len(out)}"
                      f" {'compressed' if stored < wants else 'stored raw'}")
        print(f"decoded {decoded_total} bytes ({raw_blocks} raw blocks)"
              f" sha256={digest.hexdigest()}")
        if limit > 0 and limit < len(block_sizes):
            print(f"(stopped after {limit} of {len(block_sizes)} blocks: not the whole payload)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
