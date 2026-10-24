#!/usr/bin/env bash
# Writes the web installer's manifest.json for one board, in the ESP Web Tools
# format the page reads. The image sits next to it as
# network-survey-board-<board>.bin, to be flashed at offset 0.
#
# Usage: scripts/write-installer-manifest.sh <dir> <board id> <chipFamily> <display name> <version>
set -euo pipefail

[ $# -eq 5 ] || { echo "usage: $0 <dir> <board id> <chipFamily> <display name> <version>" >&2; exit 2; }
DIR="$1" BOARD="$2" FAMILY="$3" NAME="$4" VERSION="$5"

cat > "$DIR/manifest.json" <<JSON
{
  "name": "NS Scout ($NAME)",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "$FAMILY",
      "parts": [{ "path": "network-survey-board-$BOARD.bin", "offset": 0 }]
    }
  ]
}
JSON
