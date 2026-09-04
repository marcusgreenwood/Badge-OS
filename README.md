# hu-pod

A generic operating system for conference and event badges built on a circular-screen ESP32.

Attendees get a wearable identity surface—name, photo, QR, schedule, icebreakers—plus room to launch event-specific apps and games from the same device.

## Hardware

[Waveshare ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75) — 466×466 round AMOLED, capacitive touch, IMU, 16MB flash.

## Apps

| Folder | Role | Slot |
|--------|------|------|
| **ConferenceBadge** | Badge OS (default boot) — faces, menu, app launcher | `factory` |
| **HotelTower** | Stack-the-floors promo game | `ota_1` |
| **DoomPod** | Doom on the round display (ESP-IDF) | `ota_4` |

ConferenceBadge boots by default and can hand off to apps in the OTA partitions, then return.

## Layout

```
ConferenceBadge/   Badge OS (Arduino)
DoomPod/           Doom port (ESP-IDF)
HotelTower/        Arcade game (Arduino)
libraries/         Arduino GFX + sensor libs used by the sketches
tools/             Flash helpers and asset scripts
partitions_unified.*   Multi-app flash map (16MB)
```

Local helpers such as `promo_codes.*` and capture screenshots are gitignored.

## Flash map (16MB)

| Offset | Size | Contents |
|--------|------|----------|
| `0x10000` | 2MB | ConferenceBadge |
| `0x210000` | 1536K | HotelTower |
| `0x590000` | 1536K | DoomPod |
| `0x710000` | 8MB | Doom WAD partition |

See `partitions_unified.csv` for the full table.

## Build & flash

**Badge OS** (Arduino CLI + esptool):

```bash
./tools/flash_badge_os.sh /dev/cu.usbmodemXXXX
```

**All unified slots** (when apps are built):

```bash
./tools/flash_unified.sh /dev/cu.usbmodemXXXX
```

Board FQBN used by the scripts:

`esp32:esp32:esp32s3` with 16MB flash and OPI PSRAM.

If esptool cannot connect, hold **BOOT** and tap **RESET**.

## Serial (Badge OS)

Useful debug commands over USB serial:

- `SNAP` — dump framebuffer
- `FACE n` — switch face
- `MENU 0|1` — close / open menu
- `CODE` — promo / unlock helpers

## Extending

Add a new event app as an Arduino sketch (or ESP-IDF project), place its binary in a free OTA slot from the unified partition table, and register a launcher entry in ConferenceBadge. Keep the round 466×466 canvas and shared touch / power-button helpers so apps feel like part of the same OS.
