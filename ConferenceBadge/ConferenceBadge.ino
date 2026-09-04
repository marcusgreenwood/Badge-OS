// Conference Badge OS — Waveshare ESP32-S3-Touch-AMOLED-1.75 (466×466)
// UI matches esp32-conference-badge-screens/project/Badge OS.dc.html
//
// Flash layout (partitions_unified.csv):
//   factory @ 0x10000  -> ConferenceBadge (2MB)
//   ota_1   @ 0x210000 -> Hotel Tower
//   ota_2   @ 0x390000 -> Volcano3D
//   ota_3   @ 0x490000 -> FighterJet
//   ota_4   @ 0x590000 -> DoomPod
//
// Serial: SNAP | FACE n|CODE | MENU 0|1

#include <Arduino.h>
#include "power_button.h"
#include "badge_settings.h"
#include "qr_halftone.h"
#include <Wire.h>
#include <Preferences.h>
#include <Arduino_GFX_Library.h>
#include "badge_fonts.h"
#include "company_mark.h"
#include "TouchDrvCSTXXX.hpp"
#include "SensorQMI8658.hpp"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include <math.h>
#if defined(CONFIG_BT_ENABLED)
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#endif

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_RESET 39
#define LCD_WIDTH 466
#define LCD_HEIGHT 466

#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 11
#define TP_RESET 40

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel =
    new Arduino_CO5300(bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);
TouchDrvCST92xx touch;
SensorQMI8658 qmi;

enum UiState { UI_BADGE, UI_DOOM_ANIM };
UiState state = UI_BADGE;

constexpr int16_t kCx = LCD_WIDTH / 2;
constexpr int16_t kCy = LCD_HEIGHT / 2;
constexpr float kOuterR = 224.0f;
constexpr float kCirc = 2.0f * (float)M_PI * kOuterR;
// Middle disc = face action; outside this radius = advance to next face
constexpr int16_t kActionR = 148;
constexpr size_t kFbBytes = (size_t)LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t);

BadgeFace badgeFace = FACE_IDENTITY;
bool menuOpen = false;
bool faceDirty = true;
bool imuOk = false;
uint32_t towerBest = 0;
uint32_t scanCount = 0;
uint8_t statusIdx = 0;
uint8_t iceIdx = 0;
uint8_t notifIdx = 0;
uint32_t lastInteractionMs = 0;
uint32_t animStartMs = 0;
bool doomBootTried = false;

// Theme (NVS overrides seed from badge_settings.h)
uint8_t cfgSurface = BADGE_SURFACE;
uint8_t cfgPalette = BADGE_PALETTE;
uint8_t cfgTicks = BADGE_TICKS;
uint8_t cfgFont = BADGE_FONT;

// Brief press feedback for inbox / status buttons
uint32_t btnFlashUntil = 0;
int8_t btnFlashId = -1;  // 0=later/prev, 1=accept/next

// Radar sweep
float radarAngle = 0;

struct Theme {
  uint16_t page, bg, fg, dim, faint, line, line2, line3, accent, ink, disc, hover,
      scrim;
};

Theme theme;

enum GestureEvent {
  GESTURE_NONE,
  GESTURE_TAP,
  GESTURE_HOLD_MENU,  // first rim lap complete → open menu
  GESTURE_HOLD_DOOM   // fifth rim lap complete → doom
};

bool touchDown = false;
bool holdCandidate = false;
bool holdArmed = false;
bool holdDoomSent = false;
uint32_t touchStartMs = 0;
uint32_t touchLastSeenMs = 0;
int16_t touchStartX = 0, touchStartY = 0;
int16_t tapX = 0, tapY = 0;
volatile bool touchPending = false;
uint32_t lastTouchReportMs = 0;

constexpr uint32_t kHoldArmMs = 200;    // debounce before rim arc starts
constexpr uint32_t kHoldLapMs = 2000;   // one full circle around the bezel
constexpr uint32_t kHoldPauseMs = 500;  // dwell on completed ring before next lap
constexpr uint32_t kLiftQuietMs = 120;
constexpr int kHoldDoomLaps = 5;

uint16_t *holdSnap = nullptr;
bool holdSnapValid = false;
float holdLaps = 0.0f;
bool holdMenuFired = false;

// Doom fire
constexpr int kFW = 58, kFH = 40;
uint8_t fire[kFW * kFH];
uint16_t firePal[13];

float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static uint16_t *allocFb(const char *tag) {
  uint16_t *p = (uint16_t *)heap_caps_malloc(
      kFbBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) Serial.printf("allocFb %s failed\n", tag);
  return p;
}

// ---- theme --------------------------------------------------------------

static uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return gfx->color565(r, g, b);
}

static uint16_t rgb32(uint32_t c) {
  return rgb((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// Accent swatches — cycle order on Settings. inkW=1 → white text on accent.
struct AccentSwatch {
  const char *name;
  uint8_t r, g, b;
  uint8_t inkW;
};

static const AccentSwatch kAccents[] = {
    {"Orange", 0xE8, 0x54, 0x1F, 1},
    {"Cobalt", 0x4C, 0x7D, 0xFF, 0},
    {"Yellow", 0xE5, 0xA7, 0x00, 0},
    {"Green", 0x0E, 0x8A, 0x55, 1},
    {"Teal", 0x0D, 0x9B, 0x8A, 0},
    {"Cyan", 0x00, 0xB8, 0xD4, 0},
    {"Sky", 0x4F, 0xB0, 0xFF, 0},
    {"Azure", 0x1E, 0x7A, 0xE6, 1},
    {"Violet", 0x7C, 0x5C, 0xFF, 1},
    {"Magenta", 0xD1, 0x2B, 0x8A, 1},
    {"Pink", 0xE8, 0x5A, 0x9B, 0},
    {"Coral", 0xFF, 0x6B, 0x4A, 0},
    {"Crimson", 0xD4, 0x20, 0x2A, 1},
    {"Amber", 0xF5, 0x9E, 0x0B, 0},
    {"Gold", 0xC9, 0xA2, 0x27, 0},
    {"Lime", 0x8B, 0xC3, 0x4A, 0},
    {"Mint", 0x2B, 0xBF, 0x8A, 0},
    {"Ember", 0xFF, 0x57, 0x22, 1},
    {"Rose", 0xE0, 0x4F, 0x6E, 1},
    {"Achromatic", 0, 0, 0, 0},  // uses surface fg/bg
};
static constexpr int kAccentN =
    (int)(sizeof(kAccents) / sizeof(kAccents[0]));

void applyTheme() {
  if (cfgSurface == 1) {
    // Sand
    theme.page = rgb(0xC9, 0xC5, 0xBB);
    theme.bg = rgb(0xE4, 0xE1, 0xD9);
    theme.fg = rgb(0x1A, 0x19, 0x17);
    theme.dim = rgb(0x6B, 0x68, 0x62);
    theme.faint = rgb(0x91, 0x8D, 0x85);
    theme.line = rgb(0xB7, 0xB2, 0xA7);
    theme.line2 = rgb(0xCF, 0xCA, 0xC0);
    theme.line3 = rgb(0xDA, 0xD6, 0xCC);
    theme.hover = rgb(0xD8, 0xD4, 0xCA);
    theme.scrim = rgb(0xE4, 0xE1, 0xD9);
    theme.disc = rgb(0xE4, 0xE1, 0xD9);  // same as sand bg — no white plate
  } else {
    // Black
    theme.page = rgb(0x0B, 0x0B, 0x0B);
    theme.bg = rgb(0x00, 0x00, 0x00);
    theme.fg = rgb(0xED, 0xEA, 0xE4);
    theme.dim = rgb(0x8C, 0x88, 0x80);
    theme.faint = rgb(0x5E, 0x5B, 0x55);
    theme.line = rgb(0x2A, 0x28, 0x25);
    theme.line2 = rgb(0x1C, 0x1B, 0x19);
    theme.line3 = rgb(0x13, 0x12, 0x11);
    theme.hover = rgb(0x17, 0x16, 0x14);
    theme.scrim = rgb(0x00, 0x00, 0x00);
    theme.disc = rgb(0x00, 0x00, 0x00);
  }

  if (cfgPalette >= kAccentN) cfgPalette = 0;
  const AccentSwatch &sw = kAccents[cfgPalette];
  if (cfgPalette == kAccentN - 1) {
    // Achromatic — last swatch
    theme.accent = theme.fg;
    theme.ink = theme.bg;
  } else {
    theme.accent = rgb(sw.r, sw.g, sw.b);
    theme.ink = sw.inkW ? rgb(0xFF, 0xFF, 0xFF) : rgb(0x00, 0x00, 0x00);
  }
}

static const char *surfaceName() {
  return cfgSurface == 1 ? "Sand" : "Black";
}
static const char *paletteName() {
  if (cfgPalette >= kAccentN) return kAccents[0].name;
  return kAccents[cfgPalette].name;
}
static const char *fontName() {
  static const char *n[] = {"Helvetica", "Archivo", "Space Grotesk", "Chivo",
                            "Plex Mono"};
  return n[cfgFont % 5];
}

// Call-site `scale` is relative to BADGE_FONT_MASTER_PX (28). Pick nearest bake.
static const BadgeSmoothFont *pickSmoothFont(float scale) {
  const float targetPx = (float)BADGE_FONT_MASTER_PX * scale;
  const BadgeSmoothFont *const *sizes = kSmoothFonts[cfgFont % 5];
  const BadgeSmoothFont *best = sizes[0];
  float bestErr = fabsf(targetPx - (float)best->pxSize);
  for (uint8_t i = 1; i < kSmoothFontSizeCount; i++) {
    const float err = fabsf(targetPx - (float)sizes[i]->pxSize);
    if (err < bestErr) {
      best = sizes[i];
      bestErr = err;
    }
  }
  return best;
}

static const BadgeSmoothFont *activeSmoothFont() {
  // Default face metrics / settings preview — master size
  return kSmoothFonts[cfgFont % 5][0];
}

static float smoothLocalScale(const BadgeSmoothFont *f, float scale) {
  if (!f || !f->pxSize) return scale;
  return ((float)BADGE_FONT_MASTER_PX * scale) / (float)f->pxSize;
}

// ---- prefs --------------------------------------------------------------

void refreshTowerBest() {
  // Shared NVS namespace with HotelTower.ino (prefs.begin("tower") / "best")
  Preferences tower;
  if (!tower.begin("tower", true)) {
    Serial.println("tower NVS open failed");
    return;
  }
  const uint32_t v = tower.getUInt("best", 0);
  tower.end();
  if (v != towerBest) {
    Serial.printf("tower best %lu -> %lu\n", (unsigned long)towerBest,
                  (unsigned long)v);
  }
  towerBest = v;
}

void loadBadgeStats() {
  Preferences badge;
  if (badge.begin("badge", true)) {
    scanCount = badge.getUInt("scans", 0);
    statusIdx = badge.getUChar("status", 0);
    if (statusIdx >= kStatusN) statusIdx = 0;
    cfgSurface = badge.getUChar("surface", BADGE_SURFACE);
    cfgPalette = badge.getUChar("palette", BADGE_PALETTE);
    if (cfgPalette >= kAccentN) cfgPalette = BADGE_PALETTE % kAccentN;
    cfgTicks = badge.getUChar("ticks", BADGE_TICKS);
    cfgFont = badge.getUChar("font", BADGE_FONT);
    if (cfgFont >= 5) cfgFont = 0;
    iceIdx = badge.getUChar("ice", 0);
    if (iceIdx >= kIcebreakerN) iceIdx = 0;
    badge.end();
  }
  refreshTowerBest();
  applyTheme();
}

void persistTheme() {
  Preferences badge;
  if (!badge.begin("badge", false)) return;
  badge.putUChar("surface", cfgSurface);
  badge.putUChar("palette", cfgPalette);
  badge.putUChar("ticks", cfgTicks);
  badge.putUChar("font", cfgFont);
  badge.putUChar("status", statusIdx);
  badge.putUChar("ice", iceIdx);
  badge.end();
}

void persistTowerBest() {
  // Hotel Tower owns this key; badge only reads it.
}

void bumpScanCount() {
  scanCount++;
  Preferences badge;
  if (badge.begin("badge", false)) {
    badge.putUInt("scans", scanCount);
    badge.end();
  }
}

// ---- doom fire ----------------------------------------------------------

void fireInit() {
  memset(fire, 0, sizeof(fire));
  firePal[0] = rgb(0, 0, 0);
  firePal[1] = rgb(40, 0, 0);
  firePal[2] = rgb(90, 10, 0);
  firePal[3] = rgb(140, 30, 0);
  firePal[4] = rgb(180, 60, 0);
  firePal[5] = rgb(220, 100, 0);
  firePal[6] = rgb(255, 140, 10);
  firePal[7] = rgb(255, 180, 40);
  firePal[8] = rgb(255, 210, 80);
  firePal[9] = rgb(255, 230, 140);
  firePal[10] = rgb(255, 245, 200);
  firePal[11] = rgb(255, 255, 230);
  firePal[12] = rgb(255, 255, 255);
}

void fireStep() {
  for (int x = 0; x < kFW; x++)
    fire[(kFH - 1) * kFW + x] = (uint8_t)(random(0, 3) ? 12 : random(4, 12));
  for (int y = 0; y < kFH - 1; y++) {
    for (int x = 0; x < kFW; x++) {
      const int i = y * kFW + x;
      int v = fire[i + kFW];
      v = (v + fire[i + kFW + (x > 0 ? -1 : 0)] +
           fire[i + kFW + (x < kFW - 1 ? 1 : 0)] +
           fire[i + (y < kFH - 2 ? 2 * kFW : kFW)]) /
          4;
      fire[i] = (uint8_t)(v > 0 ? v - 1 : 0);
    }
  }
}

void fireDraw() {
  const int scale = 8;
  const int ox = (LCD_WIDTH - kFW * scale) / 2;
  const int oy = LCD_HEIGHT - kFH * scale - 20;
  for (int y = 0; y < kFH; y++) {
    for (int x = 0; x < kFW; x++) {
      const uint8_t v = fire[y * kFW + x];
      if (!v) continue;
      gfx->fillRect(ox + x * scale, oy + y * scale, scale, scale, firePal[v]);
    }
  }
}

void enterDoom() {
  touchDown = false;
  holdArmed = false;
  holdSnapValid = false;
  holdMenuFired = false;
  holdLaps = 0.0f;
  menuOpen = false;
  fireInit();
  animStartMs = millis();
  doomBootTried = false;
  state = UI_DOOM_ANIM;
  panel->setBrightness(220);
  Serial.println("Doom fire intro");
}

void bootSubtype(esp_partition_subtype_t sub) {
  const esp_partition_t *p =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, NULL);
  if (!p) {
    Serial.printf("bootSubtype: partition subtype %d not found\n", (int)sub);
    return;
  }
  if (esp_ota_set_boot_partition(p) != ESP_OK) {
    Serial.println("bootSubtype: set_boot_partition failed");
    return;
  }
  delay(120);
  esp_restart();
}

// ---- drawing helpers ----------------------------------------------------

static uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a) {
  if (a == 0) return bg;
  if (a >= 255) return fg;
  const int fr = (fg >> 11) & 31, fg6 = (fg >> 5) & 63, fb = fg & 31;
  const int br = (bg >> 11) & 31, bg6 = (bg >> 5) & 63, bb = bg & 31;
  const int r = (fr * a + br * (255 - a) + 127) / 255;
  const int g = (fg6 * a + bg6 * (255 - a) + 127) / 255;
  const int b = (fb * a + bb * (255 - a) + 127) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void plotCover(uint16_t *fb, int ix, int iy, uint16_t color, uint8_t a) {
  if ((unsigned)ix >= LCD_WIDTH || (unsigned)iy >= LCD_HEIGHT || a < 6) return;
  fb[iy * LCD_WIDTH + ix] = blend565(color, fb[iy * LCD_WIDTH + ix], a);
}

// Packed 4bpp (hi nibble = even index) or raw 8bpp alpha
static uint8_t smoothAlphaAt(const BadgeSmoothFont *f, const uint8_t *alpha,
                             int w, int h, int x, int y) {
  if (x < 0 || y < 0 || x >= w || y >= h) return 0;
  const uint32_t i = (uint32_t)y * (uint32_t)w + (uint32_t)x;
  if (f->bpp == 8) return alpha[i];
  const uint8_t packed = alpha[i >> 1];
  const uint8_t n = (i & 1u) ? (packed & 0x0Fu) : (packed >> 4);
  return (uint8_t)(n * 17);  // 0..15 → 0..255
}

static uint8_t smoothAlphaBilinear(const BadgeSmoothFont *f, const uint8_t *alpha,
                                   int w, int h, float fx, float fy) {
  const int x0 = (int)floorf(fx);
  const int y0 = (int)floorf(fy);
  const float tx = fx - (float)x0;
  const float ty = fy - (float)y0;
  const float v00 = (float)smoothAlphaAt(f, alpha, w, h, x0, y0);
  const float v10 = (float)smoothAlphaAt(f, alpha, w, h, x0 + 1, y0);
  const float v01 = (float)smoothAlphaAt(f, alpha, w, h, x0, y0 + 1);
  const float v11 = (float)smoothAlphaAt(f, alpha, w, h, x0 + 1, y0 + 1);
  const float v0 = v00 + (v10 - v00) * tx;
  const float v1 = v01 + (v11 - v01) * tx;
  return (uint8_t)(v0 + (v1 - v0) * ty + 0.5f);
}

// Discrete baked sizes + residual local scale (prefer near 1:1 over big upsample).
// (cx,cy) is the glyph pen / baseline origin. xShiftPx shifts along local +X
// (pre-rotate), in output pixels — used to centre glyphs on arc text.
void drawRotatedGfxChar(float cx, float cy, char ch, float angleRad,
                        float scale, uint16_t color, float xShiftPx) {
  const BadgeSmoothFont *f = pickSmoothFont(scale);
  if (!f || (uint8_t)ch < f->first || (uint8_t)ch > f->last) return;
  const BadgeSmoothGlyph *g = &f->glyphs[(uint8_t)ch - f->first];
  if (!g->width || !g->height) return;
  const uint8_t *alpha = f->alpha + g->bitmapOffset;
  const float local = smoothLocalScale(f, scale);
  const float xOff =
      (float)g->xOffset + (local > 0.01f ? xShiftPx / local : 0.0f);
  const float yOff = (float)g->yOffset;
  const float ca = cosf(angleRad), sa = sinf(angleRad);
  const bool axis = fabsf(angleRad) < 0.02f;
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;

  // Axis-aligned: snap glyph box to integer pixels so every letter shares the
  // same baseline row (stops 1px bob from fractional bilinear phase).
  if (axis) {
    const int destX = (int)lroundf(cx + xOff * local);
    const int destY = (int)lroundf(cy + yOff * local);
    const int destW = max(1, (int)lroundf((float)g->width * local));
    const int destH = max(1, (int)lroundf((float)g->height * local));
    const bool neat = (destW == g->width && destH == g->height);
    for (int yy = 0; yy < destH; yy++) {
      const int iy = destY + yy;
      if ((unsigned)iy >= LCD_HEIGHT) continue;
      for (int xx = 0; xx < destW; xx++) {
        const int ix = destX + xx;
        if ((unsigned)ix >= LCD_WIDTH) continue;
        uint8_t a;
        if (neat) {
          a = smoothAlphaAt(f, alpha, g->width, g->height, xx, yy);
        } else {
          const float fx = ((float)xx + 0.5f) * (float)g->width / (float)destW;
          const float fy = ((float)yy + 0.5f) * (float)g->height / (float)destH;
          a = smoothAlphaBilinear(f, alpha, g->width, g->height, fx, fy);
        }
        if (a >= 6) plotCover(fb, ix, iy, color, a);
      }
    }
    return;
  }

  // Rotated (rim labels): bilinear sample around the baseline origin
  {
    const float corners[4][2] = {
        {xOff, yOff},
        {xOff + g->width, yOff},
        {xOff, yOff + g->height},
        {xOff + g->width, yOff + g->height},
    };
    float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
    for (int i = 0; i < 4; i++) {
      const float lx = corners[i][0] * local;
      const float ly = corners[i][1] * local;
      const float wx = cx + lx * ca - ly * sa;
      const float wy = cy + lx * sa + ly * ca;
      if (wx < minX) minX = wx;
      if (wy < minY) minY = wy;
      if (wx > maxX) maxX = wx;
      if (wy > maxY) maxY = wy;
    }
    const int ix0 = (int)floorf(minX) - 1;
    const int iy0 = (int)floorf(minY) - 1;
    const int ix1 = (int)ceilf(maxX) + 1;
    const int iy1 = (int)ceilf(maxY) + 1;
    for (int iy = iy0; iy < iy1; iy++) {
      if ((unsigned)iy >= LCD_HEIGHT) continue;
      for (int ix = ix0; ix < ix1; ix++) {
        if ((unsigned)ix >= LCD_WIDTH) continue;
        const float dx = (float)ix + 0.5f - cx;
        const float dy = (float)iy + 0.5f - cy;
        const float lx = (dx * ca + dy * sa) / local;
        const float ly = (-dx * sa + dy * ca) / local;
        const float fx = lx - xOff;
        const float fy = ly - yOff;
        if (fx < -1.0f || fy < -1.0f || fx > g->width + 1.0f ||
            fy > g->height + 1.0f)
          continue;
        const uint8_t a =
            smoothAlphaBilinear(f, alpha, g->width, g->height, fx, fy);
        if (a >= 6) plotCover(fb, ix, iy, color, a);
      }
    }
  }
}

// Pixel advance for this call-site scale (uses nearest bake + residual scale)
static float gfxCharAdvancePx(char ch, float scale) {
  const BadgeSmoothFont *f = pickSmoothFont(scale);
  if (!f || (uint8_t)ch < f->first || (uint8_t)ch > f->last)
    return 8.0f * scale;
  const BadgeSmoothGlyph *g = &f->glyphs[(uint8_t)ch - f->first];
  const float local = smoothLocalScale(f, scale);
  const float adv =
      g->xAdvance > 0 ? (float)g->xAdvance : (float)(g->width + 1);
  return adv * local;
}

void drawFontTextAt(const char *s, float x, float baselineY, float scale,
                    uint16_t color) {
  if (!s) return;
  // Shared integer baseline + integer pen X — no per-glyph float re-round
  const float baseY = (float)lroundf(baselineY);
  float pen = x;
  for (const char *p = s; *p; ++p) {
    if ((uint8_t)*p >= 128) continue;
    const float penX = (float)lroundf(pen);
    drawRotatedGfxChar(penX, baseY, *p, 0.0f, scale, color, 0.0f);
    pen += gfxCharAdvancePx(*p, scale);
  }
}

static float fontTextWidth(const char *s, float scale) {
  float w = 0;
  if (!s) return 0;
  for (const char *p = s; *p; ++p) {
    if ((uint8_t)*p < 128) w += gfxCharAdvancePx(*p, scale);
  }
  return w;
}

void drawFontTextCX(const char *s, float baselineY, float scale,
                    uint16_t color) {
  drawFontTextAt(s, kCx - fontTextWidth(s, scale) * 0.5f, baselineY, scale,
                 color);
}

void drawFontTextLeft(const char *s, float x, float baselineY, float scale,
                      uint16_t color) {
  drawFontTextAt(s, x, baselineY, scale, color);
}

void drawFontTextRight(const char *s, float rightX, float baselineY,
                       float scale, uint16_t color) {
  drawFontTextAt(s, rightX - fontTextWidth(s, scale), baselineY, scale, color);
}

int drawWrappedFontCX(const char *s, float topBaseline, float scale,
                      uint16_t color, float maxW) {
  char buf[160];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  const BadgeSmoothFont *f = pickSmoothFont(scale);
  const float local = smoothLocalScale(f, scale);
  const float lineH =
      ((f && f->yAdvance) ? (float)f->yAdvance : 28.0f) * local + 6.0f;
  float y = topBaseline;
  char *start = buf;
  int lines = 0;
  while (*start && lines < 6) {
    // Prefer breaking on spaces; never leave a one-letter orphan if avoidable
    char *fit = start + strlen(start);
    if (fontTextWidth(start, scale) > maxW) {
      char *best = nullptr;
      for (char *p = start; *p; ++p) {
        if (*p != ' ') continue;
        *p = 0;
        if (fontTextWidth(start, scale) <= maxW) best = p;
        *p = ' ';
        if (!best && p > start) {
          // first space already too wide — will hard-break below
        }
        if (fontTextWidth(start, scale) > maxW && best) break;
      }
      if (best) {
        fit = best;
      } else {
        fit = start + strlen(start);
        while (fit > start + 1 && fontTextWidth(start, scale) > maxW) {
          fit--;
          char saved = *fit;
          *fit = 0;
          bool ok = fontTextWidth(start, scale) <= maxW;
          *fit = saved;
          if (ok) break;
        }
      }
    }
    char saved = *fit;
    *fit = 0;
    if (*start) {
      drawFontTextCX(start, y, scale, color);
      y += lineH;
      lines++;
    }
    *fit = saved;
    start = fit;
    if (*start == ' ') start++;
    if (start == fit && *start) start++;
  }
  return (int)y;
}

void drawArcText(const char *text, float radius, float centerDeg, bool bottom,
                 uint16_t color, float scale) {
  if (!text || !*text) return;
  float total = 0;
  for (const char *p = text; *p; ++p) {
    if ((uint8_t)*p < 128) total += gfxCharAdvancePx(*p, scale);
  }
  // Top L→R: increasing angle. Bottom L→R: decreasing angle.
  const float dir = bottom ? -1.0f : 1.0f;
  const float centerRad = centerDeg * (float)M_PI / 180.0f;
  // Layout along arc length, centre each glyph on its span (stable baseline)
  float cursor = -total * 0.5f;
  for (const char *p = text; *p; ++p) {
    if ((uint8_t)*p >= 128) continue;
    const float adv = gfxCharAdvancePx(*p, scale);
    const float s = cursor + adv * 0.5f;
    const float theta = centerRad + dir * (s / radius);
    const float rot =
        theta + (float)M_PI / 2.0f + (bottom ? (float)M_PI : 0.0f);
    const float x = kCx + cosf(theta) * radius;
    const float y = kCy + sinf(theta) * radius;
    drawRotatedGfxChar(x, y, *p, rot, scale, color, -adv * 0.5f);
    cursor += adv;
  }
}

// Tiny UI chrome still uses the built-in font (menu shorts, button glyphs).
int textWidthDefault(const char *s, uint8_t size) {
  return (int)strlen(s) * 6 * size;
}

void drawTextCX(const char *s, int cy, uint8_t size, uint16_t color) {
  gfx->setFont();
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  const int w = textWidthDefault(s, size);
  gfx->setCursor(kCx - w / 2, cy - (4 * size));
  gfx->print(s);
}

void drawTextLeft(const char *s, int x, int y, uint8_t size, uint16_t color) {
  gfx->setFont();
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(s);
}

void drawTextRight(const char *s, int rightX, int y, uint8_t size,
                   uint16_t color) {
  drawTextLeft(s, rightX - textWidthDefault(s, size), y, size, color);
}

void drawCompanyMark(int cx, int cy, int h) {
  // Company logo from config bitmaps — light on black surface, dark on sand
  const uint16_t *src =
      (cfgSurface == 1) ? company_mark_dark_rgb565 : company_mark_light_rgb565;
  const float scale = (float)h / (float)COMPANY_MARK_H;
  const int dw = (int)(COMPANY_MARK_W * scale + 0.5f);
  const int dh = h;
  const int ox = cx - dw / 2;
  const int oy = cy - dh / 2;
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;
  for (int y = 0; y < dh; y++) {
    const int sy = y * COMPANY_MARK_H / dh;
    const int iy = oy + y;
    if ((unsigned)iy >= LCD_HEIGHT) continue;
    for (int x = 0; x < dw; x++) {
      const int sx = x * COMPANY_MARK_W / dw;
      const uint16_t c = src[sy * COMPANY_MARK_W + sx];
      if (c == COMPANY_MARK_KEY) continue;
      const int ix = ox + x;
      if ((unsigned)ix >= LCD_WIDTH) continue;
      fb[iy * LCD_WIDTH + ix] = c;
    }
  }
}

void drawIdentity() {
  // Design name ~56px → scale 2.0 vs master 28; picks 42px bake (~1.33×)
  float nameScale = 2.0f;
  while (nameScale > 1.4f && fontTextWidth(BADGE_NAME_L2, nameScale) > 340.0f)
    nameScale -= 0.1f;
  drawFontTextCX(BADGE_NAME_L1, kCy - 48.0f, nameScale, theme.fg);
  drawFontTextCX(BADGE_NAME_L2, kCy + 10.0f, nameScale, theme.fg);
  gfx->fillRect(kCx - 48, kCy + 38, 96, 2, theme.accent);

  const float titleScale = 0.85f;
  const float tw = fontTextWidth(BADGE_TITLE, titleScale);
  const int markH = 38;
  const int gap = 12;
  const float markBlock =
      (float)COMPANY_MARK_W * ((float)markH / (float)COMPANY_MARK_H);
  const float total = tw + gap + markBlock;
  const float left = kCx - total * 0.5f;
  const float markCy = kCy + 78.0f;

  // Title baseline so cap-box centre matches the logo centre
  float titleBaseline = markCy;
  const BadgeSmoothFont *f = pickSmoothFont(titleScale);
  const float local = smoothLocalScale(f, titleScale);
  if (f && (uint8_t)'H' >= f->first && (uint8_t)'H' <= f->last) {
    const BadgeSmoothGlyph *g = &f->glyphs['H' - f->first];
    const float capMid =
        ((float)g->yOffset + (float)g->height * 0.5f) * local;
    // Slight extra down-bias — caps look high against the mark
    titleBaseline = markCy - capMid + 2.0f;
  }
  drawFontTextAt(BADGE_TITLE, left, titleBaseline, titleScale, theme.dim);
  drawCompanyMark((int)(left + tw + gap + markBlock * 0.5f), (int)markCy, markH);
}

void drawRoundBtn(int x, int y, int w, int h, uint16_t fill, uint16_t border,
                  const char *label, uint16_t labelCol, int flashId) {
  uint16_t f = fill;
  if (flashId >= 0 && btnFlashId == flashId && millis() < btnFlashUntil)
    f = theme.hover;
  gfx->fillRoundRect(x, y, w, h, h / 2, f);
  if (border != fill) gfx->drawRoundRect(x, y, w, h, h / 2, border);
  const float scale = 0.65f;
  const float tw = fontTextWidth(label, scale);
  drawFontTextAt(label, x + (w - tw) * 0.5f, y + h * 0.62f, scale, labelCol);
}

void drawArrowBtn(int x, int y, int w, int h, bool right, int flashId) {
  uint16_t fill = theme.bg;
  if (btnFlashId == flashId && millis() < btnFlashUntil) fill = theme.hover;
  gfx->fillRoundRect(x, y, w, h, h / 2, fill);
  gfx->drawRoundRect(x, y, w, h, h / 2, theme.line);
  const int cx = x + w / 2;
  const int cy = y + h / 2;
  // Proper arrowheads (not chevron glyphs)
  if (right) {
    gfx->fillTriangle(cx - 10, cy - 14, cx - 10, cy + 14, cx + 16, cy, theme.dim);
  } else {
    gfx->fillTriangle(cx + 10, cy - 14, cx + 10, cy + 14, cx - 16, cy, theme.dim);
  }
}

bool tapInRect(int x, int y, int w, int h) {
  return tapX >= x && tapX < x + w && tapY >= y && tapY < y + h;
}

bool tapInCircle(int16_t cx, int16_t cy, int16_t r) {
  const int32_t dx = tapX - cx, dy = tapY - cy;
  return dx * dx + dy * dy <= (int32_t)r * r;
}

void drawDialTicks() {
  if (!cfgTicks) return;
  for (int i = 0; i < 60; i++) {
    const float a = i * 6.0f * (float)M_PI / 180.0f - (float)M_PI / 2;
    const bool major = (i % 5) == 0;
    const float r0 = major ? 198.0f : 203.0f;
    const float r1 = 210.0f;
    const uint16_t col = major ? theme.faint : theme.line;
    gfx->drawLine((int)(kCx + cosf(a) * r0), (int)(kCy + sinf(a) * r0),
                  (int)(kCx + cosf(a) * r1), (int)(kCy + sinf(a) * r1), col);
  }
}

void drawProgressArc() {
  const float seg = kCirc / (float)FACE_COUNT;
  const float dash = seg - 8.0f;
  gfx->drawCircle(kCx, kCy, (int)kOuterR, theme.line2);
  gfx->drawCircle(kCx, kCy, (int)kOuterR - 1, theme.line2);
  // 12 o'clock, then clockwise as the face index increases
  const float startDeg = -90.0f + (float)badgeFace * (360.0f / FACE_COUNT);
  const float spanDeg = (dash / kCirc) * 360.0f;
  gfx->fillArc(kCx, kCy, (int)kOuterR + 1, (int)kOuterR - 2, startDeg,
               startDeg + spanDeg, theme.accent);
}

void drawRimLabels() {
  // Top: caps grow outward from baseline. Bottom: caps grow inward, so use a
  // larger radius or the label sits too far from the dial edge.
  drawArcText(kFaceTop[badgeFace], 186.0f, -90.0f, false, theme.dim, 1.0f);
  drawArcText(kFaceBot[badgeFace], 208.0f, 90.0f, true, theme.accent, 1.0f);
}

void drawChrome() {
  gfx->fillScreen(theme.bg);
  drawDialTicks();
  drawProgressArc();
  drawRimLabels();
}

void drawMenuRing() {
  // Fully opaque overlay — hide the page underneath
  gfx->fillCircle(kCx, kCy, 233, theme.bg);
  gfx->drawCircle(kCx, kCy, 166, theme.line2);
  for (int n = 0; n < FACE_COUNT; n++) {
    const float a =
        (n / (float)FACE_COUNT) * 2.0f * (float)M_PI - (float)M_PI / 2;
    const int x = (int)(kCx + cosf(a) * 166.0f);
    const int y = (int)(kCy + sinf(a) * 166.0f);
    const bool on = n == (int)badgeFace;
    gfx->drawCircle(x, y, 42, on ? theme.accent : theme.line);
    const char *lab = kFaceShort[n];
    const float sc = 0.75f;
    const float tw = fontTextWidth(lab, sc);
    drawFontTextAt(lab, x - tw * 0.5f, y + 5.0f, sc,
                   on ? theme.accent : theme.fg);
  }
  gfx->drawCircle(kCx, kCy, 56, theme.line);
  drawFontTextCX("CLOSE", kCy + 6.0f, 0.85f, theme.dim);
}

void drawConnect() {
  // Sized for camera scan without crowding rim labels.
  // Finder corners stay inside the disc; quiet zone from disc ground.
  const int discR = 172;
  const int qrSize = 236;
  const int ox = kCx - qrSize / 2;
  const int oy = kCy - qrSize / 2;
  // Scanners need dark modules on a light quiet zone. Sand already is light;
  // black surface gets a white plate so the QR stays scannable.
  const bool lightPlate = (cfgSurface == 0);
  const uint16_t groundCol =
      lightPlate ? rgb(0xFF, 0xFF, 0xFF) : theme.disc;
  const uint16_t moduleCol =
      lightPlate ? rgb(0x1A, 0x19, 0x17) : theme.fg;

  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;

  gfx->fillCircle(kCx, kCy, discR + 1, theme.line);
  gfx->fillCircle(kCx, kCy, discR, groundCol);

  // Mock QR lattice in the disc caps (outside the QR square, inside discR) —
  // same expansion look as the old full-circle QR screen.
  constexpr int kQrModules = 41;
  const int mod = max(3, (qrSize + kQrModules / 2) / kQrModules);
  const int discR2 = discR * discR;
  int onN = 0, totN = 0;
  for (int sy = 8; sy < QR_HALFTONE_H - 8; sy += 5) {
    for (int sx = 8; sx < QR_HALFTONE_W - 8; sx += 5) {
      const uint16_t src = kQrHalftoneRgb565[sy * QR_HALFTONE_W + sx];
      const int r5 = (src >> 11) & 31;
      const int g6 = (src >> 5) & 63;
      const int b5 = src & 31;
      if ((r5 * 2 + g6 + b5) < 70) onN++;
      totN++;
    }
  }
  const int onThresh = totN > 0 ? (onN * 1000) / totN : 350;

  const int mx0 = -((discR + (kCx - ox)) / mod) - 1;
  const int my0 = -((discR + (kCy - oy)) / mod) - 1;
  const int mx1 = (discR + (ox + qrSize - kCx)) / mod + 2;
  const int my1 = (discR + (oy + qrSize - kCy)) / mod + 2;
  for (int my = my0; my <= my1; my++) {
    for (int mx = mx0; mx <= mx1; mx++) {
      const int x0 = ox + mx * mod;
      const int y0 = oy + my * mod;
      const int cx = x0 + mod / 2;
      const int cy = y0 + mod / 2;
      // Skip cells whose centre sits in the real QR square
      if (cx >= ox && cx < ox + qrSize && cy >= oy && cy < oy + qrSize)
        continue;
      const int dx = cx - kCx;
      const int dy = cy - kCy;
      if (dx * dx + dy * dy > (discR + mod) * (discR + mod)) continue;

      uint32_t h = (uint32_t)(mx * 374761393u) ^ (uint32_t)(my * 668265263u);
      h ^= h >> 13;
      h *= 1274126177u;
      const uint16_t color =
          ((int)(h % 1000u) < onThresh) ? moduleCol : groundCol;

      for (int yy = y0; yy < y0 + mod; yy++) {
        if ((unsigned)yy >= LCD_HEIGHT) continue;
        const int ddy = yy - kCy;
        for (int xx = x0; xx < x0 + mod; xx++) {
          if ((unsigned)xx >= LCD_WIDTH) continue;
          const int ddx = xx - kCx;
          if (ddx * ddx + ddy * ddy > discR2) continue;
          if (xx >= ox && xx < ox + qrSize && yy >= oy && yy < oy + qrSize)
            continue;
          fb[yy * LCD_WIDTH + xx] = color;
        }
      }
    }
  }

  // Real QR on top (clipped to disc)
  const int src0 = 6;
  const int srcSpan = QR_HALFTONE_W - 2 * src0;
  for (int y = 0; y < qrSize; y++) {
    const int sy = src0 + y * srcSpan / qrSize;
    for (int x = 0; x < qrSize; x++) {
      const int sx = src0 + x * srcSpan / qrSize;
      const int dx = (ox + x) - kCx, dy = (oy + y) - kCy;
      if (dx * dx + dy * dy > discR2) continue;
      const uint16_t src = kQrHalftoneRgb565[sy * QR_HALFTONE_W + sx];
      const int r5 = (src >> 11) & 31;
      const int g6 = (src >> 5) & 63;
      const int b5 = src & 31;
      const bool isModule = (r5 * 2 + g6 + b5) < 70;
      fb[(oy + y) * LCD_WIDTH + (ox + x)] = isModule ? moduleCol : groundCol;
    }
  }
  drawRimLabels();
}

void drawSchedule() {
  const int R = 146;
  gfx->drawCircle(kCx, kCy, R, theme.line2);
  gfx->drawCircle(kCx, kCy, R - 1, theme.line2);
  const float pct = BADGE_SESSION_PCT / 100.0f;
  const float span = pct * 360.0f;
  gfx->fillArc(kCx, kCy, R + 2, R - 4, -90.0f, -90.0f + span, theme.accent);
  drawFontTextCX(BADGE_SESSION_TIME, kCy - 40.0f, 2.45f, theme.fg);
  drawFontTextCX(BADGE_SESSION_IN, kCy + 28.0f, 0.7f, theme.accent);
  gfx->drawFastHLine(kCx - 60, kCy + 42, 120, theme.line);
  drawFontTextCX("AGENTIC BROWSING", kCy + 68.0f, 0.9f, theme.fg);
  drawFontTextCX("AT SCALE", kCy + 96.0f, 0.9f, theme.fg);
}

uint16_t statusColor() {
  const uint8_t t = kStatusTone[statusIdx % kStatusN];
  if (t == 0) return theme.accent;
  if (t == 1) return theme.dim;
  return rgb32(kStatusToneRgb[t]);
}

void drawStatus() {
  gfx->fillCircle(kCx, kCy - 112, 32, statusColor());
  const char *label = kStatusLabels[statusIdx % kStatusN];
  char buf[40];
  strncpy(buf, label, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  char *sp = strchr(buf, ' ');
  if (sp) {
    *sp = 0;
    drawFontTextCX(buf, kCy - 36.0f, 1.55f, theme.fg);
    drawFontTextCX(sp + 1, kCy + 6.0f, 1.55f, theme.fg);
  } else {
    drawFontTextCX(buf, kCy - 12.0f, 1.55f, theme.fg);
  }
  drawArrowBtn(kCx - 126, kCy + 48, 118, 72, false, 0);
  drawArrowBtn(kCx + 8, kCy + 48, 118, 72, true, 1);
}

void drawInbox() {
  if (notifIdx >= kNotifN) {
    drawFontTextCX("ALL CLEAR", kCy - 20.0f, 1.8f, theme.faint);
  } else {
    drawFontTextCX(kNotifs[notifIdx].name, kCy - 70.0f, 1.7f, theme.fg);
    // Wide max so short bodies stay on one line; wrap only if needed.
    drawWrappedFontCX(kNotifs[notifIdx].body, kCy - 18.0f, 0.95f, theme.dim,
                      340);
  }
  drawRoundBtn(kCx - 126, kCy + 42, 118, 64, theme.bg, theme.line, "LATER",
               theme.dim, 0);
  drawRoundBtn(kCx + 8, kCy + 42, 118, 64, theme.accent, theme.accent, "ACCEPT",
               theme.ink, 1);
}

void drawSystem() {
  int pct = powerBatteryPercent();
  if (pct < 0) pct = 78;
  const int R = 143;
  gfx->fillArc(kCx, kCy, R + 4, R - 6, 115.0f, 115.0f + 240.0f, theme.line2);
  const float span = 240.0f * (pct / 100.0f);
  gfx->fillArc(kCx, kCy, R + 4, R - 6, 115.0f, 115.0f + span, theme.accent);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  drawFontTextCX(buf, kCy - 36.0f, 2.5f, theme.fg);
  gfx->drawFastHLine(kCx - 83, kCy + 16, 166, theme.line);
  const float rowX = kCx - 83;
  const float rowR = kCx + 83;
  float y = kCy + 42.0f;
  const float sc = 0.85f;
  drawFontTextLeft("BLE", rowX, y, sc, theme.faint);
  drawFontTextRight("4 PAIRED", rowR, y, sc, theme.accent);
  y += 26;
  drawFontTextLeft("WIFI", rowX, y, sc, theme.faint);
  drawFontTextRight(BADGE_WIFI_LABEL, rowR, y, sc, theme.fg);
  y += 26;
  drawFontTextLeft("SYNC", rowX, y, sc, theme.faint);
  drawFontTextRight("2 MIN", rowR, y, sc, theme.fg);
}

void drawIcebreaker() {
  // Explicit multi-line so long openers aren't cropped to a few words
  const char *t = kIcebreakers[iceIdx % kIcebreakerN];
  drawWrappedFontCX(t, kCy - 89.0f, 1.25f, theme.fg, 268);
  // Fixed under the text block (below the tallest wrap), not per-prompt.
  constexpr int kDotY = kCy + 78;
  const int n = kIcebreakerN;
  const int totalW = n * 7 + (n - 1) * 7;
  int x = kCx - totalW / 2;
  for (int i = 0; i < n; i++) {
    gfx->fillCircle(x + 3, kDotY, 3,
                    i == (int)(iceIdx % kIcebreakerN) ? theme.accent
                                                      : theme.line);
    x += 14;
  }
}

void drawArcade() {
  refreshTowerBest();
  drawFontTextCX("HOTEL", kCy - 55.0f, 1.85f, theme.fg);
  drawFontTextCX("TOWER", kCy - 5.0f, 1.85f, theme.fg);
  char best[28];
  if (towerBest == 0) {
    snprintf(best, sizeof(best), "NO BEST YET");
  } else {
    snprintf(best, sizeof(best), "BEST: %lu FLOORS", (unsigned long)towerBest);
  }
  drawFontTextCX(best, kCy + 42.0f, 0.85f, theme.faint);
  drawRoundBtn(kCx - 86, kCy + 70, 172, 60, theme.accent, theme.accent,
               "TAP TO PLAY", theme.ink, 0);
}

void drawRadar() {
  gfx->drawCircle(kCx, kCy, 150, theme.line2);
  gfx->drawCircle(kCx, kCy, 98, theme.line3);
  gfx->drawCircle(kCx, kCy, 46, theme.line3);
  // Sweep wedge
  const float a0 = radarAngle;
  for (int i = 0; i < 22; i++) {
    const float a = a0 + i * 3.0f;
    const float rad = a * (float)M_PI / 180.0f;
    const uint8_t alpha = (uint8_t)(180 - i * 8);
    const int x1 = kCx + (int)(cosf(rad) * 150);
    const int y1 = kCy + (int)(sinf(rad) * 150);
    uint16_t *fb = gfx->getFramebuffer();
    if (!fb) continue;
    // soft line by plotting along radius
    for (int r = 20; r < 150; r += 2) {
      const int x = kCx + (int)(cosf(rad) * r);
      const int y = kCy + (int)(sinf(rad) * r);
      if ((unsigned)x >= LCD_WIDTH || (unsigned)y >= LCD_HEIGHT) continue;
      fb[y * LCD_WIDTH + x] =
          blend565(theme.accent, fb[y * LCD_WIDTH + x], alpha / 3);
    }
    (void)x1;
    (void)y1;
  }
  for (int i = 0; i < kRadarN; i++) {
    const float a = kRadar[i].angleDeg * (float)M_PI / 180.0f;
    const int x = kCx + (int)(cosf(a) * kRadar[i].radius);
    const int y = kCy + (int)(sinf(a) * kRadar[i].radius);
    gfx->fillCircle(x, y - 10, 6, theme.accent);
    const float sc = 0.7f;
    const float tw = fontTextWidth(kRadar[i].name, sc);
    drawFontTextAt(kRadar[i].name, x - tw * 0.5f, y + 14.0f, sc, theme.fg);
  }
  char nbuf[4];
  snprintf(nbuf, sizeof(nbuf), "%d", kRadarN);
  drawFontTextCX(nbuf, kCy + 4.0f, 2.4f, theme.fg);
  drawFontTextCX("NEARBY", kCy + 36.0f, 0.75f, theme.dim);
}

// Shared settings list geometry — draw + hit must stay in lockstep.
static constexpr int kSettingsRowH = 72;
static constexpr int kSettingsRows = 4;
static constexpr int kSettingsListH = kSettingsRowH * kSettingsRows;
static constexpr int kSettingsTop = kCy - kSettingsListH / 2;
static constexpr int kSettingsLeft = kCx - 160;
static constexpr int kSettingsW = 320;

void drawSettings() {
  const float left = (float)kSettingsLeft;
  const float right = (float)(kSettingsLeft + kSettingsW);
  char tickVal[4];
  snprintf(tickVal, sizeof(tickVal), cfgTicks ? "ON" : "OFF");
  const char *labels[4] = {"SURFACE", "ACCENT", "TYPE", "DIAL"};
  const char *values[4] = {surfaceName(), paletteName(), fontName(), tickVal};
  for (int i = 0; i < kSettingsRows; i++) {
    const int rowTop = kSettingsTop + i * kSettingsRowH;
    const float baseline = rowTop + kSettingsRowH * 0.62f;
    if (i < kSettingsRows - 1)
      gfx->drawFastHLine((int)left, rowTop + kSettingsRowH, kSettingsW,
                         theme.line2);
    drawFontTextLeft(labels[i], left, baseline, 0.75f, theme.faint);
    drawFontTextRight(values[i], right, baseline, 0.75f, theme.fg);
  }
}

void drawFaceContent() {
  switch (badgeFace) {
    case FACE_IDENTITY: drawIdentity(); break;
    case FACE_CONNECT: drawConnect(); break;
    case FACE_SCHEDULE: drawSchedule(); break;
    case FACE_STATUS: drawStatus(); break;
    case FACE_INBOX: drawInbox(); break;
    case FACE_SYSTEM: drawSystem(); break;
    case FACE_ICEBREAKER: drawIcebreaker(); break;
    case FACE_ARCADE: drawArcade(); break;
    case FACE_RADAR: drawRadar(); break;
    case FACE_SETTINGS: drawSettings(); break;
    default: break;
  }
}

void drawBadge() {
  drawChrome();
  drawFaceContent();
  if (menuOpen) drawMenuRing();
  gfx->flush();
  faceDirty = false;
}

void drawDoomAnim() {
  gfx->fillScreen(RGB565_BLACK);
  fireStep();
  fireDraw();
  const int dots = (millis() / 350) % 4;
  char msg[16] = "lets see";
  for (int i = 0; i < dots; i++) strcat(msg, ".");
  drawTextCX(msg, 160, 3, RGB565_WHITE);
  gfx->flush();
}

// ---- navigation ---------------------------------------------------------

void goFace(int i) {
  if (i < 0 || i >= FACE_COUNT) return;
  badgeFace = (BadgeFace)i;
  menuOpen = false;
  if (badgeFace == FACE_CONNECT) bumpScanCount();
  if (badgeFace == FACE_ARCADE) refreshTowerBest();
  faceDirty = true;
}

void cycleFace(int dir) {
  goFace(((int)badgeFace + dir + FACE_COUNT) % FACE_COUNT);
}

void cycleFaceFromTap() {
  cycleFace(tapX < kCx ? -1 : 1);
}

bool tapIsOuter() { return !tapInCircle(kCx, kCy, kActionR); }

void flashBtn(int id) {
  btnFlashId = (int8_t)id;
  btnFlashUntil = millis() + 160;
  faceDirty = true;
}

void launchHotelTower() {
  Serial.println("Launching Hotel Tower (OTA_1)");
  bootSubtype(ESP_PARTITION_SUBTYPE_APP_OTA_1);
}

void handleSettingsTap() {
  if (tapIsOuter()) {
    cycleFaceFromTap();
    return;
  }
  // Full-width bands for each row — easy to hit on the round display
  for (int i = 0; i < kSettingsRows; i++) {
    const int rowTop = kSettingsTop + i * kSettingsRowH;
    if (tapInRect(kSettingsLeft - 8, rowTop, kSettingsW + 16, kSettingsRowH)) {
      if (i == 0) cfgSurface = (cfgSurface + 1) % 2;
      else if (i == 1) cfgPalette = (cfgPalette + 1) % kAccentN;
      else if (i == 2) cfgFont = (cfgFont + 1) % 5;
      else cfgTicks = cfgTicks ? 0 : 1;
      applyTheme();
      persistTheme();
      faceDirty = true;
      return;
    }
  }
}

void handleTap() {
  if (menuOpen) {
    if (tapInCircle(kCx, kCy, 56)) {
      menuOpen = false;
      faceDirty = true;
      return;
    }
    for (int n = 0; n < FACE_COUNT; n++) {
      const float a =
          (n / (float)FACE_COUNT) * 2.0f * (float)M_PI - (float)M_PI / 2;
      const int x = (int)(kCx + cosf(a) * 166.0f);
      const int y = (int)(kCy + sinf(a) * 166.0f);
      if (tapInCircle(x, y, 42)) {
        goFace(n);
        return;
      }
    }
    return;
  }

  // Outer rim: left = previous face, right = next face
  if (tapIsOuter()) {
    cycleFaceFromTap();
    return;
  }

  switch (badgeFace) {
    case FACE_STATUS:
      // Generous hit: padded well past the pills, split left/right of centre
      if (tapInRect(kCx - 150, kCy + 28, 150, 120)) {
        statusIdx = (statusIdx + kStatusN - 1) % kStatusN;
        persistTheme();
        flashBtn(0);
        return;
      }
      if (tapInRect(kCx, kCy + 28, 150, 120)) {
        statusIdx = (statusIdx + 1) % kStatusN;
        persistTheme();
        flashBtn(1);
        return;
      }
      break;
    case FACE_INBOX:
      if (tapInRect(kCx - 126, kCy + 42, 118, 64)) {
        if (notifIdx < kNotifN) notifIdx++;
        flashBtn(0);
        return;
      }
      if (tapInRect(kCx + 8, kCy + 42, 118, 64)) {
        if (notifIdx < kNotifN) notifIdx++;
        flashBtn(1);
        return;
      }
      break;
    case FACE_ICEBREAKER:
      iceIdx = (iceIdx + 1) % kIcebreakerN;
      persistTheme();
      faceDirty = true;
      return;
    case FACE_SETTINGS:
      handleSettingsTap();
      return;
    case FACE_ARCADE:
      if (tapInRect(kCx - 86, kCy + 70, 172, 60) ||
          tapInCircle(kCx, kCy, 120)) {
        flashBtn(0);
        launchHotelTower();
        return;
      }
      break;
    default:
      break;
  }
  // No face action hit — navigate by side
  cycleFaceFromTap();
}

void handleGesture(GestureEvent ev) {
  if (ev == GESTURE_HOLD_DOOM) {
    enterDoom();
    return;
  }
  if (ev == GESTURE_HOLD_MENU) {
    menuOpen = true;
    // Bake the menu into the hold snapshot so the rim arc continues on top
    drawChrome();
    drawFaceContent();
    drawMenuRing();
    captureHoldSnap();
    faceDirty = false;
    return;
  }
  if (ev == GESTURE_TAP) handleTap();
}

// ---- touch --------------------------------------------------------------

void IRAM_ATTR onTouchInterrupt() { touchPending = true; }

bool takeTouchInterrupt() {
  noInterrupts();
  const bool pending = touchPending;
  touchPending = false;
  interrupts();
  return pending;
}

float holdProgressForMs(uint32_t elapsedMs) {
  constexpr uint32_t cycleMs = kHoldLapMs + kHoldPauseMs;
  if (cycleMs == 0) return 0.0f;
  const uint32_t completed = elapsedMs / cycleMs;
  if (completed >= (uint32_t)kHoldDoomLaps) return (float)kHoldDoomLaps;
  const uint32_t within = elapsedMs % cycleMs;
  // Full ring holds during the pause; next lap starts after kHoldPauseMs.
  if (within >= kHoldLapMs) return (float)(completed + 1);
  return (float)completed + (float)within / (float)kHoldLapMs;
}

void captureHoldSnap() {
  uint16_t *fb = gfx->getFramebuffer();
  if (!holdSnap) holdSnap = allocFb("holdSnap");
  if (holdSnap && fb) {
    memcpy(holdSnap, fb, kFbBytes);
    holdSnapValid = true;
  }
}

// Lap 0 = accent, lap 4 (5th) = #FF0000. Intermediate laps lerp toward red.
uint16_t holdRingColor(int lapIndex) {
  const int i = lapIndex < 0 ? 0 : (lapIndex > kHoldDoomLaps - 1 ? kHoldDoomLaps - 1
                                                                  : lapIndex);
  const float t = (float)i / (float)(kHoldDoomLaps - 1);
  uint8_t ar = ((theme.accent >> 11) & 0x1F) * 255 / 31;
  uint8_t ag = ((theme.accent >> 5) & 0x3F) * 255 / 63;
  uint8_t ab = (theme.accent & 0x1F) * 255 / 31;
  const uint8_t r = (uint8_t)(ar + (255 - ar) * t + 0.5f);
  const uint8_t g = (uint8_t)(ag + (0 - ag) * t + 0.5f);
  const uint8_t b = (uint8_t)(ab + (0 - ab) * t + 0.5f);
  return rgb(r, g, b);
}

void drawHoldArc(float spanDeg, uint16_t color) {
  if (spanDeg <= 0.05f) return;
  if (spanDeg > 360.0f) spanDeg = 360.0f;
  gfx->fillArc(kCx, kCy, (int)kOuterR + 4, (int)kOuterR - 6, -90.0f,
               -90.0f + spanDeg, color);
}

void paintHoldFrame() {
  if (!holdArmed || !holdSnapValid || !holdSnap) return;
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;
  memcpy(fb, holdSnap, kFbBytes);

  const int completed = (int)floorf(holdLaps + 1e-4f);
  float frac = holdLaps - (float)completed;
  if (frac < 0.0f) frac = 0.0f;

  // Completed rings stay on the bezel; each new lap paints over the previous.
  const int fullRings = completed > kHoldDoomLaps ? kHoldDoomLaps : completed;
  for (int i = 0; i < fullRings; i++) {
    drawHoldArc(360.0f, holdRingColor(i));
  }
  if (frac > 0.001f && completed < kHoldDoomLaps) {
    drawHoldArc(frac * 360.0f, holdRingColor(completed));
  }
  gfx->flush();
}

GestureEvent pollGesture() {
  static uint32_t pollUntil = 0;
  static uint32_t lastReadMs = 0;
  const uint32_t now = millis();

  if (takeTouchInterrupt()) pollUntil = now + 180;
  bool gotPoint = false;
  int16_t sx = 0, sy = 0;
  if ((menuOpen || touchDown || now <= pollUntil) && now - lastReadMs >= 8) {
    lastReadMs = now;
    int16_t x, y;
    if (touch.getPoint(&x, &y, 1) > 0) {
      gotPoint = true;
      sx = LCD_WIDTH - 1 - x;
      sy = LCD_HEIGHT - 1 - y;
      tapX = sx;
      tapY = sy;
      touchLastSeenMs = now;
      lastTouchReportMs = now;
      pollUntil = now + 75;
    }
  }

  if (gotPoint && !touchDown) {
    touchDown = true;
    holdCandidate = true;
    holdArmed = false;
    holdDoomSent = false;
    holdMenuFired = false;
    holdLaps = 0.0f;
    touchStartMs = now;
    touchStartX = sx;
    touchStartY = sy;
    lastInteractionMs = now;
  }

  if (touchDown) {
    const uint32_t contactMs = touchLastSeenMs - touchStartMs;
    if (gotPoint && !holdArmed &&
        (abs(sx - touchStartX) > 28 || abs(sy - touchStartY) > 28)) {
      holdCandidate = false;
    }
    if (holdCandidate && contactMs >= kHoldArmMs) {
      if (!holdArmed) {
        holdArmed = true;
        // Snapshot the current face under the progressing rim arc
        drawChrome();
        drawFaceContent();
        if (menuOpen) drawMenuRing();
        captureHoldSnap();
        lastInteractionMs = now;
      }
      holdLaps = holdProgressForMs(now - touchStartMs - kHoldArmMs);
      if (!holdMenuFired && holdLaps >= 1.0f) {
        holdMenuFired = true;
        lastInteractionMs = now;
        return GESTURE_HOLD_MENU;
      }
      if (!holdDoomSent && holdLaps >= (float)kHoldDoomLaps) {
        holdDoomSent = true;
        holdArmed = false;
        holdSnapValid = false;
        lastInteractionMs = now;
        return GESTURE_HOLD_DOOM;
      }
    }

    if (!gotPoint && now - touchLastSeenMs > kLiftQuietMs) {
      touchDown = false;
      lastInteractionMs = now;
      const bool wasHold = holdArmed;
      const bool openedMenu = holdMenuFired;
      const bool partialOnly = wasHold && !openedMenu;
      holdArmed = false;
      holdDoomSent = false;
      holdSnapValid = false;
      holdLaps = 0.0f;
      if (wasHold) {
        // Drop the rim overlay — partial first lap must not linger.
        faceDirty = true;
        if (partialOnly) {
          // Restore the face under the aborted arc (menu never opened).
          drawBadge();
        }
        return GESTURE_NONE;
      }
      if (contactMs >= kHoldArmMs) return GESTURE_NONE;
      return GESTURE_TAP;
    }
  }
  return GESTURE_NONE;
}

// ---- serial -------------------------------------------------------------

void dumpFramebufferSnap() {
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) {
    Serial.println("SNAPERR no framebuffer");
    return;
  }
  // Ensure latest frame is painted
  drawBadge();
  const uint32_t nbytes =
      (uint32_t)LCD_WIDTH * (uint32_t)LCD_HEIGHT * sizeof(uint16_t);
  Serial.printf("SNAPOK %d %d %lu\n", LCD_WIDTH, LCD_HEIGHT,
                (unsigned long)nbytes);
  Serial.flush();
  const uint8_t *p = (const uint8_t *)fb;
  constexpr size_t kChunk = 2048;
  for (uint32_t off = 0; off < nbytes; off += kChunk) {
    const size_t n = (size_t)min((uint32_t)kChunk, nbytes - off);
    Serial.write(p + off, n);
  }
  Serial.flush();
  Serial.println();
  Serial.println("SNAPEND");
}

int parseFaceArg(const char *arg) {
  if (!arg || !*arg) return -1;
  if (arg[0] >= '0' && arg[0] <= '9' && arg[1] == 0) return arg[0] - '0';
  if (strcmp(arg, "10") == 0) return -1;
  for (int i = 0; i < FACE_COUNT; i++) {
    if (strcasecmp(arg, kFaceCode[i]) == 0) return i;
    if (strcasecmp(arg, kFaceShort[i]) == 0) return i;
  }
  // numeric multi-digit
  char *end = nullptr;
  long v = strtol(arg, &end, 10);
  if (end && *end == 0 && v >= 0 && v < FACE_COUNT) return (int)v;
  return -1;
}

void pollSerialCommands() {
  static char line[48];
  static uint8_t len = 0;
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[len] = 0;
      if (len > 0) {
        char upper[48];
        for (uint8_t i = 0; i <= len; i++) {
          const char ch = line[i];
          upper[i] = (ch >= 'a' && ch <= 'z') ? (char)(ch - 32) : ch;
        }
        if (strcmp(upper, "SNAP") == 0) {
          dumpFramebufferSnap();
        } else if (strncmp(upper, "FACE ", 5) == 0) {
          const int f = parseFaceArg(line + 5);
          if (f >= 0) {
            goFace(f);
            drawBadge();
            Serial.printf("FACEOK %d %s\n", f, kFaceCode[f]);
          } else {
            Serial.println("FACEERR");
          }
        } else if (strncmp(upper, "MENU ", 5) == 0) {
          menuOpen = (upper[5] == '1');
          faceDirty = true;
          drawBadge();
          Serial.printf("MENUOK %d\n", menuOpen ? 1 : 0);
        } else if (strcmp(upper, "TOWERBEST") == 0) {
          refreshTowerBest();
          Serial.printf("TOWERBEST %lu\n", (unsigned long)towerBest);
        } else if (strncmp(upper, "THEME SURFACE ", 14) == 0) {
          const int v = upper[14] - '0';
          if (v == 0 || v == 1) {
            cfgSurface = (uint8_t)v;
            applyTheme();
            persistTheme();
            faceDirty = true;
            drawBadge();
            Serial.printf("THEMEOK SURFACE %d\n", cfgSurface);
          } else {
            Serial.println("THEMEERR SURFACE");
          }
        } else if (strcmp(upper, "THEME RESET") == 0) {
          cfgSurface = BADGE_SURFACE;
          cfgPalette = BADGE_PALETTE;
          cfgTicks = BADGE_TICKS;
          cfgFont = BADGE_FONT;
          applyTheme();
          persistTheme();
          faceDirty = true;
          drawBadge();
          Serial.println("THEMEOK RESET");
        }
      }
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    } else {
      len = 0;
    }
  }
}

// ---- BLE ----------------------------------------------------------------

void startBleBeacon() {
#if defined(CONFIG_BT_ENABLED)
  BLEDevice::init(BADGE_BLE_NAME);
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  BLEAdvertisementData payload;
  payload.setName(BADGE_BLE_NAME);
  uint32_t urlHash = 2166136261u;
  for (const char *p = BADGE_URL; *p; ++p) {
    urlHash ^= (uint8_t)*p;
    urlHash *= 16777619u;
  }
  const char manufacturer[] = {
      0x55, 0x42, (char)(urlHash >> 24), (char)(urlHash >> 16),
      (char)(urlHash >> 8), (char)urlHash};
  payload.setManufacturerData(String(manufacturer, sizeof(manufacturer)));
  advertising->setAdvertisementData(payload);
  advertising->start();
  Serial.println("BLE intro beacon active");
#else
  Serial.println("BLE disabled in this build");
#endif
}

// ---- setup / loop -------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println();
  Serial.println("Badge OS boot…");

  powerButtonInit();
  Serial.println("power ok");

  if (!gfx->begin(80000000)) Serial.println("gfx->begin() failed!");
  else Serial.println("gfx ok");
  panel->setBrightness(220);

  touch.setPins(TP_RESET, TP_INT);
  if (touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchInterrupt, FALLING);
  } else {
    Serial.println("touch init failed!");
  }

  imuOk = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (imuOk) {
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_250Hz,
                            SensorQMI8658::LPF_MODE_0);
    qmi.enableAccelerometer();
  }

  Wire.setClock(400000);
  Wire.setTimeOut(20);

  Serial.println("Badge OS version: 2026-09-04-V1");
  loadBadgeStats();
  Serial.println("prefs ok");
  lastInteractionMs = millis();
  // BLE can stall USB-CDC bring-up on some resets; defer slightly
  delay(50);
  startBleBeacon();
  Serial.println("drawing…");
  drawBadge();
  Serial.printf("Badge OS ready: %s / %s @ %s\n", BADGE_NAME, BADGE_TITLE,
                BADGE_COMPANY);
  Serial.println("Commands: SNAP | FACE n|CODE | MENU 0|1");
}

void loop() {
  powerButtonTick();
  pollSerialCommands();

  if (state == UI_DOOM_ANIM) {
    drawDoomAnim();
    if (!doomBootTried && millis() - animStartMs > 3200) {
      doomBootTried = true;
      Serial.println("...yes it can");
      bootSubtype(ESP_PARTITION_SUBTYPE_APP_OTA_4);
      Serial.println("Doom boot failed — returning to badge");
      state = UI_BADGE;
      faceDirty = true;
      drawBadge();
    }
    return;
  }

  const GestureEvent gesture = pollGesture();
  handleGesture(gesture);

  if (btnFlashId >= 0 && millis() >= btnFlashUntil) {
    btnFlashId = -1;
    faceDirty = true;
  }
  if (badgeFace == FACE_RADAR && !holdArmed && !menuOpen) {
    static uint32_t lastRadar = 0;
    if (millis() - lastRadar > 40) {
      lastRadar = millis();
      radarAngle = fmodf(radarAngle + 4.0f, 360.0f);
      faceDirty = true;
    }
  }

  if (holdArmed) {
    paintHoldFrame();
  } else if (faceDirty) {
    drawBadge();
  }
}
