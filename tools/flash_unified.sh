#!/usr/bin/env bash
# Flash all hu-pod apps using partitions_unified.csv (16MB Waveshare board).
# Requires arduino-cli builds in each sketch folder and esptool on PATH.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LIBS="$ROOT/libraries"
FQBN="esp32:esp32:esp32s3"
PORT="${1:-/dev/cu.usbmodem*}"

pick_port() {
  if [[ "$PORT" != *"*"* ]]; then
    echo "$PORT"
    return
  fi
  ls $PORT 2>/dev/null | head -1
}

build_sketch() {
  local dir="$1"
  echo "==> build $dir"
  arduino-cli compile --fqbn "$FQBN" --libraries "$LIBS" "$ROOT/$dir"
}

flash_bin() {
  local offset="$1"
  local bin="$2"
  echo "==> flash $bin @ $offset"
  esptool.py --chip esp32s3 --port "$DEV" write_flash "$offset" "$bin"
}

DEV="$(pick_port || true)"
if [[ -z "${DEV:-}" ]]; then
  echo "No serial port found. Pass port as first argument." >&2
  exit 1
fi

echo "Using port $DEV"

# Partition table + otadata (empty -> boot factory)
esptool.py --chip esp32s3 --port "$DEV" write_flash 0x8000 "$ROOT/partitions_unified.bin" 2>/dev/null || {
  echo "Build partition binary first:"
  echo "  python3 -m esptool gen_esp32part $ROOT/partitions_unified.csv $ROOT/partitions_unified.bin"
  exit 1
}

for sketch in ConferenceBadge HotelTower Volcano3D FighterJet; do
  build_sketch "$sketch"
done

CB="$ROOT/ConferenceBadge/build/esp32.esp32.esp32s3"
HT="$ROOT/HotelTower/build/esp32.esp32.esp32s3"
VO="$ROOT/Volcano3D/build/esp32.esp32.esp32s3"
FJ="$ROOT/FighterJet/build/esp32.esp32.esp32s3"

flash_bin 0x0 "$CB/ConferenceBadge.ino.bootloader.bin"
flash_bin 0x10000 "$CB/ConferenceBadge.ino.bin"
flash_bin 0x210000 "$HT/HotelTower.ino.bin"
flash_bin 0x390000 "$VO/Volcano3D.ino.bin"
flash_bin 0x490000 "$FJ/FighterJet.ino.bin"

# Do not flash Loader; erase ota_0 so the old HU-POD menu cannot boot.
echo "==> erase legacy Loader slot (ota_0 @ 0xF10000)"
esptool.py --chip esp32s3 --port "$DEV" erase_region 0xF10000 0xF0000

echo "Done. Power-cycle to boot ConferenceBadge (factory)."
