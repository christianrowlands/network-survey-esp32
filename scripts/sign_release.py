#!/usr/bin/env python3
"""Signs the OTA app images in dist/ and writes the release manifest.

See docs/release-manifest.md for the output format. Called by
scripts/sign-release.sh, which picks a Python that has `cryptography`.
"""

import argparse
import base64
import hashlib
import json
import os
import re
import stat
import sys
from datetime import datetime, timezone
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = "firmware-manifest.json"
FORMAT = 1

# board id (as in build-all-targets.sh) -> BoardModel and Chip enum names
# (proto/ns_board.proto, without their prefixes) and the ESP image chip ID.
BOARDS = {
    "esp32c5-devkitc1": ("ESP32_C5_DEVKITC_1", "ESP32_C5", 0x17),
    "xiao-esp32c5": ("XIAO_ESP32_C5", "ESP32_C5", 0x17),
    "esp32c6-devkitc1": ("ESP32_C6_GENERIC", "ESP32_C6", 0x0D),
    "esp32s3-devkitc1": ("ESP32_S3_GENERIC", "ESP32_S3", 0x09),
    "esp32c3-devkitm1": ("ESP32_C3_GENERIC", "ESP32_C3", 0x05),
}


def fail(message):
    print(f"sign-release: {message}", file=sys.stderr)
    sys.exit(1)


def load_private_key():
    """NS_SIGNING_KEY (PEM text, or base64 of the raw 32-byte seed) or
    NS_SIGNING_KEY_FILE (path to a PEM file)."""
    text = os.environ.get("NS_SIGNING_KEY", "").strip()
    path = os.environ.get("NS_SIGNING_KEY_FILE", "").strip()
    if not text and path:
        mode = Path(path).stat().st_mode
        if mode & (stat.S_IRWXG | stat.S_IRWXO):
            print(f"sign-release: warning: {path} is readable by others (chmod 600 it)", file=sys.stderr)
        text = Path(path).read_text().strip()
    if not text:
        fail("no key: set NS_SIGNING_KEY or NS_SIGNING_KEY_FILE")
    if text.startswith("-----BEGIN"):
        key = serialization.load_pem_private_key(text.encode(), password=None)
        if not isinstance(key, Ed25519PrivateKey):
            fail("the key is not an Ed25519 key")
        return key
    try:
        seed = base64.b64decode(text, validate=True)
    except ValueError:
        fail("NS_SIGNING_KEY is neither PEM nor base64")
    if len(seed) != 32:
        fail("a raw Ed25519 key must be 32 bytes")
    return Ed25519PrivateKey.from_private_bytes(seed)


def load_public_key(path):
    """A file holding the base64 raw 32-byte public key; # lines are comments."""
    lines = [l.strip() for l in Path(path).read_text().splitlines()]
    data = base64.b64decode("".join(l for l in lines if l and not l.startswith("#")))
    if len(data) != 32:
        fail(f"{path}: not a 32-byte Ed25519 public key")
    return Ed25519PublicKey.from_public_bytes(data)


def raw_public(key):
    return key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def project_version():
    text = (ROOT / "firmware/CMakeLists.txt").read_text()
    match = re.search(r'set\(PROJECT_VER "([^"]+)"\)', text)
    if not match:
        fail("PROJECT_VER not found in firmware/CMakeLists.txt")
    return match.group(1)


def protocol_version():
    text = (ROOT / "proto/ns_board.proto").read_text()
    major = re.search(r"PROTOCOL_VERSION_MAJOR = (\d+);", text)
    minor = re.search(r"PROTOCOL_VERSION_MINOR = (\d+);", text)
    if not major or not minor:
        fail("ProtocolVersion not found in proto/ns_board.proto")
    return int(major.group(1)), int(minor.group(1))


def released_time():
    """UTC now, or SOURCE_DATE_EPOCH for reproducible output."""
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    when = datetime.fromtimestamp(int(epoch), timezone.utc) if epoch else datetime.now(timezone.utc)
    return when.strftime("%Y-%m-%dT%H:%M:%SZ")


def parse_version(text):
    match = re.fullmatch(r"v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:[-+].*)?", text or "")
    return tuple(int(part) for part in match.groups()) if match else None


def image_facts(data):
    """Chip ID and app version from an ESP-IDF app image header."""
    if len(data) < 80 or data[0] != 0xE9:
        return None, None
    chip_id = int.from_bytes(data[12:14], "little")
    if int.from_bytes(data[32:36], "little") != 0xABCD5432:
        return chip_id, None
    return chip_id, data[48:80].split(b"\0", 1)[0].decode(errors="replace")


def sign(args):
    if os.environ.get("CI") and not args.public_key:
        fail("in CI, pass --public-key so a wrong signing secret is caught before upload")
    key = load_private_key()
    version = project_version()
    major, minor = protocol_version()
    if args.min_version:
        floor, current = parse_version(args.min_version), parse_version(version)
        if not floor or not current or floor > current:
            fail(f"--min-version {args.min_version} must be a version no higher than {version}")
    dist = Path(args.dist)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    # Never ship leftovers from an earlier run.
    for old in out.iterdir():
        if old.is_file() and (old.suffix in (".bin", ".sig") or old.name == MANIFEST):
            old.unlink()

    images = []
    for board, (model, chip, chip_id) in BOARDS.items():
        source = dist / board / f"network-survey-board-{board}-app.bin"
        if not source.exists():
            print(f"skip {board}: {source} not built")
            continue
        data = source.read_bytes()
        found_chip, found_version = image_facts(data)
        if found_chip != chip_id:
            fail(f"{source}: not an app image for {chip}")
        if found_version != version:
            fail(f"{source}: built as {found_version!r}, PROJECT_VER is {version!r}; rebuild first")
        name = f"network-survey-board-{board}-{version}.bin"
        signature = key.sign(data)
        (out / name).write_bytes(data)  # Exactly the bytes that were checked and signed.
        (out / f"{name}.sig").write_text(base64.b64encode(signature).decode() + "\n")
        images.append({
            "board": board,
            "board_model": model,
            "chip": chip,
            "file": name,
            "url": args.base_url + name if args.base_url else name,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "signature": base64.b64encode(signature).decode(),
        })
        print(f"signed {name} ({len(data)} bytes)")
    if not images:
        fail(f"no app images in {dist}; run scripts/build-all-targets.sh first")

    manifest = {
        "format": FORMAT,
        "name": "NS Scout firmware",
        "version": version,
        "min_version": args.min_version or "0.0.0",
        "protocol_major": major,
        "protocol_minor": minor,
        "released": released_time(),
        "images": images,
    }
    body = (json.dumps(manifest, indent=2) + "\n").encode()
    (out / MANIFEST).write_bytes(body)
    (out / f"{MANIFEST}.sig").write_text(base64.b64encode(key.sign(body)).decode() + "\n")
    print(f"wrote {out / MANIFEST} for version {version}, {len(images)} image(s)")
    print(f"public key: {base64.b64encode(raw_public(key)).decode()}")
    if args.public_key:
        verify_dir(out, load_public_key(args.public_key))


def check_signature(public, signature_b64, data, what):
    try:
        public.verify(base64.b64decode(signature_b64, validate=True), data)
    except (InvalidSignature, ValueError):
        fail(f"{what}: signature does not match the public key")


def verify_dir(folder, public):
    folder = Path(folder)
    try:
        body = (folder / MANIFEST).read_bytes()
        check_signature(public, (folder / f"{MANIFEST}.sig").read_text().strip(), body, MANIFEST)
        manifest = json.loads(body)
        chips = {chip: chip_id for _, chip, chip_id in BOARDS.values()}
        for image in manifest["images"]:
            name = image["file"]
            if "/" in name or "\\" in name or name in ("", ".", ".."):
                fail(f"{name!r}: file names must not contain a path")
            data = (folder / name).read_bytes()
            if len(data) != image["size"] or hashlib.sha256(data).hexdigest() != image["sha256"]:
                fail(f"{name}: size or SHA-256 differs from the manifest")
            check_signature(public, image["signature"], data, name)
            found_chip, found_version = image_facts(data)
            if found_chip != chips.get(image["chip"]) or found_version != manifest["version"]:
                fail(f"{name}: image header does not match {image['chip']} {manifest['version']}")
    except (OSError, ValueError, KeyError, TypeError) as error:
        fail(f"cannot read the release in {folder}: {error}")
    print(f"verified {MANIFEST} and {len(manifest['images'])} image(s)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dist", default=str(ROOT / "dist"), help="build output (default: dist)")
    parser.add_argument("--out", default=str(ROOT / "dist/release"), help="release folder (default: dist/release)")
    parser.add_argument("--base-url", default=os.environ.get("NS_RELEASE_BASE_URL", ""),
                        help="prefix for image URLs; default: URLs relative to the manifest")
    parser.add_argument("--min-version", default=os.environ.get("NS_MIN_VERSION", ""),
                        help="oldest version still safe to install (default 0.0.0)")
    parser.add_argument("--public-key", help="after signing, verify against this public key file")
    parser.add_argument("--verify", metavar="DIR", help="only verify a release folder (needs --public-key)")
    args = parser.parse_args()
    if args.verify:
        if not args.public_key:
            fail("--verify needs --public-key")
        verify_dir(args.verify, load_public_key(args.public_key))
    else:
        sign(args)


if __name__ == "__main__":
    main()
