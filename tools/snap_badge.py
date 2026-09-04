#!/usr/bin/env python3
"""Grab a PNG screenshot of the ConferenceBadge framebuffer over USB serial.

Trigger: firmware listens for a line `SNAP` and replies with:
  SNAPOK <w> <h> <nbytes>\\n
  <nbytes bytes of RGB565 little-endian>
  SNAPEND\\n

Usage:
  tools/.venv/bin/python tools/snap_badge.py
  tools/.venv/bin/python tools/snap_badge.py --port /dev/cu.usbmodem1101 -o shot.png
"""

from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("pyserial required: tools/.venv/bin/pip install pyserial", file=sys.stderr)
    sys.exit(1)

try:
    from PIL import Image
except ImportError:
    print("Pillow required: tools/.venv/bin/pip install Pillow", file=sys.stderr)
    sys.exit(1)

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "screenshots"


def find_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = list(list_ports.comports())
    for p in ports:
        if "usbmodem" in p.device or "USB" in (p.description or ""):
            return p.device
    if ports:
        return ports[0].device
    raise SystemExit("No serial port found. Pass --port /dev/cu.usbmodem…")


def rgb565_to_rgb(buf: bytes, w: int, h: int) -> Image.Image:
    if len(buf) != w * h * 2:
        raise ValueError(f"expected {w * h * 2} bytes, got {len(buf)}")
    px = bytearray(w * h * 3)
    for i in range(w * h):
        lo = buf[i * 2]
        hi = buf[i * 2 + 1]
        v = lo | (hi << 8)
        r = ((v >> 11) & 0x1F) * 255 // 31
        g = ((v >> 5) & 0x3F) * 255 // 63
        b = (v & 0x1F) * 255 // 31
        o = i * 3
        px[o] = r
        px[o + 1] = g
        px[o + 2] = b
    return Image.frombytes("RGB", (w, h), bytes(px))


def read_line(ser: serial.Serial, timeout_s: float = 5.0) -> str:
    deadline = time.time() + timeout_s
    buf = bytearray()
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b == b"\n":
            return buf.decode("utf-8", errors="replace").rstrip("\r")
        buf.extend(b)
        if len(buf) > 500:
            buf.clear()
    raise TimeoutError("timed out waiting for line")


def snap(port: str, out: Path, baud: int = 115200) -> Path:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    with serial.Serial(port, baud, timeout=0.2) as ser:
        time.sleep(0.15)
        ser.reset_input_buffer()
        ser.write(b"SNAP\n")
        ser.flush()

        # Skip any chatter until SNAPOK
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
            raise TimeoutError("no SNAPOK from device (is ConferenceBadge running?)")

        parts = header.split()
        w, h, nbytes = int(parts[1]), int(parts[2]), int(parts[3])
        raw = bytearray()
        while len(raw) < nbytes:
            chunk = ser.read(nbytes - len(raw))
            if not chunk:
                # USB CDC can stall briefly
                time.sleep(0.01)
                continue
            raw.extend(chunk)

        # Drain until SNAPEND (may have a blank line first)
        end_deadline = time.time() + 3.0
        while time.time() < end_deadline:
            line = read_line(ser, timeout_s=1.0)
            if line.strip() == "SNAPEND":
                break

    img = rgb565_to_rgb(bytes(raw), w, h)
    img.save(out)
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default=None, help="Serial port (auto-detect usbmodem)")
    ap.add_argument(
        "-o",
        "--output",
        default=None,
        help="Output PNG path (default screenshots/badge-YYYYMMDD-HHMMSS.png)",
    )
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    port = find_port(args.port)
    if args.output:
        out = Path(args.output)
    else:
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        out = OUT_DIR / f"badge-{stamp}.png"

    print(f"Port {port} → SNAP …")
    path = snap(port, out, baud=args.baud)
    print(f"Wrote {path} ({path.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
