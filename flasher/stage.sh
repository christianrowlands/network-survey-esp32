#!/usr/bin/env bash
# Assembles the web installer into a folder ready for static hosting: the page
# from flasher/ plus, per board, the manifest and merged image from
# <images>/<board>/: dist/<board>/ as written by scripts/build-all-targets.sh,
# or another folder of the same shape (flasher/stage-release.sh uses one).
#
# Usage: flasher/stage.sh [--from images-dir] [out-dir]   (default: dist, dist/flasher)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/flasher"
DIST="$ROOT/dist"
FROM="$DIST"
if [ "${1:-}" = "--from" ]; then
  [ -d "${2:-}" ] || { echo "--from needs an existing folder" >&2; exit 2; }
  FROM="$(cd "$2" && pwd)"
  shift 2
fi
OUT="${1:-$DIST/flasher}"

BOARDS="xiao-esp32c5 esp32c5-devkitc1 esp32c6-devkitc1 esp32s3-devkitc1 esp32c3-devkitm1"

mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
case "$OUT" in
  "$ROOT" | "$SRC" | "$DIST" | "$FROM" | /) echo "refusing to replace $OUT" >&2; exit 1 ;;
esac
rm -rf "$OUT"
mkdir -p "$OUT"
cp "$SRC/index.html" "$SRC"/*.css "$OUT/"
cp -R "$SRC/js" "$SRC/fonts" "$SRC/img" "$OUT/"
# Serve the folder as is on GitHub Pages (no Jekyll processing).
touch "$OUT/.nojekyll"

staged=0
for board in $BOARDS; do
  from="$FROM/$board"
  image="network-survey-board-$board.bin"
  if [ ! -f "$from/manifest.json" ] || [ ! -f "$from/$image" ]; then
    echo "skip $board: no image in $from (run scripts/build-all-targets.sh)" >&2
    continue
  fi
  mkdir -p "$OUT/firmware/$board"
  cp "$from/manifest.json" "$from/$image" "$OUT/firmware/$board/"
  staged=$((staged + 1))
done

echo "Staged the installer with $staged board(s) in $OUT"
