#!/usr/bin/env bash
# Signs the OTA app images built by build-all-targets.sh with Ed25519 and
# writes dist/release/: the images, a .sig per image, firmware-manifest.json
# and firmware-manifest.json.sig (format: docs/release-manifest.md).
#
# Key (never commit it):
#   NS_SIGNING_KEY       PEM text, or base64 of the raw 32-byte private key (CI secret)
#   NS_SIGNING_KEY_FILE  path to a PEM file (local testing)
#
# Usage: scripts/sign-release.sh [--min-version V] [--base-url URL] [--public-key FILE] [--dist DIR] [--out DIR]
#        scripts/sign-release.sh --verify DIR --public-key FILE
# --public-key checks the result against a public key, for example
# scripts/keys/dev-signing-key.pub (DEV, test only).
#
# Needs Python 3 with the `cryptography` package. The ESP-IDF Python
# environment has it, so an activated IDF environment is enough.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY=python3
if [ -x "${IDF_PYTHON_ENV_PATH:-}/bin/python" ]; then
  PY="$IDF_PYTHON_ENV_PATH/bin/python"
fi
"$PY" -c "import cryptography" 2>/dev/null || {
  echo "sign-release: $PY lacks the cryptography package (activate ESP-IDF or pip install cryptography)" >&2
  exit 1
}
exec "$PY" "$ROOT/scripts/sign_release.py" "$@"
