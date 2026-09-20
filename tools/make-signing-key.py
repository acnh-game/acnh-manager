#!/usr/bin/env python3
"""Generate the release-signing key pair (ECDSA P-256) and publish its public half.

The private key is the project's identity: every ACNH-Manager in the field trusts manifests
signed with it (the matching public key is compiled into the app as `data/agent_pubkey.bin`).
It lives outside this repository, in the local secrets directory (`~/.acnh/`), and is backed up:
never regenerate it casually -- a new key means no already-shipped app will accept new releases
until it is updated itself.

    python3 tools/make-signing-key.py --key ~/.acnh/acnh-manager-signing-key.pem
    python3 tools/sign-manifest.py --key ~/.acnh/acnh-manager-signing-key.pem

The public key is written to `data/agent_pubkey.bin` (committed); the private key is not, and
this tool refuses to overwrite an existing one unless --force says so.
"""

import argparse
import pathlib
import shutil
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_PUBKEY = REPO_ROOT / "data" / "agent_pubkey.bin"


def run(args: list[str]) -> bytes:
    result = subprocess.run(args, capture_output=True)
    if result.returncode != 0:
        sys.exit("error: %s\n%s" % (" ".join(args), result.stderr.decode("utf-8", "replace").strip()))
    return result.stdout


def show(path: pathlib.Path) -> str:
    """Path for messages: relative to the repository when it is inside it."""
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--key", type=pathlib.Path, required=True,
                        help="where the private key lives (keep it out of this repository)")
    parser.add_argument("--pubkey", type=pathlib.Path, default=DEFAULT_PUBKEY,
                        help="embedded public key written by this tool (default: %(default)s)")
    parser.add_argument("--force", action="store_true",
                        help="overwrite an existing private key (only when you really mean it)")
    args = parser.parse_args()

    if shutil.which("openssl") is None:
        return int(sys.exit("error: openssl not found in PATH") or 1)
    if args.key.exists() and not args.force:
        return int(sys.exit("error: %s already exists (use --force to replace it)" % args.key) or 1)

    args.key.parent.mkdir(parents=True, exist_ok=True)
    run(["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(args.key)])
    args.key.chmod(0o600)
    pem = run(["openssl", "ec", "-in", str(args.key), "-pubout"])
    args.pubkey.parent.mkdir(parents=True, exist_ok=True)
    args.pubkey.write_bytes(pem)

    print("private key: %s (keep it safe, never commit it)" % args.key)
    print("public key : %s (%d B, embedded into the NRO at build time)"
          % (show(args.pubkey), len(pem)))
    print("next       : python3 tools/sign-manifest.py --key %s" % args.key)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
