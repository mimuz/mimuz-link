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
#
# Builds the bridge (examples/bridge/s3_bridge) and the client (examples/client/c6_test_device) with arduino-cli and
# puts, for each, the bootloader, the partition table, boot_app0 and the application into www/firmware/<name>/ with
# a manifest.json for ESP Web Tools. The parts are written at the same offsets as "arduino-cli upload"; the merged
# image is not used because it covers the whole flash and would also overwrite the settings (NVS at 0x9000-0xDFFF:
# pairing, label, ...).
#
# usage: web-console/firmware/build.sh   (needs arduino-cli with the esp32 core, see the README)
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/web-console/www/firmware"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

VERSION=$(sed -n 's/^#define MIMUZ_LINK_VERSION "\(.*\)"/\1/p' "$ROOT/libs/mimuz-link/src/mimuz_link_proto.h")
REV=$(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo unknown)
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
echo "version $VERSION, rev $REV"
