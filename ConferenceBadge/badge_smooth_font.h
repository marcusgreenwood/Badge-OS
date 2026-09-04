#ifndef BADGE_SMOOTH_FONT_H
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
