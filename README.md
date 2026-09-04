# Badge OS

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

## Using a coding agent

Badge OS is designed so you can change event branding, faces, and apps by prompting a coding agent (Cursor, Claude Code, etc.) in this repo.

### What to ask for

Point the agent at the right surface, then describe the outcome:

| Goal | Start here |
|------|------------|
| Name, company, Wi‑Fi, schedule, icebreakers, theme | `ConferenceBadge/badge_settings.h`, `profile.h` |
| Photo / QR / logo assets | `ConferenceBadge/photo.h`, `qr_halftone.h`, `company_mark*.h` (or regenerate via `tools/`) |
| New or changed badge faces / menu | `ConferenceBadge/ConferenceBadge.ino` (`BadgeFace`, `drawFace*`, gestures) |
| Arcade / promo game behavior | `HotelTower/HotelTower.ino` |
| Doom or other ESP-IDF apps | `DoomPod/` |
| Flash layout / new app slots | `partitions_unified.csv` + launcher handoff in ConferenceBadge |

Example prompts:

- “Update the identity face for DevConf 2026: name Jane Doe, company Acme, sand theme, cobalt accent.”
- “Replace the schedule and icebreaker copy with this agenda list.”
- “Add a new face that shows our sponsor QR and put it in the rim menu.”
- “Add a new Arduino sketch app in `ota_2` and wire a launcher tile from Arcade.”

### Constraints to mention

When prompting, remind the agent of the platform limits so changes stay flashable:

- Display is **466×466 circular** — UI must stay inside the round safe area (rim + center action disc).
- **ConferenceBadge** and **HotelTower** are Arduino sketches; **DoomPod** is ESP-IDF.
- Multi-app boots use the unified partition table; apps return via the shared launcher/power helpers (`launcher_exit.h`, `power_button.h`).
- Prefer editing config headers over hard-coding one-off strings in draw code when possible.
- After firmware changes, flash with `./tools/flash_badge_os.sh` (OS only) or `./tools/flash_unified.sh` (all slots). Use `SNAP` / `FACE` / `MENU` over serial to verify.

### Suggested agent workflow

1. Open this repo in the agent and state the event goal in one sentence.
2. Ask it to read `README.md`, `badge_settings.h`, and the relevant `.ino` before editing.
3. Have it make a small, reviewable change (config first, then UI/logic).
4. Build/flash, then iterate with screenshot or serial feedback (`tools/snap_badge_os.py` if available).
