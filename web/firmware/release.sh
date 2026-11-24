#!/bin/sh
#
#  release.sh mimuz-link: make a release commit with the firmware and tag it
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/22 : Start Writing
#
# The tag v<version> goes on a commit that already holds the firmware built for that version:
#   1. "npm version <version> --no-git-tag-version": package.json, and through the "version" script the firmware
#      sources (MIMUZ_LINK_VERSION), library.properties and web/package.json
#   2. commit "Version <version>"
#   3. build the bridge and client firmware from that commit (web/firmware/build.sh)
#   4. commit "Firmware <version>" (web/www/firmware)
#   5. tag v<version> on that commit
# Nothing is pushed. Push the branch first and the tag last:
#   git push origin <branch> && git push origin v<version>
#
# usage: npm run release -- <version>      (e.g. npm run release -- 0.2.0)
set -e

NEW=$1
if [ -z "$NEW" ]; then
  echo "usage: npm run release -- <version>" >&2
  exit 2
fi
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

if [ -n "$(git status --porcelain)" ]; then
  echo "there are uncommitted changes; commit or stash them first" >&2
  exit 1
fi
if git rev-parse -q --verify "refs/tags/v$NEW" > /dev/null; then
  echo "tag v$NEW already exists" >&2
  exit 1
fi

npm version "$NEW" --no-git-tag-version
git add package.json
git commit -m "Version $NEW"

MIMUZ_RELEASE=1 sh ./web/firmware/build.sh
git add web/www/firmware
git commit -m "Firmware $NEW"

git tag -a "v$NEW" -m "mimuz-link $NEW"
BRANCH=$(git rev-parse --abbrev-ref HEAD)
echo
echo "release v$NEW is ready (not pushed). Push the branch first and the tag last:"
echo "  git push origin $BRANCH && git push origin v$NEW"
