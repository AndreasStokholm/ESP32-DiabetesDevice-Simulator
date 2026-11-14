#!/bin/sh
# Builds the web flasher into flasher/dist: the page, its manifest, one merged
# firmware image, and a pinned copy of ESP Web Tools (Apache-2.0).
# Upload that folder to any HTTPS web server (Web Serial needs HTTPS).
#
#   tools/make_flasher.sh                 build flasher/dist
#   tools/make_flasher.sh user@host:dir   build, then rsync it there
set -eu
cd "$(dirname "$0")/.."

EWT=10.5.2
OUT=flasher/dist
VERSION=$(git rev-parse --short HEAD)
[ -z "$(git status --porcelain)" ] || { echo "commit your changes first: the flasher names the commit it was built from" >&2; exit 1; }

pio run
B=.pio/build/esp32s3
BOOT_APP0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

rm -rf "$OUT"
mkdir -p "$OUT/vendor/esp-web-tools"
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 merge_bin -o "$OUT/esp-sim-$VERSION.bin" \
    --flash_mode dio --flash_freq 80m --flash_size 8MB \
    0x0 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$B/firmware.bin"

cat > "$OUT/manifest.json" <<JSON
{
  "name": "ESP-Sim (Omnipod pod + Dexcom G7 simulator)",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "builds": [
    { "chipFamily": "ESP32-S3", "parts": [ { "path": "esp-sim-$VERSION.bin", "offset": 0 } ] }
  ]
}
JSON
cp flasher/index.html "$OUT/"

# ESP Web Tools, pinned and served from the same site, so the page does not
# depend on a CDN.
TMP=$(mktemp -d)
curl -fsSL --retry 3 "https://registry.npmjs.org/esp-web-tools/-/esp-web-tools-$EWT.tgz" | tar xz -C "$TMP"
cp -R "$TMP/package/dist/web/." "$OUT/vendor/esp-web-tools/"
cp "$TMP/package/LICENSE" "$OUT/vendor/esp-web-tools/LICENSE" 2>/dev/null || true
rm -rf "$TMP"

echo "built $OUT ($(du -sh "$OUT" | cut -f1)) for commit $VERSION"
if [ $# -ge 1 ]; then
    rsync -av --delete "$OUT/" "$1/"
fi
