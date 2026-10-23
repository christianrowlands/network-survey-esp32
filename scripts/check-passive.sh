#!/usr/bin/env bash
# Fails if the firmware contains code that could transmit 802.11 frames, scan
# actively, connect to other devices or send ESP-NOW. The board is a passive
# receiver (docs/protocol.md). Key handshakes are kept out of raw capture by
# raw_data_keep (components/sniffer/raw_frame.c), which the host tests cover.
#
# Usage: scripts/check-passive.sh [firmware-dir]   (default: this repo's firmware/)
# test/host/check_passive runs it against planted files.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${1:-$ROOT/firmware}" || exit 2

# Source we own: skip build output and downloaded components.
FILES=$(find . \( -path ./build -o -path ./managed_components \) -prune -o \
  -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.txt' -o -name '*.cmake' \
             -o -name 'Kconfig*' -o -name 'sdkconfig*' -o -name '*.defaults' \) -print)

FAIL=0
check() {
  local what="$1" pattern="$2" exclude="${3:-}"
  local hits
  hits=$(echo "$FILES" | { if [ -n "$exclude" ]; then grep -v -E "$exclude"; else cat; fi; } |
    tr '\n' '\0' | xargs -0 grep -n -I -E -i -- "$pattern" 2>/dev/null)
  if [ -n "$hits" ]; then
    echo "FAIL [$what]"
    echo "$hits"
    echo
    FAIL=1
  fi
}

check "raw 802.11 transmit" 'esp_wifi_80211_tx|esp_wifi_tx_|ieee80211_raw_frame'
check "active Wi-Fi scan or connect" 'esp_wifi_scan_start|WIFI_SCAN_TYPE_ACTIVE|esp_wifi_connect[[:space:]]*\(|esp_wifi_set_mode[[:space:]]*\([[:space:]]*WIFI_MODE_(STA|AP|APSTA)'
check "active BLE scan" '\.passive[[:space:]]*=[[:space:]]*0'
check "BLE central connect" 'ble_gap_(ext_)?connect[[:space:]]*\('
check "ESP-NOW" 'esp_now_'

# Every file that starts a BLE scan must ask for a passive one (passive = 1);
# a zero (the default) means active scanning with scan requests.
SCANNERS=$(echo "$FILES" | tr '\n' '\0' | xargs -0 grep -l -I -E 'ble_gap_(ext_)?disc[[:space:]]*\(' 2>/dev/null)
for f in $SCANNERS; do
  if ! grep -q -E 'passive[[:space:]]*=[[:space:]]*1' "$f"; then
    echo "FAIL [BLE scan without passive = 1]"
    grep -n -E 'ble_gap_(ext_)?disc[[:space:]]*\(' "$f" | sed "s|^|$f:|"
    echo
    FAIL=1
  fi
done
# The 802.11 parser may name received deauthentication frames; nothing else may.
check "deauthentication" 'deauth|disassoc' '^\./components/dot11_parser/'

if [ $FAIL -eq 0 ]; then
  echo "check-passive: clean"
fi
exit $FAIL
