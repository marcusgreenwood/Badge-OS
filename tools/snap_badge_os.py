#!/usr/bin/env python3
"""Batch-capture every Badge OS screen over USB.

Requires ConferenceBadge running with FACE / SNAP serial commands.

Usage:
  tools/.venv/bin/python tools/snap_badge_os.py
  tools/.venv/bin/python tools/snap_badge_os.py --port /dev/cu.usbmodem1101
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("pyserial required: tools/.venv/bin/pip install pyserial", file=sys.stderr)
    sys.exit(1)

# Reuse RGB565 decode from snap_badge
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from snap_badge import find_port, rgb565_to_rgb, read_line  # noqa: E402

OUT_DIR = ROOT / "screenshots" / "badge-os"

FACES = [
    "IDENTITY",
    "CONNECT",
    "SCHEDULE",
    "STATUS",
    "INBOX",
    "SYSTEM",
    "ICEBREAKER",
    "ARCADE",
    "RADAR",
    "SETTINGS",
]


def send_line(ser: serial.Serial, line: str) -> None:
    ser.write((line.strip() + "\n").encode("utf-8"))
    ser.flush()


def expect_prefix(ser: serial.Serial, prefix: str, timeout_s: float = 5.0) -> str:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = read_line(ser, timeout_s=1.0)
        if line.startswith(prefix):
            return line
        if line.startswith("FACEERR") or line.startswith("SNAPERR"):
            raise RuntimeError(line)
    raise TimeoutError(f"timed out waiting for {prefix}")


def snap_raw(ser: serial.Serial) -> tuple[int, int, bytes]:
    send_line(ser, "SNAP")
    header = None
    deadline = time.time() + 8.0
    while time.time() < deadline:
        line = read_line(ser, timeout_s=2.0)
        if line.startswith("SNAPOK "):
            header = line
            break
        if line.startswith("SNAPERR"):
            raise RuntimeError(line)
    if not header:
        raise TimeoutError("no SNAPOK")
    parts = header.split()
    w, h, nbytes = int(parts[1]), int(parts[2]), int(parts[3])
    raw = bytearray()
    while len(raw) < nbytes:
        chunk = ser.read(nbytes - len(raw))
        if not chunk:
            time.sleep(0.01)
            continue
        raw.extend(chunk)
    end_deadline = time.time() + 3.0
    while time.time() < end_deadline:
        line = read_line(ser, timeout_s=1.0)
        if line.strip() == "SNAPEND":
            break
    return w, h, bytes(raw)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default=None)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument(
        "--out",
        type=Path,
        default=OUT_DIR,
        help="Output directory (default screenshots/badge-os)",
    )
    args = ap.parse_args()

    port = find_port(args.port)
    out_dir: Path = args.out
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"Port {port} → capturing {len(FACES)} faces …")
    with serial.Serial(port, args.baud, timeout=0.2) as ser:
        time.sleep(0.2)
        ser.reset_input_buffer()

        send_line(ser, "THEME RESET")
        try:
            expect_prefix(ser, "THEMEOK")
        except Exception:
            pass  # older firmware without THEME RESET
        time.sleep(0.1)

        for i, code in enumerate(FACES):
            send_line(ser, f"FACE {code}")
            expect_prefix(ser, "FACEOK")
            time.sleep(0.15)
            w, h, raw = snap_raw(ser)
            img = rgb565_to_rgb(raw, w, h)
            path = out_dir / f"{i:02d}-{code.lower()}.png"
            img.save(path)
            print(f"  wrote {path.name} ({path.stat().st_size} bytes)")

    print(f"Done → {out_dir}")


if __name__ == "__main__":
    main()
