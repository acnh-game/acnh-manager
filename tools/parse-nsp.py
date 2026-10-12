#!/usr/bin/env python3
"""Parse an NSP on the development machine: what the console code is supposed to do, done here.

Why it exists: the install path reads a container, decrypts the meta NCA's header and reads the
CNMT out of it.  Getting any of those steps wrong costs a build, a deploy and a console round
trip, and the console can only tell us "not installable" plus a one-line diagnostic.  This tool
runs the same steps against a real package on the Mac, so the layout questions ("is the CNMT at
0xC20?", "is that section plaintext?") are answered here instead of on hardware.

It never writes anything and never prints key material: keys are read from
`SDcard/switch/{prod,title}.keys` (tracked in this workspace, see the root AGENTS.md) and only
used in memory.

Usage:
    <python> tools/parse-nsp.py "../upd/[UPD][v2.0.0][...].nsp"
    <python> tools/parse-nsp.py foo.nsp --keys ../../SDcard/switch
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys

# ---------------------------------------------------------------------------------------------
# AES-128 (encryption of a single block), XTS and CTR modes.  Written out instead of imported
# because the development Python has no crypto package, and this only ever handles a few KB.

_SBOX = [
    0x63,0x7C,0x77,0x7B,0xF2,0x6B,0x6F,0xC5,0x30,0x01,0x67,0x2B,0xFE,0xD7,0xAB,0x76,
    0xCA,0x82,0xC9,0x7D,0xFA,0x59,0x47,0xF0,0xAD,0xD4,0xA2,0xAF,0x9C,0xA4,0x72,0xC0,
    0xB7,0xFD,0x93,0x26,0x36,0x3F,0xF7,0xCC,0x34,0xA5,0xE5,0xF1,0x71,0xD8,0x31,0x15,
    0x04,0xC7,0x23,0xC3,0x18,0x96,0x05,0x9A,0x07,0x12,0x80,0xE2,0xEB,0x27,0xB2,0x75,
    0x09,0x83,0x2C,0x1A,0x1B,0x6E,0x5A,0xA0,0x52,0x3B,0xD6,0xB3,0x29,0xE3,0x2F,0x84,
    0x53,0xD1,0x00,0xED,0x20,0xFC,0xB1,0x5B,0x6A,0xCB,0xBE,0x39,0x4A,0x4C,0x58,0xCF,
    0xD0,0xEF,0xAA,0xFB,0x43,0x4D,0x33,0x85,0x45,0xF9,0x02,0x7F,0x50,0x3C,0x9F,0xA8,
    0x51,0xA3,0x40,0x8F,0x92,0x9D,0x38,0xF5,0xBC,0xB6,0xDA,0x21,0x10,0xFF,0xF3,0xD2,
    0xCD,0x0C,0x13,0xEC,0x5F,0x97,0x44,0x17,0xC4,0xA7,0x7E,0x3D,0x64,0x5D,0x19,0x73,
    0x60,0x81,0x4F,0xDC,0x22,0x2A,0x90,0x88,0x46,0xEE,0xB8,0x14,0xDE,0x5E,0x0B,0xDB,
    0xE0,0x32,0x3A,0x0A,0x49,0x06,0x24,0x5C,0xC2,0xD3,0xAC,0x62,0x91,0x95,0xE4,0x79,
    0xE7,0xC8,0x37,0x6D,0x8D,0xD5,0x4E,0xA9,0x6C,0x56,0xF4,0xEA,0x65,0x7A,0xAE,0x08,
    0xBA,0x78,0x25,0x2E,0x1C,0xA6,0xB4,0xC6,0xE8,0xDD,0x74,0x1F,0x4B,0xBD,0x8B,0x8A,
    0x70,0x3E,0xB5,0x66,0x48,0x03,0xF6,0x0E,0x61,0x35,0x57,0xB9,0x86,0xC1,0x1D,0x9E,
    0xE1,0xF8,0x98,0x11,0x69,0xD9,0x8E,0x94,0x9B,0x1E,0x87,0xE9,0xCE,0x55,0x28,0xDF,
    0x8C,0xA1,0x89,0x0D,0xBF,0xE6,0x42,0x68,0x41,0x99,0x2D,0x0F,0xB0,0x54,0xBB,0x16,
]
_RCON = [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]

_INV_SBOX = [0] * 256
for _i, _v in enumerate(_SBOX):
    _INV_SBOX[_v] = _i



def _xtime(a: int) -> int:
    a <<= 1
    return (a ^ 0x1B) & 0xFF if a & 0x100 else a


def _expand_key(key: bytes) -> list[bytes]:
    assert len(key) == 16
    words = [list(key[i * 4 : i * 4 + 4]) for i in range(4)]
    for i in range(4, 44):
        temp = list(words[i - 1])
        if i % 4 == 0:
            temp = temp[1:] + temp[:1]
            temp = [_SBOX[b] for b in temp]
            temp[0] ^= _RCON[i // 4 - 1]
        words.append([words[i - 4][j] ^ temp[j] for j in range(4)])
    return [bytes(sum(words[r * 4 : r * 4 + 4], [])) for r in range(11)]


def _encrypt_block(round_keys: list[bytes], block: bytes) -> bytes:
    state = [block[i] ^ round_keys[0][i] for i in range(16)]
    for rnd in range(1, 11):
        state = [_SBOX[b] for b in state]
        state = [
            state[0], state[5], state[10], state[15],
            state[4], state[9], state[14], state[3],
            state[8], state[13], state[2], state[7],
            state[12], state[1], state[6], state[11],
        ]
        if rnd != 10:
            mixed = []
            for c in range(4):
                a = state[c * 4 : c * 4 + 4]
                t = a[0] ^ a[1] ^ a[2] ^ a[3]
                mixed += [
                    a[0] ^ t ^ _xtime(a[0] ^ a[1]),
                    a[1] ^ t ^ _xtime(a[1] ^ a[2]),
                    a[2] ^ t ^ _xtime(a[2] ^ a[3]),
                    a[3] ^ t ^ _xtime(a[3] ^ a[0]),
                ]
            state = mixed
        state = [state[i] ^ round_keys[rnd][i] for i in range(16)]
    return bytes(state)



def _mul(a: int, b: int) -> int:
    """GF(2^8) multiply (only the constants 9/11/13/14 are needed here)."""
    result = 0
    for _ in range(8):
        if b & 1:
            result ^= a
        b >>= 1
        a = _xtime(a)
    return result


def _decrypt_block(round_keys: list[bytes], block: bytes) -> bytes:
    state = [block[i] ^ round_keys[10][i] for i in range(16)]
    for rnd in range(9, -1, -1):
        state = [
            state[0], state[13], state[10], state[7],
            state[4], state[1], state[14], state[11],
            state[8], state[5], state[2], state[15],
            state[12], state[9], state[6], state[3],
        ]
        state = [_INV_SBOX[b] for b in state]
        state = [state[i] ^ round_keys[rnd][i] for i in range(16)]
        if rnd != 0:
            mixed = []
            for c in range(4):
                a = state[c * 4 : c * 4 + 4]
                mixed += [
                    _mul(a[0], 14) ^ _mul(a[1], 11) ^ _mul(a[2], 13) ^ _mul(a[3], 9),
                    _mul(a[0], 9) ^ _mul(a[1], 14) ^ _mul(a[2], 11) ^ _mul(a[3], 13),
                    _mul(a[0], 13) ^ _mul(a[1], 9) ^ _mul(a[2], 14) ^ _mul(a[3], 11),
                    _mul(a[0], 11) ^ _mul(a[1], 13) ^ _mul(a[2], 9) ^ _mul(a[3], 14),
                ]
            state = mixed
    return bytes(state)

def _xor(a: bytes, b: bytes) -> bytes:
    return bytes(x ^ y for x, y in zip(a, b))


def _sector_tweak(sector: int) -> bytes:
    """libnx's `is_nintendo` tweak: the sector number big-endian in the *upper* half, taken as the
    tweak directly (it is not pre-encrypted -- that happens per 16-byte block, as XTS defines).
    Found by sweeping the plausible conventions against a real package: the standard
    little-endian-in-the-lower-half form decrypts to noise."""
    return b"\x00" * 8 + struct.pack(">Q", sector)


def aes_xts_decrypt(data: bytes, key: bytes, sector_size: int = 0x200) -> bytes:
    """AES-128-XTS over an NCA header, the way libnx does it with `is_nintendo = true`:

      * the tweak for sector s is AES-encrypted with the tweak key after the sector number is
        written big-endian into its *upper* half (libnx: "Nintendo uses big endian
        tweak-from-sector, despite little endian gf multiplication");
      * within a sector each block advances the tweak by multiplying by x in GF(2^128)
        (little-endian, 0x87 reduction) -- not by adding one, which was the bug this tool existed
        to find.
    """
    data_key, tweak_key = key[:16], key[16:32]
    round_keys = _expand_key(data_key)
    tweak_keys = _expand_key(tweak_key)
    out = bytearray()
    for pos in range(0, len(data), sector_size):
        chunk = data[pos : pos + sector_size]
        tweak = _encrypt_block(tweak_keys, b"\x00" * 8 + struct.pack(">Q", pos // sector_size))
        for block in range(0, len(chunk), 16):
            piece = _xor(chunk[block : block + 16], tweak)
            out += _xor(_decrypt_block(round_keys, piece), tweak)
            tweak = _incr_tweak(tweak)
    return bytes(out)


def _incr_tweak(tweak: bytes) -> bytes:
    """Multiply the tweak by x in GF(2^128), little-endian, with the 0x87 reduction."""
    value = int.from_bytes(tweak, "little")
    carry = value >> 127
    value = (value << 1) & ((1 << 128) - 1)
    if carry:
        value ^= 0x87
    return value.to_bytes(16, "little")


def aes_ctr_decrypt(data: bytes, key: bytes, media_offset: int) -> bytes:
    """AES-128-CTR as the console stores NCA sections: counter[8:16] = bswap64(offset >> 4)."""
    round_keys = _expand_key(key)
    counter = bytearray(16)
    counter[8:16] = struct.pack(">Q", media_offset >> 4)
    out = bytearray()
    for pos in range(0, len(data), 16):
        keystream = _encrypt_block(round_keys, bytes(counter))
        out += _xor(data[pos : pos + 16], keystream)
        for i in range(15, 7, -1):
            counter[i] = (counter[i] + 1) & 0xFF
            if counter[i]:
                break
    return bytes(out)


# ---------------------------------------------------------------------------------------------


def read_keys(directory: pathlib.Path) -> dict[str, bytes]:
    keys: dict[str, bytes] = {}
    for name in ("prod.keys", "title.keys"):
        path = directory / name
        if not path.is_file():
            continue
        for line in path.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            try:
                keys[key.strip()] = bytes.fromhex(value.strip())
            except ValueError:
                pass
    return keys


def read_at(fd, offset: int, size: int) -> bytes:
    fd.seek(offset)
    data = fd.read(size)
    if len(data) != size:
        raise SystemExit(f"short read at {offset}: wanted {size}, got {len(data)}")
    return data


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("nsp", type=pathlib.Path)
    parser.add_argument("--keys", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent.parent.parent.parent
                        / "SDcard" / "switch")
    args = parser.parse_args()

    keys = read_keys(args.keys)
    if "header_key" not in keys:
        print(f"no header_key in {args.keys}", file=sys.stderr)
        return 1

    with args.nsp.open("rb") as fd:
        size = args.nsp.stat().st_size
        print(f"{args.nsp.name}: {size} bytes")
        head = read_at(fd, 0, 0x40)
        if head[:4] != b"PFS0":
            print(f"not a PFS0 container (magic {head[:4]!r})")
            return 1
        files, strings_size = struct.unpack_from("<II", head, 4)
        table = read_at(fd, 0x10, files * 0x18)
        strings = read_at(fd, 0x10 + files * 0x18, strings_size)
        data_start = 0x10 + files * 0x18 + strings_size
        entries = []
        for i in range(files):
            offset, entry_size, name_offset = struct.unpack_from("<QQI", table, i * 0x18)
            name = strings[name_offset : strings.index(b"\x00", name_offset)].decode()
            entries.append((name, data_start + offset, entry_size))
        print(f"  {files} entries, data at 0x{data_start:x}")
        for name, offset, entry_size in entries:
            print(f"    {name}  offset=0x{offset:x} size={entry_size}")

        meta = next((e for e in entries if e[0].endswith(".cnmt.nca")), None)
        if meta is None:
            print("no .cnmt.nca entry")
            return 1
        _, meta_offset, meta_size = meta

        encrypted = read_at(fd, meta_offset, 0xC00)
        header = aes_xts_decrypt(encrypted, keys["header_key"])
        magic = header[0x200:0x204]
        print(f"  meta NCA header magic: {magic!r} (want b'NCA3')")
        if magic != b"NCA3":
            return 1
        distribution, content_type = header[0x204], header[0x205]
        key_gen = max(header[0x206], header[0x220])
        print(f"  distribution={distribution} content_type={content_type} keygen=0x{key_gen:x}"
              f" size={struct.unpack_from('<Q', header, 0x208)[0]}")
        section_start, section_end = struct.unpack_from("<II", header, 0x240)
        print(f"  section0: start=0x{section_start * 0x200:x} end=0x{section_end * 0x200:x}")
        for index in range(4):
            start, end = struct.unpack_from("<II", header, 0x240 + index * 0x10)
            print(f"    table[{index}] start=0x{start * 0x200:x} end=0x{end * 0x200:x}")

        section_offset = section_start * 0x200
        raw = read_at(fd, meta_offset + section_offset, 0x40)
        print(f"  section0 first bytes: {raw[:4]!r} (at 0x{section_offset:x} in the NCA)")
        if raw[:4] != b"PFS0":
            print("  -> section is not plaintext; trying the title key")
            # An update package's meta content is encrypted with the *patch* title key: the rights
            # id is <title id> followed by the content kind (000b = update).
            title_id = header[0x210:0x218].hex()
            for rights_id in (f"{title_id}000000000000000b", f"{title_id}0000000000000000"):
                title_key = keys.get(rights_id)
                if title_key:
                    print(f"  using title key for rights id {rights_id}")
                    plain = aes_ctr_decrypt(read_at(fd, meta_offset + section_offset, 0x200),
                                            title_key, section_offset)
                    print(f"  decrypted first bytes: {plain[:4]!r}")
                    break
            else:
                print("  no title key for this content")
                return 1
        else:
            plain = raw
        if plain[:4] != b"PFS0":
            print("  still not a PFS0 -- decryption is wrong somewhere")
            return 1
        files_in_section, strings_in_section = struct.unpack_from("<II", plain, 4)
        print(f"  meta section: {files_in_section} files, name table {strings_in_section} bytes")
        data_offset = 0x10 + files_in_section * 0x18 + strings_in_section
        print(f"  -> CNMT payload starts 0x{data_offset:x} into the section"
              f" (i.e. 0x{section_offset + data_offset:x} in the NCA)")
    print("done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
