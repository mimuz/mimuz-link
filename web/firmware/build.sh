#!/bin/sh
#
#  build.sh mimuz-link web console: build the firmware offered by the firmware uploader
#
#  ©2026 by D.F.Mac.@TripArts Music
#  Programmed by Claude Code
#
#  ver. history
#
#  - 2026/11/22 : Start Writing
#  - 2026/11/22 : Version from the top package.json (checked against the firmware); run by "npm run build"
#  - 2026/11/22 : Record the source commit; quiet about the missing tag when run by release.sh
#  - 2026/11/23 : Also the basic client example (www/firmware/basic)
#
# Builds the bridge (examples/bridge/s3_bridge), the client test device (examples/client/c6_test_device, "client") and
# the basic client example (examples/client/basic, "basic") with arduino-cli and
# puts, for each, the bootloader, the partition table, boot_app0 and the application into www/firmware/<name>/ with
# a manifest.json for ESP Web Tools. The parts are written at the same offsets as "arduino-cli upload"; the merged
# image is not used because it covers the whole flash and would also overwrite the settings (NVS at 0x9000-0xDFFF:
# pairing, label, ...).
#
# The version comes from the top package.json (kept with the git tag v<version>; "npm version" changes both and
# copies it into the firmware sources). The build stops if the sources do not carry the same version, and warns if
# there are uncommitted changes or (except in a release, see release.sh, where the tag comes after the firmware
# commit) the commit is not the one tagged v<version>. Each manifest records the source commit ("rev").
#
# usage: npm run build   (or web/firmware/build.sh; needs arduino-cli with the esp32 core, see the README)
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/web/www/firmware"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

VERSION=$(node -e 'console.log(JSON.parse(require("fs").readFileSync(process.argv[1], "utf8")).version)' "$ROOT/package.json")
node "$ROOT/web/firmware/sync-version.mjs" --check || {
  echo "the version in the sources differs from package.json ($VERSION); run: node web/firmware/sync-version.mjs" >&2
  exit 1
}
REV=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
if [ -n "$(git -C "$ROOT" status --porcelain -- . ':!web/www/firmware' 2>/dev/null)" ]; then
  REV="$REV-dirty"
  echo "warning: building with uncommitted changes ($REV); a release is built by: npm run release -- <version>" >&2
elif [ -z "$MIMUZ_RELEASE" ] && [ "$(git -C "$ROOT" describe --tags --exact-match 2>/dev/null)" != "v$VERSION" ]; then
  echo "warning: $REV is not the commit tagged v$VERSION (fine for testing; a release is built by: npm run release -- <version>)" >&2
fi
BUILT=$(date +%Y-%m-%dT%H:%M:%S%z)
DATA=$(arduino-cli config get directories.data)
CORE=$(ls -d "$DATA"/packages/esp32/hardware/esp32/*/ | sort -V | tail -1)
BOOT_APP0="$CORE/tools/partitions/boot_app0.bin"

# build <name> <title> <chip family> <fqbn> <sketch dir>
build() {
  name=$1 title=$2 chip=$3 fqbn=$4 sketch=$5
  base=$(basename "$sketch")
  echo "== $name ($fqbn)"
  arduino-cli compile --libraries "$ROOT/libs" --libraries "$ROOT/test" --fqbn "$fqbn" --output-dir "$TMP/$name" "$ROOT/$sketch"
  rm -rf "${OUT:?}/$name"
  mkdir -p "$OUT/$name"
  cp "$TMP/$name/$base.ino.bootloader.bin" "$OUT/$name/bootloader.bin"
  cp "$TMP/$name/$base.ino.partitions.bin" "$OUT/$name/partitions.bin"
  cp "$BOOT_APP0" "$OUT/$name/boot_app0.bin"
  cp "$TMP/$name/$base.ino.bin" "$OUT/$name/app.bin"
  size=$(wc -c < "$OUT/$name/app.bin" | tr -d ' ')
  cat > "$OUT/$name/manifest.json" <<JSON
{
  "name": "$title",
  "version": "$VERSION",
  "rev": "$REV",
  "built": "$BUILT",
  "app_size": $size,
  "new_install_prompt_erase": false,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "$chip",
      "parts": [
        { "path": "bootloader.bin", "offset": 0 },
        { "path": "partitions.bin", "offset": 32768 },
        { "path": "boot_app0.bin", "offset": 57344 },
        { "path": "app.bin", "offset": 65536 }
      ]
    }
  ]
}
JSON
  echo "   -> $OUT/$name (app $size bytes)"
}

build bridge "mimuz-link bridge (XIAO ESP32-S3)" "ESP32-S3" \
  "esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default" examples/bridge/s3_bridge
build client "mimuz-link client test device (XIAO ESP32-C6)" "ESP32-C6" \
  "esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app" examples/client/c6_test_device
build basic "mimuz-link client: basic example (XIAO ESP32-C6)" "ESP32-C6" \
  "esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc,PartitionScheme=huge_app" examples/client/basic
echo "version $VERSION, rev $REV"
