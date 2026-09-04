#!/usr/bin/env python3
"""Generate a halftone QR code embedding a portrait (kloet.net / research method).

Each QR module becomes a (3*factor)x(3*factor) block. The center factor×factor
submodules are locked to the QR bit; the surrounding submodules carry a
Floyd–Steinberg dither of the source image. Finder and alignment patterns
stay solid for scannability.

Writes ConferenceBadge/qr_halftone.h (RGB565 LE) for the badge firmware.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import qrcode
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "ConferenceBadge"


def rgb565_le(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def alignment_pattern_positions(version: int) -> list[int]:
    """QR alignment pattern centers for versions 1–10 (ISO/IEC 18004)."""
    table = {
        1: [],
        2: [6, 18],
        3: [6, 22],
        4: [6, 26],
        5: [6, 30],
        6: [6, 34],
        7: [6, 22, 38],
        8: [6, 24, 42],
        9: [6, 26, 46],
        10: [6, 28, 50],
    }
    return table.get(version, [])


def is_protected(mx: int, my: int, n: int, border: int, alignment: list[int]) -> bool:
    """Finder patterns (+ separators), quiet zone, and alignment patterns stay solid."""
    b = border
    dn = n - 2 * b
    dx, dy = mx - b, my - b

    # Quiet zone
    if dx < 0 or dy < 0 or dx >= dn or dy >= dn:
        return True

    # Finders / separators: 8×8 in each corner of the data grid
    if dx < 8 and dy < 8:
        return True
    if dx >= dn - 8 and dy < 8:
        return True
    if dx < 8 and dy >= dn - 8:
        return True

    # Alignment patterns: 5×5 centered on each alignment coordinate
    for ay in alignment:
        for ax in alignment:
            if (ax < 8 and ay < 8) or (ax >= dn - 8 and ay < 8) or (ax < 8 and ay >= dn - 8):
                continue
            if abs(dx - ax) <= 2 and abs(dy - ay) <= 2:
                return True
    return False


def make_halftone_qr(
    url: str,
    photo: Image.Image,
    *,
    factor: int = 3,
    border: int = 2,
    ecc=qrcode.constants.ERROR_CORRECT_H,
    out_size: int = 340,
    logo_scale: float = 1.0,
) -> Image.Image:
    qr = qrcode.QRCode(
        version=None,
        error_correction=ecc,
        box_size=1,
        border=border,
    )
    qr.add_data(url)
    qr.make(fit=True)
    matrix = qr.get_matrix()  # includes quiet zone (border)
    n = len(matrix)
    version = qr.version
    align = alignment_pattern_positions(version)

    cell = 3 * factor
    raw = n * cell

    # Optional letterboxing: shrink the logo on a white field so it reads smaller
    # inside the QR (finder patterns stay clear around the edges).
    base = photo.convert("RGB")
    if logo_scale < 1.0:
        scale = max(0.2, min(1.0, logo_scale))
        lw = max(1, int(raw * scale))
        lh = max(1, int(raw * scale))
        logo = ImageOps.contain(base, (lw, lh), Image.Resampling.LANCZOS)
        canvas = Image.new("RGB", (raw, raw), (255, 255, 255))
        canvas.paste(logo, ((raw - logo.size[0]) // 2, (raw - logo.size[1]) // 2))
        src = ImageOps.grayscale(canvas)
    else:
        src = ImageOps.grayscale(base).resize((raw, raw), Image.Resampling.LANCZOS)
        src = ImageOps.autocontrast(src, cutoff=2)
    pixels = src.load()

    err = [[0.0] * raw for _ in range(raw)]
    out = Image.new("L", (raw, raw), 255)
    out_px = out.load()

    def set_px(x: int, y: int, val: int) -> None:
        out_px[x, y] = 0 if val else 255

    for my in range(n):
        for mx in range(n):
            dark = bool(matrix[my][mx])
            ox, oy = mx * cell, my * cell
            protected = is_protected(mx, my, n, border, align)

            if protected:
                for dy in range(cell):
                    for dx in range(cell):
                        set_px(ox + dx, oy + dy, 1 if dark else 0)
                continue

            c0 = factor
            for dy in range(cell):
                for dx in range(cell):
                    x, y = ox + dx, oy + dy
                    in_center = c0 <= dx < c0 + factor and c0 <= dy < c0 + factor
                    if in_center:
                        set_px(x, y, 1 if dark else 0)
                        continue

                    lum = pixels[x, y] / 255.0 + err[y][x]
                    bit = 1 if lum < 0.5 else 0
                    set_px(x, y, bit)
                    quant = 0.0 if bit else 1.0
                    e = lum - quant
                    if x + 1 < raw:
                        err[y][x + 1] += e * 7 / 16
                    if y + 1 < raw:
                        if x > 0:
                            err[y + 1][x - 1] += e * 3 / 16
                        err[y + 1][x] += e * 5 / 16
                        if x + 1 < raw:
                            err[y + 1][x + 1] += e * 1 / 16

    return out.resize((out_size, out_size), Image.Resampling.NEAREST).convert("RGB")


def write_header(img: Image.Image, path: Path, symbol: str = "kQrHalftoneRgb565") -> None:
    w, h = img.size
    pixels = img.load()
    words: list[int] = []
    for y in range(h):
        for x in range(w):
            r, g, b = pixels[x, y]
            words.append(rgb565_le(r, g, b))

    lines = [
        "// Auto-generated by tools/make_halftone_qr.py — do not edit by hand.",
        "#pragma once",
        "",
        f"#define QR_HALFTONE_W {w}",
        f"#define QR_HALFTONE_H {h}",
        "",
        f"static const uint16_t {symbol}[QR_HALFTONE_W * QR_HALFTONE_H] PROGMEM = {{",
    ]
    for i in range(0, len(words), 12):
        chunk = words[i : i + 12]
        lines.append("  " + ", ".join(f"0x{w_:04X}" for w_ in chunk) + ",")
    lines.append("};")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="https://www.linkedin.com/in/marcusgreenwood/")
    ap.add_argument("--photo", type=Path, default=None, help="Source portrait (default: fetch via scrape)")
    ap.add_argument("--factor", type=int, default=3)
    ap.add_argument("--size", type=int, default=320, help="Output pixel size for the badge")
    ap.add_argument("--logo-scale", type=float, default=1.0,
                    help="Shrink logo on white field inside the QR (e.g. 0.7)")
    ap.add_argument("--preview", type=Path, default=OUT_DIR / "qr_halftone_preview.png")
    args = ap.parse_args()

    # Prefer regenerating from LinkedIn via the scraper helpers
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    if args.photo and args.photo.exists():
        raw = Image.open(args.photo)
        if raw.mode in ("RGBA", "LA") or (raw.mode == "P" and "transparency" in raw.info):
            bg = Image.new("RGBA", raw.size, (255, 255, 255, 255))
            photo = Image.alpha_composite(bg, raw.convert("RGBA")).convert("RGB")
        else:
            photo = raw.convert("RGB")
    else:
        from scrape_linkedin_badge import download_photo, parse_profile

        profile = parse_profile(args.url)
        print(f"Photo from LinkedIn: {profile['name']} / {profile['company']}")
        photo = download_photo(profile["photo_url"])

    print(f"Building halftone QR for {args.url} "
          f"(factor={args.factor}, size={args.size}, logo_scale={args.logo_scale})")
    img = make_halftone_qr(
        args.url, photo, factor=args.factor, out_size=args.size, logo_scale=args.logo_scale
    )
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    img.save(args.preview)
    write_header(img, OUT_DIR / "qr_halftone.h")
    print(f"Wrote {OUT_DIR / 'qr_halftone.h'} and {args.preview}")


if __name__ == "__main__":
    main()
