#!/usr/bin/env bash
# Generates the nanopb C code for proto/ns_board.proto into
# firmware/components/ns_proto/. The generated files are committed.
#
# Usage: scripts/gen-proto.sh [--check]
#   --check  generate into a temp dir and fail if the committed files differ.
#
# Needs protoc on PATH (brew install protobuf). The nanopb generator is pinned
# in scripts/requirements-proto.txt and installed into .venv/ (gitignored) on
# first run.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

CHECK=0
case "${1:-}" in
  --check) CHECK=1 ;;
  "") ;;
  *) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac

REQS="scripts/requirements-proto.txt"
VENV=".venv"
STAMP="$VENV/.requirements-proto"
OUT="firmware/components/ns_proto"

command -v protoc >/dev/null || { echo "protoc not found (brew install protobuf)" >&2; exit 1; }

if [ ! -x "$VENV/bin/protoc-gen-nanopb" ] || ! cmp -s "$REQS" "$STAMP"; then
  echo "Installing nanopb generator into $VENV"
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install --quiet --disable-pip-version-check -r "$REQS"
  cp "$REQS" "$STAMP"
fi

generate() {
  protoc \
    --plugin=protoc-gen-nanopb="$VENV/bin/protoc-gen-nanopb" \
    -I proto \
    --nanopb_opt=-Iproto \
    --nanopb_out="$1" \
    proto/ns_board.proto
}

if [ $CHECK -eq 1 ]; then
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  generate "$TMP"
  STALE=0
  for f in ns_board.pb.c ns_board.pb.h; do
    if ! cmp -s "$TMP/$f" "$OUT/$f"; then
      echo "stale: $OUT/$f (run scripts/gen-proto.sh)" >&2
      STALE=1
    fi
  done
  [ $STALE -eq 0 ] && echo "gen-proto: up to date"
  exit $STALE
fi

mkdir -p "$OUT"
generate "$OUT"
echo "Generated $OUT/ns_board.pb.c and $OUT/ns_board.pb.h"
