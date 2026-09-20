#!/usr/bin/env python3
"""Sign the release manifest, so an app that cannot verify TLS can still verify *content*.

Why this exists: TLS certificate verification is off on the Switch (no trust store reaches
libcurl + mbedTLS), so the manifest -- and with it the hashes of every file the app downloads --
would otherwise be whatever the network hands over.  The app therefore carries a public key and
refuses any manifest whose signature does not match.

    python3 tools/sign-manifest.py                 # uses ~/.acnh/acnh-manager-signing-key.pem
    python3 tools/sign-manifest.py --key <other key>

It signs `agent-manifest.json` (the copy at the repository root, which is what the app fetches)
into `agent-manifest.json.sig` (the app appends `.sig` to the URL it read the manifest from), and
refuses to do so when the signing key does not match the public key embedded in the app
(`data/agent_pubkey.bin`) -- that mismatch is exactly the failure the app would report to every
player, so it is caught here instead.

Signature format: the DER encoding `openssl dgst -sha256 -sign` produces, which is what
`net::VerifySignature()` (mbedTLS) expects.
"""

import argparse
import pathlib
import shutil
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "agent-manifest.json"
DEFAULT_SIGNATURE = REPO_ROOT / "agent-manifest.json.sig"
DEFAULT_PUBKEY = REPO_ROOT / "data" / "agent_pubkey.bin"
DEFAULT_KEY = pathlib.Path.home() / ".acnh" / "acnh-manager-signing-key.pem"


def run(args: list[str], check_output: bool = True) -> subprocess.CompletedProcess:
    result = subprocess.run(args, capture_output=True)
    if check_output and result.returncode != 0:
        sys.exit("error: %s\n%s" % (" ".join(args), result.stderr.decode("utf-8", "replace").strip()))
    return result


def public_key_of(key: pathlib.Path) -> bytes:
    return run(["openssl", "ec", "-in", str(key), "-pubout"]).stdout


def show(path: pathlib.Path) -> str:
    """Path for messages: relative to the repository when it is inside it."""
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--key", type=pathlib.Path, default=DEFAULT_KEY,
                        help="release signing key (default: %(default)s)")
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--signature", type=pathlib.Path, default=DEFAULT_SIGNATURE)
    parser.add_argument("--pubkey", type=pathlib.Path, default=DEFAULT_PUBKEY)
    args = parser.parse_args()

    if shutil.which("openssl") is None:
        return int(sys.exit("error: openssl not found in PATH") or 1)
    for path in (args.key, args.manifest):
        if not path.is_file():
            return int(sys.exit("error: %s does not exist" % path) or 1)

    embedded = args.pubkey.read_bytes() if args.pubkey.is_file() else b""
    if embedded and public_key_of(args.key).strip() != embedded.strip():
        return int(sys.exit(
            "error: this key does not match the public key embedded in the app (%s);\n"
            "       the app would reject every manifest signed with it" % args.pubkey) or 1)
    if not embedded:
        print("warning: %s is missing; the app will refuse every manifest until it is built in"
              % args.pubkey)

    run(["openssl", "dgst", "-sha256", "-sign", str(args.key), "-out", str(args.signature),
         str(args.manifest)])
    # Read the signature back the same way the app will: if this fails, the app would fail too.
    verify = run(["openssl", "dgst", "-sha256", "-verify", str(args.pubkey),
                  "-signature", str(args.signature), str(args.manifest)], check_output=False)
    if verify.returncode != 0:
        return int(sys.exit("error: the signature does not verify with %s" % args.pubkey) or 1)

    print("signed %s (%d B) -> %s (%d B)"
          % (show(args.manifest), args.manifest.stat().st_size,
             show(args.signature), args.signature.stat().st_size))
    print("verified with %s" % show(args.pubkey))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
