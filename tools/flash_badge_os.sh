#!/usr/bin/env bash
# Flash ConferenceBadge (Badge OS) into the factory slot with the unified
# 16MB partition table. Hold BOOT and tap RESET if esptool cannot connect.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/ConferenceBadge/build"
ESPTOOL="${ESPTOOL:-$HOME/Library/Arduino15/packages/esp32/tools/esptool_py/5.3.0/esptool}"
PORT="${1:-/dev/cu.usbmodem1101}"
FQBN="esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,CDCOnBoot=cdc"
LIBS="$ROOT/libraries"

echo "==> compile"
arduino-cli compile --fqbn "$FQBN" --libraries "$LIBS" --build-path "$BUILD" "$ROOT/ConferenceBadge"

echo "==> flash (port $PORT) — hold BOOT + tap RESET if this hangs"
"$ESPTOOL" --chip esp32s3 --port "$PORT" write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0 "$BUILD/ConferenceBadge.ino.bootloader.bin" \
  0x8000 "$ROOT/partitions_unified.bin" \
  0xe000 "$BUILD/boot_app0.bin" \
  0x10000 "$BUILD/ConferenceBadge.ino.bin"

# Legacy Loader (HU-POD / "Can it play Doom?") used to live in ota_0.
# Wipe it so OTA rollback cannot resurrect that menu.
echo "==> erase legacy Loader slot (ota_0 @ 0xF10000)"
"$ESPTOOL" --chip esp32s3 --port "$PORT" erase-region 0xF10000 0xF0000

echo "Done. Wait ~3s then: tools/.venv/bin/python tools/snap_badge_os.py"
