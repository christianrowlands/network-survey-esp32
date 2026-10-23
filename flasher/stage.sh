#!/usr/bin/env bash
# Assembles the web installer into a folder ready for static hosting: the page
# from flasher/ plus, per board, the esp-web-tools manifest and merged image
# from dist/<board>/ (written by scripts/build-all-targets.sh).
#
# Usage: flasher/stage.sh [out-dir]   (default: dist/flasher)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/flasher"
DIST="$ROOT/dist"
OUT="${1:-$DIST/flasher}"

BOARDS="xiao-esp32c5 esp32c5-devkitc1 esp32c6-devkitc1 esp32s3-devkitc1 esp32c3-devkitm1"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
case "$OUT" in
  "$ROOT" | "$SRC" | "$DIST" | /) echo "refusing to replace $OUT" >&2; exit 1 ;;
esac
rm -rf "$OUT"
mkdir -p "$OUT/img"
cp "$SRC/index.html" "$SRC/style.css" "$SRC/app.js" "$OUT/"
cp "$SRC"/img/* "$OUT/img/"
# Serve the folder as is on GitHub Pages (no Jekyll processing).
touch "$OUT/.nojekyll"

staged=0
for board in $BOARDS; do
  from="$DIST/$board"
  image="network-survey-board-$board.bin"
  if [ ! -f "$from/manifest.json" ] || [ ! -f "$from/$image" ]; then
    echo "skip $board: no build in dist/$board (run scripts/build-all-targets.sh)" >&2
    continue
  fi
  mkdir -p "$OUT/firmware/$board"
  cp "$from/manifest.json" "$from/$image" "$OUT/firmware/$board/"
  staged=$((staged + 1))
done

echo "Staged the installer with $staged board(s) in $OUT"
