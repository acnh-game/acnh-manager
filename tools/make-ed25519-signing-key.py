#!/usr/bin/env python3
"""Generate the Ed25519 authorization key used by the Manager transition build.

The private key lives outside every repository at the canonical path
`~/.switch/acnh-signing-key.pem` and must be mode 0600.  This tool publishes only the raw
32-byte public key to `data/agent_ed25519_pubkey.bin`; the NRO embeds that file at build time.

This key is the future shared authorization identity described by the A/B/C architecture
whitepaper.  Do not casually replace it: existing transition Managers trust the public half
compiled into them.  The older P-256 Manager key remains separate during the migration window.
"""

import argparse
import os
import pathlib
import stat
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_KEY = pathlib.Path.home() / ".switch" / "acnh-signing-key.pem"
DEFAULT_PUBKEY = REPO_ROOT / "data" / "agent_ed25519_pubkey.bin"

# SubjectPublicKeyInfo prefix for an Ed25519 key.  DER is fixed-length: DER prefix + 32-byte
# raw key.  Refusing any other shape keeps shell/tooling assumptions from silently drifting.
ED25519_SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")
ED25519_PUBLIC_KEY_SIZE = 32


def run(args: list[str], input_bytes: bytes | None = None) -> subprocess.CompletedProcess:
    result = subprocess.run(args, input=input_bytes, capture_output=True)
    if result.returncode != 0:
        sys.exit("error: %s\n%s" % (
            " ".join(args), result.stderr.decode("utf-8", "replace").strip()))
    return result


def show(path: pathlib.Path) -> str:
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def inside_repo(path: pathlib.Path) -> bool:
    try:
        path.resolve().relative_to(REPO_ROOT)
        return True
    except ValueError:
        return False


def check_private_mode(path: pathlib.Path) -> None:
    mode = stat.S_IMODE(path.stat().st_mode)
    if mode & 0o077:
        sys.exit(f"error: {path} must not be group/world accessible (mode is {mode:03o})")


def raw_public_key(path: pathlib.Path) -> bytes:
    der = run(["openssl", "pkey", "-in", str(path), "-pubout", "-outform", "DER"]).stdout
    if len(der) != len(ED25519_SPKI_PREFIX) + ED25519_PUBLIC_KEY_SIZE:
        sys.exit("error: OpenSSL produced an unexpected Ed25519 public-key encoding")
    if not der.startswith(ED25519_SPKI_PREFIX):
        sys.exit("error: the key is not Ed25519 (unexpected SubjectPublicKeyInfo prefix)")
    return der[len(ED25519_SPKI_PREFIX):]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--key", type=pathlib.Path, default=DEFAULT_KEY,
                        help="Ed25519 private key (default: %(default)s)")
    parser.add_argument("--pubkey", type=pathlib.Path, default=DEFAULT_PUBKEY,
                        help="raw public key written for bin2s (default: %(default)s)")
    parser.add_argument("--force", action="store_true",
                        help="replace an existing private key; this changes the trust identity")
    args = parser.parse_args()

    if not args.key.is_absolute():
        args.key = pathlib.Path.cwd() / args.key
    if inside_repo(args.key):
        sys.exit("error: the Ed25519 private key must live outside the repository")
    if args.key.exists() and not args.force:
        check_private_mode(args.key)
        public_key = raw_public_key(args.key)
    else:
        args.key.parent.mkdir(parents=True, exist_ok=True)
        args.key.parent.chmod(0o700)
        previous_umask = os.umask(0o077)
        try:
            run(["openssl", "genpkey", "-algorithm", "ED25519", "-out", str(args.key)])
        finally:
            os.umask(previous_umask)
        args.key.chmod(0o600)
        public_key = raw_public_key(args.key)

    args.pubkey.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix="acnh-ed25519-pub-", dir=args.pubkey.parent,
                                     delete=False) as temporary:
        temporary.write(public_key)
        temporary_path = pathlib.Path(temporary.name)
    temporary_path.chmod(0o644)
    temporary_path.replace(args.pubkey)

    print(f"private key: {show(args.key)} (mode 0600, keep it outside the repository)")
    print(f"public key : {show(args.pubkey)} ({ED25519_PUBLIC_KEY_SIZE} B, embedded into the NRO)")
    print("next       : python3 tools/sign-manifest.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
