#!/usr/bin/env bash
# Fails if a tracked file contains text this public repository avoids: em
# dashes (use commas, colons or parentheses) and absolute home-directory paths
# from someone's machine.
#
# Usage: scripts/check-text.sh
set -uo pipefail
export LC_ALL=C

cd "$(dirname "$0")/.." || exit 2

FAIL=0
scan() {
  local what="$1" pattern="$2" hits
  hits=$(git ls-files -z | xargs -0 grep -H -n -I -E -- "$pattern" 2>/dev/null)
  if [ -n "$hits" ]; then
    echo "FAIL [$what]"
    echo "$hits" | head -50
    echo
    FAIL=1
  fi
}

scan "em dash" $'\xe2\x80\x94'
scan "absolute home path" '/(Users|home)/[A-Za-z]'

if [ $FAIL -eq 0 ]; then
  echo "check-text: clean"
fi
exit $FAIL
