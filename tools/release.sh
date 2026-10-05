#!/usr/bin/env bash
# Tag and push a release; the "release" GitHub workflow then builds macOS, Windows and Linux packages and publishes them.
#   tools/release.sh v0.2.0          release          tools/release.sh v0.2.0-rc1   pre-release (any -suffix)
#   --yes skips the confirmation; --any-branch allows tagging a branch other than main.
set -euo pipefail
cd "$(dirname "$0")/.."

TAG=; YES=0; ANY=0
for a in "$@"; do
  case $a in
    --yes) YES=1 ;;
    --any-branch) ANY=1 ;;
    -*) echo "unknown option $a" >&2; exit 2 ;;
    *) TAG=$a ;;
  esac
done
[ -n "$TAG" ] || { echo "usage: tools/release.sh vX.Y.Z[-suffix] [--yes] [--any-branch]" >&2; exit 2; }
[[ $TAG =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] || { echo "error: '$TAG' is not vMAJOR.MINOR.PATCH[-suffix]" >&2; exit 2; }

git diff --quiet && git diff --cached --quiet || { echo "error: working tree has uncommitted changes" >&2; exit 1; }
BRANCH=$(git branch --show-current)
if [ "$ANY" = 0 ] && [ "$BRANCH" != main ]; then echo "error: on '$BRANCH', not main (use --any-branch to override)" >&2; exit 1; fi
git fetch -q origin --tags
if git rev-parse -q --verify "refs/tags/$TAG" >/dev/null; then echo "error: tag $TAG already exists" >&2; exit 1; fi
if [ "$(git rev-parse HEAD)" != "$(git rev-parse "origin/$BRANCH" 2>/dev/null || echo none)" ]; then
  echo "error: HEAD is not the same as origin/$BRANCH; push or pull first" >&2; exit 1
fi

KIND=release; [[ $TAG == *-* ]] && KIND=pre-release
echo "About to tag $(git rev-parse --short HEAD) ($BRANCH) as $TAG ($KIND) and push it to origin."
if [ "$YES" = 0 ]; then read -r -p "Continue? [y/N] " ans; [ "$ans" = y ] || { echo aborted; exit 1; }; fi
git tag -a "$TAG" -m "Snatcher port $TAG"
git push origin "$TAG"
echo "pushed $TAG: watch the build at https://github.com/bgyss/snatcher-port/actions"
