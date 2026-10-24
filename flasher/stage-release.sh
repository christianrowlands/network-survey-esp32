#!/usr/bin/env bash
# Stages the web installer around the full flash images of a published GitHub
# Release instead of a local build, so a change to the page can go live
# without a new firmware release. .github/workflows/installer.yml runs it.
# Needs the gh CLI, signed in (GH_TOKEN in CI).
#
# Usage: flasher/stage-release.sh [tag] [out-dir]
#   tag      a full release such as v0.1.0 (default: the latest full release)
#   out-dir  default: dist/flasher
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TAG="${1:-}"
OUT="${2:-$ROOT/dist/flasher}"

if [ -z "$TAG" ]; then
  TAG=$(gh release view --json tagName --jq .tagName)
fi
case "$TAG" in
  v[0-9]*) ;;
  *) echo "not a release tag: $TAG" >&2; exit 1 ;;
esac
# The installer offers full releases only, as release.yml does.
if [ "$(gh release view "$TAG" --json isPrerelease --jq .isPrerelease)" != "false" ]; then
  echo "$TAG is a pre-release; the installer only offers full releases" >&2
  exit 1
fi
VERSION="${TAG#v}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
gh release download "$TAG" --pattern "*-$VERSION-full-flash.bin" --dir "$WORK/download"

staged=0
while IFS='|' read -r board target family name; do
  src="$WORK/download/network-survey-board-$board-$VERSION-full-flash.bin"
  if [ ! -f "$src" ]; then
    echo "skip $board: $TAG has no full flash image for it" >&2
    continue
  fi
  mkdir -p "$WORK/images/$board"
  cp "$src" "$WORK/images/$board/network-survey-board-$board.bin"
  "$ROOT/scripts/write-installer-manifest.sh" "$WORK/images/$board" "$board" "$family" "$name" "$VERSION"
  staged=$((staged + 1))
done < <("$ROOT/scripts/build-all-targets.sh" --table)

[ "$staged" -gt 0 ] || { echo "$TAG has no full flash images" >&2; exit 1; }
echo "Using $staged image(s) from $TAG"
"$ROOT/flasher/stage.sh" --from "$WORK/images" "$OUT"
