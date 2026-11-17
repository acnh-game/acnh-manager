#!/usr/bin/env python3
"""Dual-sign the release manifest for the P-256 -> Ed25519 migration window.

The app has no usable TLS trust store, so the manifest must authenticate itself.  During the
migration:

  * `agent-manifest.json.sig` remains a P-256/SHA-256 DER signature over the exact manifest
    bytes.  Old Managers still fetch exactly this file and can therefore upgrade to the
    transition build.
  * `agent-manifest.json.ed25519.sig` is a 64-byte Ed25519 signature over
    `"ACNH-MANAGER-MANIFEST-v1" NUL || exact manifest bytes`.  The transition Manager prefers
    it and falls back to the legacy signature only when the new one is unavailable or invalid.

Both private keys live outside the repository.  The public halves embedded in the NRO are
checked before anything is written, so a mismatch fails on the release machine instead of on
every player's console.
"""

import argparse
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "agent-manifest.json"
DEFAULT_P256_SIGNATURE = REPO_ROOT / "agent-manifest.json.sig"
DEFAULT_ED25519_SIGNATURE = REPO_ROOT / "agent-manifest.json.ed25519.sig"
DEFAULT_P256_PUBKEY = REPO_ROOT / "data" / "agent_pubkey.bin"
DEFAULT_ED25519_PUBKEY = REPO_ROOT / "data" / "agent_ed25519_pubkey.bin"
DEFAULT_P256_KEY = pathlib.Path.home() / ".switch" / "acnh-manager-signing-key.pem"
DEFAULT_ED25519_KEY = pathlib.Path.home() / ".switch" / "acnh-signing-key.pem"

ED25519_DOMAIN = b"ACNH-MANAGER-MANIFEST-v1\0"
ED25519_SPKI_PREFIX = bytes.fromhex("302a300506032b6570032100")
ED25519_PUBLIC_KEY_SIZE = 32
ED25519_SIGNATURE_SIZE = 64


def run(args: list[str], check_output: bool = True,
        input_bytes: bytes | None = None) -> subprocess.CompletedProcess:
    result = subprocess.run(args, input=input_bytes, capture_output=True)
    if check_output and result.returncode != 0:
        sys.exit("error: %s\n%s" % (
            " ".join(args), result.stderr.decode("utf-8", "replace").strip()))
    return result


def show(path: pathlib.Path) -> str:
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def require_private_key(path: pathlib.Path) -> None:
    if not path.is_file():
        sys.exit(f"error: {path} does not exist")
    if path.is_symlink():
        sys.exit(f"error: {path} must not be a symlink")
    info = path.stat()
    if info.st_uid != os.geteuid():
        sys.exit(f"error: {path} is not owned by the current user")
    mode = stat.S_IMODE(info.st_mode)
    if mode & 0o077:
        sys.exit(f"error: {path} must be mode 0600 (current {mode:03o})")


def p256_public_key_of(key: pathlib.Path) -> bytes:
    return run(["openssl", "ec", "-in", str(key), "-pubout"]).stdout


def ed25519_public_key_of(key: pathlib.Path) -> bytes:
    der = run(["openssl", "pkey", "-in", str(key), "-pubout", "-outform", "DER"]).stdout
    if len(der) != len(ED25519_SPKI_PREFIX) + ED25519_PUBLIC_KEY_SIZE:
        sys.exit("error: the Ed25519 public key has an unexpected DER length")
    if not der.startswith(ED25519_SPKI_PREFIX):
        sys.exit("error: the Ed25519 key has an unexpected SubjectPublicKeyInfo encoding")
    return der[len(ED25519_SPKI_PREFIX):]


def write_atomic(path: pathlib.Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(prefix=path.name + ".", dir=path.parent,
                                     delete=False) as temporary:
        temporary.write(data)
        temporary_path = pathlib.Path(temporary.name)
    temporary_path.chmod(0o644)
    temporary_path.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--key", "--p256-key", dest="p256_key", type=pathlib.Path,
                        default=DEFAULT_P256_KEY,
                        help="legacy P-256 private key (default: %(default)s)")
    parser.add_argument("--ed25519-key", type=pathlib.Path, default=DEFAULT_ED25519_KEY,
                        help="Ed25519 private key (default: %(default)s)")
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--signature", type=pathlib.Path, default=None,
                        help="legacy P-256 signature output (default: manifest name + .sig)")
    parser.add_argument("--ed25519-signature", type=pathlib.Path,
                        default=None,
                        help="Ed25519 signature output "
                             "(default: manifest name + .ed25519.sig)")
    parser.add_argument("--pubkey", type=pathlib.Path, default=DEFAULT_P256_PUBKEY,
                        help="embedded legacy P-256 public key (default: %(default)s)")
    parser.add_argument("--ed25519-pubkey", type=pathlib.Path,
                        default=DEFAULT_ED25519_PUBKEY,
                        help="embedded raw Ed25519 public key (default: %(default)s)")
    args = parser.parse_args()

    if args.signature is None:
        args.signature = (DEFAULT_P256_SIGNATURE
                          if args.manifest == DEFAULT_MANIFEST
                          else args.manifest.with_name(args.manifest.name + ".sig"))
    if args.ed25519_signature is None:
        args.ed25519_signature = (
            DEFAULT_ED25519_SIGNATURE
            if args.manifest == DEFAULT_MANIFEST
            else args.manifest.with_name(args.manifest.name + ".ed25519.sig"))

    if shutil.which("openssl") is None:
        sys.exit("error: openssl not found in PATH")
    if not args.manifest.is_file():
        sys.exit(f"error: {args.manifest} does not exist")
    require_private_key(args.p256_key)
    require_private_key(args.ed25519_key)
    if not args.pubkey.is_file():
        sys.exit(f"error: {args.pubkey} is missing; the app would reject the legacy signature")
    if not args.ed25519_pubkey.is_file():
        sys.exit(f"error: {args.ed25519_pubkey} is missing; the app would reject Ed25519")

    embedded_p256 = args.pubkey.read_bytes().strip()
    if p256_public_key_of(args.p256_key).strip() != embedded_p256:
        sys.exit(
            f"error: the P-256 key does not match {args.pubkey}; the app would reject every "
            "manifest signed with it")

    embedded_ed25519 = args.ed25519_pubkey.read_bytes()
    if len(embedded_ed25519) != ED25519_PUBLIC_KEY_SIZE:
        sys.exit(f"error: {args.ed25519_pubkey} must contain exactly 32 bytes")
    if ed25519_public_key_of(args.ed25519_key) != embedded_ed25519:
        sys.exit(
            f"error: the Ed25519 key does not match {args.ed25519_pubkey}; the app would "
            "reject every new-format manifest signed with it")

    manifest_bytes = args.manifest.read_bytes()
    ed25519_input = ED25519_DOMAIN + manifest_bytes

    with tempfile.TemporaryDirectory(prefix="acnh-manifest-sign-") as temporary_dir:
        temporary_root = pathlib.Path(temporary_dir)
        p256_signature = temporary_root / "manifest.p256.sig"
        ed25519_signature = temporary_root / "manifest.ed25519.sig"
        ed25519_public_pem = temporary_root / "ed25519-public.pem"
        ed25519_input_path = temporary_root / "manifest.ed25519.input"

        ed25519_input_path.write_bytes(ed25519_input)
        run(["openssl", "dgst", "-sha256", "-sign", str(args.p256_key),
             "-out", str(p256_signature), str(args.manifest)])
        ed_signature = run([
            "openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(args.ed25519_key),
            "-in", str(ed25519_input_path)
        ]).stdout
        if len(ed_signature) != ED25519_SIGNATURE_SIZE:
            sys.exit("error: OpenSSL produced an Ed25519 signature that is not 64 bytes")
        ed25519_signature.write_bytes(ed_signature)
        ed25519_public_pem.write_bytes(run([
            "openssl", "pkey", "-in", str(args.ed25519_key), "-pubout"
        ]).stdout)

        p256_verify = run([
            "openssl", "dgst", "-sha256", "-verify", str(args.pubkey),
            "-signature", str(p256_signature), str(args.manifest)
        ], check_output=False)
        if p256_verify.returncode != 0:
            sys.exit(f"error: the P-256 signature does not verify with {args.pubkey}")

        ed_verify = run([
            "openssl", "pkeyutl", "-verify", "-pubin", "-inkey", str(ed25519_public_pem),
            "-rawin", "-in", str(ed25519_input_path), "-sigfile", str(ed25519_signature)
        ], check_output=False)
        if ed_verify.returncode != 0:
            sys.exit(f"error: the Ed25519 signature does not verify with {args.ed25519_pubkey}")

        write_atomic(args.signature, p256_signature.read_bytes())
        write_atomic(args.ed25519_signature, ed25519_signature.read_bytes())

    print("signed %s" % show(args.manifest))
    print("  P-256   : %s (%d B), verified with %s"
          % (show(args.signature), args.signature.stat().st_size, show(args.pubkey)))
    print("  Ed25519 : %s (%d B), verified with %s"
          % (show(args.ed25519_signature), args.ed25519_signature.stat().st_size,
             show(args.ed25519_pubkey)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
