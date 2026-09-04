#!/usr/bin/env python3
"""Rasterize Google Fonts into 4bpp alpha PROGMEM headers (VLW-style).

Arduino_GFX has no smooth-font loader, so we bake discrete pixel sizes and let
firmware pick the nearest (near 1:1 blit beats upscaling a single master).

Flash budget: factory app is 1MB and ~88% full with 8bpp@28. We pack 4bpp and
emit sizes 28 + 42 so most UI draws at ~1.0 local scale.
"""
from __future__ import annotations

import pathlib
import re
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[1]
FONT_DIR = ROOT / "tools" / "fonts"
OUT_DIR = ROOT / "ConferenceBadge" / "fonts"

FAMILIES = [
    ("Inter", "Inter-Bold.ttf", "font_helvetica"),
    ("Archivo", "Archivo-Bold.ttf", "font_archivo"),
    ("SpaceGrotesk", "SpaceGrotesk-Bold.ttf", "font_spacegrotesk"),
    ("Chivo", "Chivo-Bold.ttf", "font_chivo"),
    ("IBMPlexMono", "IBMPlexMono-Bold.ttf", "font_plexmono"),
]

# Discrete bake sizes (firmware scale is still relative to MASTER_PX=28)
SIZES = (28, 42)
MASTER_PX = 28
BPP = 4  # 2 pixels / byte
OVERSAMPLE = 2  # render 2× then Lanczos-down for cleaner AA

CHARS = (
    " !\"#$%&'()*+,-./0123456789:;<=>?@"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
    "abcdefghijklmnopqrstuvwxyz{|}~"
)


def pack_4bpp(pixels: bytes) -> bytes:
    out = bytearray((len(pixels) + 1) // 2)
    for i, p in enumerate(pixels):
        n = min(15, (p + 8) // 17)  # 0..255 → 0..15
        if i & 1:
            out[i >> 1] |= n
        else:
            out[i >> 1] = n << 4
    return bytes(out)


def shift_glyph_y(img: Image.Image, yoff: int, target_yoff: int) -> tuple[Image.Image, int]:
    """Pad/crop so yOffset becomes target without moving ink relative to baseline."""
    dy = yoff - target_yoff
    if dy == 0:
        return img, yoff
    w, h = img.size
    if dy > 0:
        # Current yo is less negative → pad empty rows on top
        out = Image.new("L", (w, h + dy), 0)
        out.paste(img, (0, dy))
        return out, target_yoff
    # dy < 0: crop |dy| from top (ink shifts down in bitmap = higher yo)
    crop = -dy
    if crop >= h:
        return img, yoff
    return img.crop((0, crop, w, h)), target_yoff


def rasterize(ttf: pathlib.Path, px: int):
    # Oversample for better edge coverage, then downsample to target px
    src_px = px * OVERSAMPLE
    font = ImageFont.truetype(str(ttf), src_px)
    ascent, descent = font.getmetrics()
    y_adv = max(1, int(round((ascent + descent) / OVERSAMPLE)))
    raw = []
    for ch in CHARS:
        bbox = font.getbbox(ch, anchor="ls")
        if bbox is None:
            w = h = 1
            xoff = yoff = 0
            xadv = max(1, int(round((font.getlength(ch) or src_px / 3) / OVERSAMPLE)))
            img = Image.new("L", (1, 1), 0)
        else:
            x0, y0, x1, y1 = bbox
            pad = OVERSAMPLE
            x0 -= pad
            y0 -= pad
            x1 += pad
            y1 += pad
            sw = max(1, x1 - x0)
            sh = max(1, y1 - y0)
            big = Image.new("L", (sw, sh), 0)
            ImageDraw.Draw(big).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
            w = max(1, int(round(sw / OVERSAMPLE)))
            h = max(1, int(round(sh / OVERSAMPLE)))
            img = big.resize((w, h), Image.Resampling.LANCZOS)
            xoff = int(round(x0 / OVERSAMPLE))
            yoff = int(round(y0 / OVERSAMPLE))
            xadv = int(round(font.getlength(ch) / OVERSAMPLE))
        xadv = max(0, min(127, xadv))
        xoff = max(-128, min(127, xoff))
        yoff = max(-128, min(127, yoff))
        raw.append(
            {
                "ch": ch,
                "img": img,
                "xAdvance": xadv,
                "xOffset": xoff,
                "yOffset": yoff,
            }
        )

    # Snap shared baselines so caps / digits / x-height letters don't bob 1px
    def median_yo(pred):
        ys = sorted(g["yOffset"] for g in raw if pred(g["ch"]))
        return ys[len(ys) // 2] if ys else 0

    cap_yoff = median_yo(lambda c: "A" <= c <= "Z")
    digit_yoff = median_yo(lambda c: "0" <= c <= "9")
    xh = set("acemnorsuvwxz")          # flat x-height
    asc = set("bdfhiklt")              # ascenders
    desc = set("gjpqy")                # descenders
    xh_yoff = median_yo(lambda c: c in xh)
    asc_yoff = median_yo(lambda c: c in asc)
    desc_yoff = median_yo(lambda c: c in desc)
    punct_yoff = median_yo(lambda c: c in ":-.")

    glyphs = []
    alpha = bytearray()
    for g in raw:
        img, yoff = g["img"], g["yOffset"]
        ch = g["ch"]
        if "A" <= ch <= "Z":
            img, yoff = shift_glyph_y(img, yoff, cap_yoff)
        elif "0" <= ch <= "9":
            img, yoff = shift_glyph_y(img, yoff, digit_yoff)
        elif ch in xh:
            img, yoff = shift_glyph_y(img, yoff, xh_yoff)
        elif ch in asc:
            img, yoff = shift_glyph_y(img, yoff, asc_yoff)
        elif ch in desc:
            img, yoff = shift_glyph_y(img, yoff, desc_yoff)
        elif ch in ":-.":
            img, yoff = shift_glyph_y(img, yoff, punct_yoff)
        w, h = img.size
        w = min(255, w)
        h = min(255, h)
        if img.size != (w, h):
            img = img.crop((0, 0, w, h))
        packed = pack_4bpp(img.tobytes())
        offset = len(alpha)
        alpha.extend(packed)
        glyphs.append(
            {
                "ch": g["ch"],
                "offset": offset,
                "w": w,
                "h": h,
                "xAdvance": g["xAdvance"],
                "xOffset": g["xOffset"],
                "yOffset": yoff,
            }
        )
    return {
        "px": px,
        "yAdvance": min(255, y_adv),
        "glyphs": glyphs,
        "alpha": bytes(alpha),
        "first": ord(CHARS[0]),
        "last": ord(CHARS[-1]),
        "bpp": BPP,
    }


def emit_header(sym: str, data: dict, out: pathlib.Path):
    g = data["glyphs"]
    first, last = data["first"], data["last"]
    by_cp = {ord(x["ch"]): x for x in g}
    lines = [
        "#pragma once",
        "#include <stdint.h>",
        '#include "badge_smooth_font.h"',
        "",
        f"// Auto-generated — {data['px']}px {data['bpp']}bpp AA "
        f"(oversample×{OVERSAMPLE})",
        f"static const uint8_t {sym}_alpha[] PROGMEM = {{",
    ]
    a = data["alpha"]
    for i in range(0, len(a), 16):
        chunk = ", ".join(f"0x{b:02X}" for b in a[i : i + 16])
        lines.append(f"  {chunk},")
    lines.append("};")
    lines.append("")
    lines.append(f"static const BadgeSmoothGlyph {sym}_glyphs[] PROGMEM = {{")
    for cp in range(first, last + 1):
        if cp in by_cp:
            x = by_cp[cp]
            lines.append(
                f"  {{{x['offset']}, {x['w']}, {x['h']}, {x['xAdvance']}, "
                f"{x['xOffset']}, {x['yOffset']}}}, // {chr(cp)!r}"
            )
        else:
            lines.append("  {0, 0, 0, 0, 0, 0},")
    lines.append("};")
    lines.append("")
    lines.append(f"static const BadgeSmoothFont {sym} PROGMEM = {{")
    lines.append(f"  {sym}_alpha,")
    lines.append(f"  {sym}_glyphs,")
    lines.append(f"  {first}, {last},")
    lines.append(f"  {data['yAdvance']},")
    lines.append(f"  {data['px']},")
    lines.append(f"  {data['bpp']},")
    lines.append("};")
    lines.append("")
    out.write_text("\n".join(lines))
    print(f"  wrote {out.name}  alpha={len(a)}B  (~{len(a)/1024:.1f}KB packed)")


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUT_DIR.parent / "badge_smooth_font.h").write_text(
        """#ifndef BADGE_SMOOTH_FONT_H
#define BADGE_SMOOTH_FONT_H
#include <stdint.h>
#ifndef PROGMEM
#define PROGMEM
#endif

struct BadgeSmoothGlyph {
  uint32_t bitmapOffset;
  uint8_t width;
  uint8_t height;
  int8_t xAdvance;
  int8_t xOffset;
  int8_t yOffset;
};

struct BadgeSmoothFont {
  const uint8_t *alpha;
  const BadgeSmoothGlyph *glyphs;
  uint8_t first;
  uint8_t last;
  uint8_t yAdvance;
  uint8_t pxSize;
  uint8_t bpp;  // 4 or 8
};

// Call-site scales are relative to this master (historical 28px bake).
#ifndef BADGE_FONT_MASTER_PX
#define BADGE_FONT_MASTER_PX 28
#endif
#endif
"""
    )

    index = [
        "#pragma once",
        '#include "badge_smooth_font.h"',
        "",
        f"#define kSmoothFontSizeCount {len(SIZES)}",
        "",
    ]
    for label, ttf_name, sym_base in FAMILIES:
        ttf = FONT_DIR / ttf_name
        if not ttf.exists():
            print(f"MISSING {ttf}", file=sys.stderr)
            return 1
        for px in SIZES:
            sym = f"{sym_base}_{px}"
            print(f"rasterize {label} {px}px from {ttf.name}")
            data = rasterize(ttf, px)
            hdr = OUT_DIR / f"{sym}.h"
            emit_header(sym, data, hdr)
            index.append(f'#include "fonts/{sym}.h"')
        index.append("")

    index.append(
        f"static const BadgeSmoothFont *const kSmoothFonts[5][kSmoothFontSizeCount] = {{"
    )
    for _, _, sym_base in FAMILIES:
        refs = ", ".join(f"&{sym_base}_{px}" for px in SIZES)
        index.append(f"  {{ {refs} }},")
    index.append("};")
    index.append("")
    (OUT_DIR.parent / "badge_fonts.h").write_text("\n".join(index) + "\n")
    print("wrote ConferenceBadge/badge_fonts.h")
    print(f"MASTER_PX={MASTER_PX} sizes={SIZES} bpp={BPP}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
