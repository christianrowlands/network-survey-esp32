#!/usr/bin/env bash
# Builds the firmware for every supported board and writes flashable images
# to dist/<board>/:
#   network-survey-board-<board>.bin      merged image, flash at offset 0
#   network-survey-board-<board>-app.bin  app image, for OTA updates
#   manifest.json                         esp-web-tools manifest
#
# Usage: scripts/build-all-targets.sh [board ...]   (default: all boards)
#        scripts/build-all-targets.sh --list        (print the board ids)
#        scripts/build-all-targets.sh --table       (print the board table below)
# Needs an activated ESP-IDF v6.0.1 environment (IDF_PATH set), except --list
# and --table.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FW="$ROOT/firmware"
DIST="$ROOT/dist"

# board id | IDF target | esp-web-tools chipFamily | display name
BOARDS="
esp32c5-devkitc1|esp32c5|ESP32-C5|ESP32-C5-DevKitC-1
xiao-esp32c5|esp32c5|ESP32-C5|XIAO ESP32-C5
esp32c6-devkitc1|esp32c6|ESP32-C6|ESP32-C6-DevKitC-1
esp32s3-devkitc1|esp32s3|ESP32-S3|ESP32-S3-DevKitC-1
esp32c3-devkitm1|esp32c3|ESP32-C3|ESP32-C3-DevKitM-1
"

if [ "${1:-}" = "--list" ]; then
  echo "$BOARDS" | cut -d'|' -f1 | sed '/^$/d'
  exit 0
fi
if [ "${1:-}" = "--table" ]; then
  echo "$BOARDS" | sed '/^$/d'
  exit 0
fi

[ -f "${IDF_PATH:-}/tools/idf.py" ] || { echo "IDF_PATH not set: activate ESP-IDF v6.0.1 first" >&2; exit 1; }
# Some IDF activation scripts define idf.py as a shell function, which a
# script does not inherit, so call it through the IDF Python directly.
PY=python
if [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && [ -x "$IDF_PYTHON_ENV_PATH/bin/python" ]; then
  PY="$IDF_PYTHON_ENV_PATH/bin/python"
fi
idf() { "$PY" "$IDF_PATH/tools/idf.py" "$@"; }

VERSION=$(sed -n 's/^set(PROJECT_VER "\(.*\)")/\1/p' "$FW/CMakeLists.txt")
[ -n "$VERSION" ] || { echo "PROJECT_VER not found in firmware/CMakeLists.txt" >&2; exit 1; }

WANTED="$*"
SUMMARY=""
BUILT=0

while IFS='|' read -r board target family name; do
  [ -z "$board" ] && continue
  if [ -n "$WANTED" ] && ! [[ " $WANTED " == *" $board "* ]]; then
    continue
  fi
  [ -f "$FW/boards/$board.defaults" ] || { echo "missing boards/$board.defaults" >&2; exit 1; }
  echo "=== $board ($target) ==="
  build="$FW/build/$board"
  # Regenerate sdkconfig so it always matches the defaults files.
  rm -f "$build/sdkconfig"
  idf -C "$FW" -B "$build" \
    -DIDF_TARGET="$target" \
    -DSDKCONFIG="$build/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="$FW/sdkconfig.defaults;$FW/boards/$board.defaults" \
    build

  out="$DIST/$board"
  image="network-survey-board-$board.bin"
  rm -rf "$out"
  mkdir -p "$out"
  (cd "$build" && "$PY" -m esptool --chip "$target" merge-bin -o "$out/$image" @flash_args)
  cp "$build/network_survey_board.bin" "$out/network-survey-board-$board-app.bin"
  "$ROOT/scripts/write-installer-manifest.sh" "$out" "$board" "$family" "$name" "$VERSION"
  SUMMARY="$SUMMARY$(printf '%-18s %-8s %9d bytes merged' "$board" "$target" "$(wc -c < "$out/$image")")"$'\n'
  BUILT=$((BUILT + 1))
done <<< "$BOARDS"

[ "$BUILT" -gt 0 ] || { echo "no board matched: $WANTED" >&2; exit 1; }
echo
echo "Built $BUILT board(s), version $VERSION, into dist/:"
printf '%s' "$SUMMARY"
