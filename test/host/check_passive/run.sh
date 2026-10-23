#!/usr/bin/env bash
# Plants files in a scratch firmware tree and checks that check-passive.sh
# catches each forbidden pattern, accepts the allowed ones, and passes on the
# real firmware tree.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
CHECK="$ROOT/scripts/check-passive.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0
expect() {  # expect <exit code> <name> <file contents>
  local want="$1" name="$2" body="$3"
  rm -rf "$WORK/fw" && mkdir -p "$WORK/fw/components/x"
  printf '%s\n' "$body" > "$WORK/fw/components/x/planted.c"
  "$CHECK" "$WORK/fw" > "$WORK/out" 2>&1
  local got=$?
  if [ "$got" -eq "$want" ]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "check_passive: '$name' gave exit $got, expected $want"
    cat "$WORK/out"
  fi
}

expect 0 "clean file" 'int add(int a, int b) { return a + b; }'
expect 1 "BLE scan without passive" 'void s(void) { struct ble_gap_ext_disc_params p = {.itvl = 160}; ble_gap_ext_disc(0, 0, 0, 0, 0, 0, &p, NULL, cb, NULL); }'
expect 1 "legacy BLE scan without passive" 'void s(void) { struct ble_gap_disc_params p = {0}; ble_gap_disc(0, BLE_HS_FOREVER, &p, cb, NULL); }'
expect 0 "BLE scan with passive = 1" 'void s(void) { struct ble_gap_ext_disc_params p = {.passive = 1}; ble_gap_ext_disc(0, 0, 0, 0, 0, 0, &p, NULL, cb, NULL); }'
expect 1 "BLE scan with passive = 0" 'void s(void) { struct ble_gap_disc_params p = {.passive = 0}; ble_gap_disc(0, 0, &p, cb, NULL); }'
expect 0 "scan state queries" 'bool a(void) { if (ble_gap_disc_active()) { ble_gap_disc_cancel(); } return true; }'
expect 1 "BLE central connect" 'void c(void) { ble_gap_connect(0, &peer, 30000, NULL, cb, NULL); }'
expect 1 "BLE extended connect" 'void c(void) { ble_gap_ext_connect(0, &peer, 30000, 1, NULL, NULL, NULL, cb, NULL); }'
expect 1 "ESP-NOW" 'void n(void) { esp_now_send(peer, data, 4); }'
expect 1 "raw 802.11 transmit" 'void t(void) { esp_wifi_80211_tx(WIFI_IF_STA, buf, 24, false); }'
expect 1 "active Wi-Fi scan" 'void t(void) { esp_wifi_scan_start(&cfg, false); }'
expect 1 "station mode" 'void t(void) { esp_wifi_set_mode(WIFI_MODE_STA); }'
expect 1 "deauthentication outside the parser" 'void d(void) { send_deauth(); }'

"$CHECK" > "$WORK/out" 2>&1
if [ $? -eq 0 ]; then
  pass=$((pass + 1))
else
  fail=$((fail + 1))
  echo "check_passive: the real firmware tree fails"
  cat "$WORK/out"
fi

if [ "$fail" -ne 0 ]; then
  echo "check_passive: $fail of $((pass + fail)) cases FAILED"
  exit 1
fi
echo "check_passive: OK ($pass cases)"
