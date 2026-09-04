// HOTEL TOWER - promo game for Hotel Universe (hotel-universe.travel)
// Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466 round)
// Lives in ota_1; ConferenceBadge is the factory default on boot.
//
// Tap to drop the sliding floor. Overhang is sliced off; a complete miss
// ends the game. Build from street level up through sunset and night into
// space - reach floor 50 (five stars) and unlock a promo code.
//
// Brand palette: cream paper, black ink, Hotel Universe yellow.

#include <Arduino.h>
#include "launcher_exit.h"
#include "power_button.h"
#include "promo_code.h"
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include "TouchDrvCSTXXX.hpp"
#include "SensorQMI8658.hpp"
#include "esp_ota_ops.h"

// ---- config -------------------------------------------------------------

#define PROMO_FLOORS 50  // floors to unlock the first promo code

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

constexpr int kBlockH = 36;        // floor height in px
constexpr int kBaseW = 220;        // starting floor width
constexpr int kGroundH = 42;       // street strip
constexpr int kCameraLine = 268;   // keep the active floor around this y
constexpr float kPerfectPx = 6;    // snap threshold for a perfect drop

// ---- types (before any function: Arduino preprocessor quirk) ------------

struct Block {
  float x;  // left edge, world coords
  float w;
  uint8_t tier;  // star tier when placed: facade gets fancier
};

struct Slice {
  float x, w, worldY, vy;
  int age;
  uint8_t tier;
};

struct Spark {
  float x, y, vx, vy;
  int16_t life, maxLife;
  uint8_t r, g, b;
};

enum GameState { ST_MENU, ST_PLAYING, ST_PROMO_SPLASH, ST_GAMEOVER, ST_RESET_CONFIRM };

// ---- display pipeline (same as FighterJet/Volcano3D) --------------------

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *panel = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

class SwapCanvas : public Arduino_Canvas {
 public:
  using Arduino_Canvas::Arduino_Canvas;
  void setFB(uint16_t *f) { _framebuffer = f; }
};

SwapCanvas *gfx = new SwapCanvas(LCD_WIDTH, LCD_HEIGHT, panel);
uint16_t *bufs[2] = {nullptr, nullptr};
int curBuf = 0;
QueueHandle_t flushQueue;
SemaphoreHandle_t renderReady;

void flushTask(void *) {
  uint16_t *frame;
  for (;;) {
    if (xQueueReceive(flushQueue, &frame, portMAX_DELAY) == pdTRUE) {
      panel->draw16bitBeRGBBitmap(0, 0, frame, LCD_WIDTH, LCD_HEIGHT);
      xSemaphoreGive(renderReady);
      vTaskDelay(1);  // keep IDLE0 fed
    }
  }
}

uint16_t beColor(uint8_t r, uint8_t g, uint8_t b) {
  const uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  return __builtin_bswap16(c);
}

// ---- touch (tap anywhere = drop) ----------------------------------------

TouchDrvCST92xx touch;
bool touchOk = false;

// ---- tilt (QMI8658): subtle 3D parallax --------------------------------

SensorQMI8658 qmi;
bool imuOk = false;
float tiltX = 0, tiltY = 0;          // smoothed, in g, clamped small
float tiltTX = 0, tiltTY = 0;        // raw targets

float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void updateTilt() {
  static uint32_t lastImuMs = 0;
  if (imuOk && millis() - lastImuMs >= 80) {
    lastImuMs = millis();
    float ax, ay, az;
    if (qmi.getDataReady() && qmi.getAccelerometer(ax, ay, az)) {
      // IMU is mounted with x/y swapped relative to the screen,
      // and screen-horizontal tilt needs its sign flipped
      tiltTX = clampf(-ay, -0.45f, 0.45f);
      tiltTY = clampf(ax, -0.45f, 0.45f);
    }
  }
  tiltX += 0.12f * (tiltTX - tiltX);
  tiltY += 0.12f * (tiltTY - tiltY);
}
volatile bool touchPending = false;
uint32_t lastTouchReportMs = 0;
bool armed = true;
uint32_t armDelayUntil = 0;  // ignore taps briefly after state changes

void IRAM_ATTR onTouchInterrupt() { touchPending = true; }

bool takeTouchInterrupt() {
  noInterrupts();
  const bool pending = touchPending;
  touchPending = false;
  interrupts();
  return pending;
}

int16_t tapX = 0, tapY = 0;  // display coords of the last registered tap

// returns true once per tap press; tapX/tapY hold its position
bool pollTap() {
  bool tapped = false;
  if (touchOk && takeTouchInterrupt()) {
    int16_t x, y;
    if (touch.getPoint(&x, &y, 1) > 0) {
      lastTouchReportMs = millis();
      if (armed && millis() > armDelayUntil) {
        armed = false;
        tapped = true;
        // touch panel is mounted 180deg rotated vs the display: flip both
        tapX = LCD_WIDTH - 1 - x;
        tapY = LCD_HEIGHT - 1 - y;
      }
    }
  }
  if (!armed && millis() - lastTouchReportMs > 80) {
    armed = true;
  }
  return tapped;
}

// ---- game state ---------------------------------------------------------

Preferences prefs;
GameState state = ST_MENU;

constexpr int kMaxFloors = 400;
Block blocks[kMaxFloors + 1];
int numFloors = 0;        // floors successfully placed (excludes base)
float moveX = 0;          // sliding block left edge
float moveW = kBaseW;
int moveDir = 1;
float cam = 0;            // world height at the bottom of the screen
float camTarget = 0;
Slice slices[4];
int flashFrames = 0;      // perfect-drop flash
int perfectStreak = 0;    // consecutive perfect drops
int bonusFloors = 0;      // remaining auto-built bonus floors
uint32_t bonusNextMs = 0;
uint32_t perfectShowUntil = 0;
uint32_t best = 0;
bool promoUnlocked = false;
uint32_t gameOverAt = 0;

// Slim launcher: the side button (~4 o'clock on the bezel)
// opens the launcher.

// Persist personal best whenever the run beats it. Called after every
 // successful floor and before returning to the badge launcher — otherwise
 // exiting mid-run (or past floor 50 without dying) leaves NVS stale and the
 // badge arcade page shows an old score.
void saveBestIfNeeded() {
  if ((uint32_t)numFloors <= best) return;
  best = (uint32_t)numFloors;
  prefs.putUInt("best", best);
  Serial.printf("tower best -> %lu\n", (unsigned long)best);
}

void bootLauncher() {
  saveBestIfNeeded();
  esp_restart();
}

// side BOOT button:
//   short press (release after >=30ms) -> launcher (badge)
//   hold 5s -> factory reset confirmation, shown DURING the hold
uint32_t buttonHeldSince = 0;
bool buttonLongFired = false;

void towerButtonTick() {
  static bool inited = false;
  if (!inited) {
    inited = true;
    pinMode(0, INPUT_PULLUP);
  }
  if (digitalRead(0) == LOW) {
    if (buttonHeldSince == 0) {
      buttonHeldSince = millis();
      Serial.println("btn: down");
    }
  } else if (buttonHeldSince != 0) {
    const uint32_t held = millis() - buttonHeldSince;
    buttonHeldSince = 0;
    Serial.printf("btn: up after %lums\n", (unsigned long)held);
    if (buttonLongFired) {
      buttonLongFired = false;  // consumed by the confirm screen
      return;
    }
    if (held >= 30 && state != ST_RESET_CONFIRM) {
      bootLauncher();
    }
  }
}

// called every frame from loop(): fires the confirm screen mid-hold and
// overlays a filling progress arc so the hold has visible feedback
void buttonHoldOverlay() {
  if (buttonHeldSince == 0) return;
  const uint32_t held = millis() - buttonHeldSince;
  if (!buttonLongFired && held > 5000 && state != ST_RESET_CONFIRM) {
    buttonLongFired = true;
    armDelayUntil = millis() + 400;
    state = ST_RESET_CONFIRM;
    Serial.println("btn: hold 5s -> reset confirm");
    return;
  }
  if (held > 500 && state != ST_RESET_CONFIRM) {
    // arc grows clockwise from 12 o'clock as the hold progresses
    const float frac = held > 5000 ? 1.0f : (held - 500) / 4500.0f;
    gfx->fillArc(LCD_WIDTH / 2, LCD_HEIGHT / 2, 226, 216,
                 -90, -90 + (int)(360 * frac), beColor(255, 205, 60));
  }
}

// confirm screen buttons
bool tapInYes() { return tapX >= 84 && tapX <= 216 && tapY >= 288 && tapY <= 352; }
bool tapInNo() { return tapX >= 250 && tapX <= 382 && tapY >= 288 && tapY <= 352; }

void drawResetConfirm() {
  drawSky();
  drawTower();
  gfx->fillRoundRect(53, 110, 360, 260, 18, beColor(250, 246, 236));
  gfx->drawRoundRect(53, 110, 360, 260, 18, beColor(24, 22, 20));
  centerText("RESET DEVICE?", 140, 3, beColor(24, 22, 20));
  centerText("CLEARS THE HIGH SCORE", 186, 2, beColor(110, 104, 96));
  centerText("AND RESET PROMO CODES", 210, 2, beColor(110, 104, 96));

  gfx->fillRoundRect(84, 292, 132, 56, 14, beColor(200, 50, 40));
  gfx->setTextSize(3);
  gfx->setTextColor(beColor(250, 246, 236));
  gfx->setCursor(124, 310);
  gfx->print("YES");

  gfx->fillRoundRect(250, 292, 132, 56, 14, beColor(24, 22, 20));
  gfx->drawRoundRect(250, 292, 132, 56, 14, beColor(255, 205, 60));
  gfx->setTextColor(beColor(255, 205, 60));
  gfx->setCursor(298, 310);
  gfx->print("NO");
}

void doFactoryReset() {
  best = 0;
  prefs.putUInt("best", 0);
  promoUnlocked = false;
  resetGame();
}

// ---- fireworks (a new star every 20 floors) -----------------------------

constexpr int kMaxSparks = 96;
Spark sparks[kMaxSparks];
int fwPending = 0;
uint32_t fwNextMs = 0;

void fireworksStart() {
  fwPending = 3;
  fwNextMs = millis();
}

void spawnBurst() {
  const uint8_t palette[5][3] = {
      {255, 205, 60}, {255, 255, 255}, {255, 140, 40}, {235, 70, 50}, {250, 246, 236}};
  const uint8_t *c1 = palette[esp_random() % 5];
  const uint8_t *c2 = palette[esp_random() % 5];
  const float cx = 110 + (float)(esp_random() % 246);
  const float cy = 80 + (float)(esp_random() % 150);
  int spawned = 0;
  for (int i = 0; i < kMaxSparks && spawned < 30; i++) {
    if (sparks[i].life > 0) continue;
    const float a = (esp_random() % 628) / 100.0f;
    const float s = 1.6f + (esp_random() % 30) / 10.0f;
    const uint8_t *c = (spawned & 1) ? c1 : c2;
    sparks[i] = {cx, cy, cosf(a) * s, sinf(a) * s,
                 (int16_t)(34 + esp_random() % 22), 0, c[0], c[1], c[2]};
    sparks[i].maxLife = sparks[i].life;
    spawned++;
  }
}

void updateFireworks() {
  if (fwPending > 0 && millis() >= fwNextMs) {
    fwPending--;
    fwNextMs = millis() + 300;
    spawnBurst();
  }
  for (int i = 0; i < kMaxSparks; i++) {
    if (sparks[i].life > 0) {
      sparks[i].vy += 0.10f;
      sparks[i].x += sparks[i].vx;
      sparks[i].y += sparks[i].vy;
      sparks[i].life--;
    }
  }
}

void drawFireworks() {
  for (int i = 0; i < kMaxSparks; i++) {
    if (sparks[i].life > 0) {
      const float k = (float)sparks[i].life / sparks[i].maxLife;
      const int x = (int)sparks[i].x, y = (int)sparks[i].y;
      if (x >= 0 && x < LCD_WIDTH - 2 && y >= 0 && y < LCD_HEIGHT - 2) {
        gfx->fillRect(x, y, 2, 2,
                      beColor(sparks[i].r * k, sparks[i].g * k, sparks[i].b * k));
      }
    }
  }
}

// stable per-floor randomness for lit windows
uint32_t floorHash(int floorIdx, int wnd) {
  uint32_t h = floorIdx * 2654435761u + wnd * 40503u + 0x9E37u;
  h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
  return h;
}

float blockSpeed() {
  float s = 3.0f + numFloors * 0.11f;
  return s > 8.5f ? 8.5f : s;
}

void resetGame() {
  numFloors = 0;
  blocks[0].x = (LCD_WIDTH - kBaseW) / 2.0f;
  blocks[0].w = kBaseW;
  blocks[0].tier = 0;
  moveW = kBaseW;
  moveX = 20;
  moveDir = 1;
  cam = 0;
  camTarget = 0;
  for (int i = 0; i < 4; i++) slices[i].age = -1;
  flashFrames = 0;
  perfectStreak = 0;
  bonusFloors = 0;
  perfectShowUntil = 0;
  promoUnlocked = false;
}

void spawnSlice(float x, float w, float worldY, uint8_t tier) {
  for (int i = 0; i < 4; i++) {
    if (slices[i].age < 0) {
      slices[i] = {x, w, worldY, 0, 0, tier};
      return;
    }
  }
}

// ---- sky ----------------------------------------------------------------

// altitude keyframes: {topR,topG,topB, botR,botG,botB}
// cream day -> golden sunset -> dusk navy -> deep space
const int kSkyStops = 4;
const float kSkyAlt[kSkyStops] = {0, 500, 1050, 1650};
const uint8_t kSkyCols[kSkyStops][6] = {
    {214, 226, 236, 250, 246, 236},  // day: pale blue to cream
    {120, 110, 160, 250, 196, 120},  // sunset: violet to gold
    {24, 28, 64, 90, 80, 120},       // dusk: navy
    {6, 8, 20, 14, 16, 34},          // space
};

void skyColorsAt(float alt, uint8_t top[3], uint8_t bot[3]) {
  int s = 0;
  while (s < kSkyStops - 2 && alt > kSkyAlt[s + 1]) s++;
  float t = (alt - kSkyAlt[s]) / (kSkyAlt[s + 1] - kSkyAlt[s]);
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  for (int i = 0; i < 3; i++) {
    top[i] = kSkyCols[s][i] + t * ((int)kSkyCols[s + 1][i] - (int)kSkyCols[s][i]);
    bot[i] = kSkyCols[s][3 + i] + t * ((int)kSkyCols[s + 1][3 + i] - (int)kSkyCols[s][3 + i]);
  }
}

void drawSky() {
  uint8_t top[3], bot[3];
  skyColorsAt(cam, top, bot);
  const int band = 6;
  for (int y = 0; y < LCD_HEIGHT; y += band) {
    const float t = (float)y / LCD_HEIGHT;
    const uint8_t r = top[0] + t * (bot[0] - top[0]);
    const uint8_t g = top[1] + t * (bot[1] - top[1]);
    const uint8_t b = top[2] + t * (bot[2] - top[2]);
    gfx->fillRect(0, y, LCD_WIDTH, band, beColor(r, g, b));
  }

  // stars fade in above the dusk line, fixed in world space
  if (cam > 800) {
    const uint8_t bright = cam > 1300 ? 220 : (uint8_t)(220 * (cam - 800) / 500);
    for (int i = 0; i < 90; i++) {
      const uint32_t h = floorHash(i, 777);
      const int sx = h % LCD_WIDTH;
      const float wy = 900 + (h >> 9) % 2200;
      const int sy = LCD_HEIGHT - (int)(wy - cam);
      if (sy > -4 && sy < LCD_HEIGHT) {
        const uint8_t v = bright - (h % 90);
        gfx->drawPixel(sx, sy, beColor(v, v, v));
        if ((h & 7) == 0) gfx->drawPixel(sx + 1, sy, beColor(v / 2, v / 2, v / 2));
      }
    }
    // a ringed planet and the Hotel Universe yellow "sun"
    const int py = LCD_HEIGHT - (int)(1500 - cam);
    if (py > -80 && py < LCD_HEIGHT + 40) {
      gfx->fillCircle(370, py, 26, beColor(196, 160, 130));
      gfx->drawLine(330, py + 8, 410, py - 8, beColor(230, 210, 180));
      gfx->drawLine(330, py + 10, 410, py - 6, beColor(160, 130, 105));
    }
    const int sy2 = LCD_HEIGHT - (int)(1950 - cam);
    if (sy2 > -60 && sy2 < LCD_HEIGHT + 60) {
      gfx->fillCircle(96, sy2, 34, beColor(255, 205, 60));
      gfx->drawCircle(96, sy2, 40, beColor(120, 100, 40));
    }
  }

  // street when near the ground
  if (cam < kGroundH + 60) {
    const int gy = LCD_HEIGHT - (int)(kGroundH - cam);
    gfx->fillRect(0, gy, LCD_WIDTH, LCD_HEIGHT - gy, beColor(40, 40, 44));
    gfx->fillRect(0, gy, LCD_WIDTH, 3, beColor(255, 205, 60));  // curb line
  }
}

// ---- tower --------------------------------------------------------------

int worldToScreenY(float worldY) {
  // worldY = height above street; returns the screen y of that height
  return LCD_HEIGHT - kGroundH - (int)(worldY - cam);
}

// facade palette per star tier - each tier is a different hotel entirely:
// concrete -> cream boutique -> terracotta grand -> white residence ->
// marble palace -> blue glass tower -> royal gold
const uint8_t kTierFacade[7][3] = {
    {168, 164, 158}, {238, 226, 200}, {216, 178, 132}, {242, 240, 234},
    {246, 242, 234}, {60, 110, 170}, {234, 192, 72},
};

// balconies by cadence: every 5th floor, every 3rd at 3 stars,
// every 2nd from 4 stars up - full-width, slightly protruding
void drawBalcony(int xi, int wi, int sy, int floorIdx, uint8_t tier) {
  const int cadence = tier >= 4 ? 2 : (tier == 3 ? 3 : 5);
  if (floorIdx <= 0 || floorIdx % cadence != 0) return;
  const uint16_t ink = beColor(30, 28, 26);
  gfx->fillRect(xi - 4, sy + kBlockH - 4, wi + 8, 3, ink);   // slab
  gfx->drawRect(xi - 4, sy + kBlockH - 11, wi + 8, 8, ink);  // railing
  for (int px = xi - 1; px < xi + wi; px += 10) {            // posts
    gfx->drawPixel(px, sy + kBlockH - 7, ink);
    gfx->drawPixel(px, sy + kBlockH - 6, ink);
  }
  if (tier >= 3) {  // greenery on the fancier balconies
    for (int px = xi + 2; px < xi + wi - 4; px += 26) {
      gfx->fillRect(px, sy + kBlockH - 10, 4, 3, beColor(40, 150, 60));
    }
  }
}

// ground floor gets an entrance; balcony floors get 2 balcony doors,
// each 2 windows in from the ends
void drawFloorDoors(int xi, int wi, int sy, int floorIdx, uint8_t tier) {
  const uint16_t ink = beColor(30, 28, 26);
  const uint16_t wood = beColor(88, 60, 34);
  const uint16_t brass = beColor(255, 205, 60);

  if (floorIdx == 0) {
    // grand entrance: centered double door
    const int dx = xi + wi / 2 - 10;
    gfx->fillRect(dx, sy + kBlockH - 26, 20, 25, wood);
    gfx->drawRect(dx, sy + kBlockH - 26, 20, 25, ink);
    gfx->drawLine(dx + 10, sy + kBlockH - 26, dx + 10, sy + kBlockH - 2, ink);
    gfx->fillRect(dx + 6, sy + kBlockH - 15, 2, 2, brass);
    gfx->fillRect(dx + 12, sy + kBlockH - 15, 2, 2, brass);
    return;
  }

  const int cadence = tier >= 4 ? 2 : (tier == 3 ? 3 : 5);
  if (floorIdx % cadence != 0) return;

  if (tier == 5) {
    // glass tower: framed sliding doors in the curtain wall grid
    int n = 0;
    for (int gx = xi + 2; gx + 10 < xi + wi - 2; gx += 12) n++;
    if (n < 2) return;
    const int sL = n >= 6 ? 2 : (n >= 3 ? 1 : 0);
    const int sR = n - 1 - sL;
    for (int k = 0; k < 2; k++) {
      const int slot = k == 0 ? sL : sR;
      if (k == 1 && sR <= sL) break;
      const int gx = xi + 2 + slot * 12;
      gfx->fillRect(gx + 1, sy + 5, 10, kBlockH - 8, beColor(26, 50, 86));
      gfx->drawRect(gx + 1, sy + 5, 10, kBlockH - 8, beColor(150, 200, 255));
    }
    return;
  }

  int n = 0;
  for (int wx = xi + 8; wx + 12 < xi + wi - 6; wx += 20) n++;
  if (n < 2) return;
  const int sL = n >= 6 ? 2 : (n >= 3 ? 1 : 0);
  const int sR = n - 1 - sL;
  for (int k = 0; k < 2; k++) {
    const int slot = k == 0 ? sL : sR;
    if (k == 1 && sR <= sL) break;
    const int wx = xi + 8 + slot * 20;
    gfx->fillRect(wx - 1, sy + 9, 14, kBlockH - 11, wood);
    gfx->drawRect(wx - 1, sy + 9, 14, kBlockH - 11, ink);
    gfx->fillRect(wx + 9, sy + 22, 2, 2, brass);
  }
}

void drawBlockRect(float x, float w, float worldY, int floorIdx, bool isMoving,
                   uint8_t tier) {
  const int sy = worldToScreenY(worldY + kBlockH);
  if (sy > LCD_HEIGHT || sy + kBlockH < 0) return;

  // tilt parallax: rows shear away from the screen center, and each box
  // gets an extruded top/side face - a gentle look-around-the-tower effect
  const float rowOff = -tiltX * ((sy + kBlockH / 2) - 233) * 0.18f;
  const int xi = (int)(x + rowOff);
  const int wi = (int)w;
  const int ex = (int)(-tiltX * 48);                       // side extrusion
  const int vy = (int)clampf(8 + tiltY * 20, 2, 26);       // top-face depth

  uint8_t fr = kTierFacade[tier][0], fg = kTierFacade[tier][1], fb = kTierFacade[tier][2];
  if (!(floorIdx & 1)) { fr -= 12; fg -= 12; fb -= 12; }
  if (isMoving && flashFrames == 0) { fr = 246; fg = 240; fb = 226; }
  const uint16_t ink = beColor(30, 28, 26);

  // top face (lighter)
  const uint16_t topc = beColor(min(255, fr + 22), min(255, fg + 22), min(255, fb + 22));
  gfx->fillTriangle(xi, sy, xi + ex, sy - vy, xi + wi + ex, sy - vy, topc);
  gfx->fillTriangle(xi, sy, xi + wi + ex, sy - vy, xi + wi, sy, topc);
  // side face (darker), on the side the extrusion leans toward
  const uint16_t sidec = beColor(fr * 2 / 3, fg * 2 / 3, fb * 2 / 3);
  if (ex > 0) {
    gfx->fillTriangle(xi + wi, sy, xi + wi + ex, sy - vy, xi + wi + ex, sy + kBlockH - vy, sidec);
    gfx->fillTriangle(xi + wi, sy, xi + wi + ex, sy + kBlockH - vy, xi + wi, sy + kBlockH, sidec);
  } else if (ex < 0) {
    gfx->fillTriangle(xi, sy, xi + ex, sy - vy, xi + ex, sy + kBlockH - vy, sidec);
    gfx->fillTriangle(xi, sy, xi + ex, sy + kBlockH - vy, xi, sy + kBlockH, sidec);
  }

  // front face
  gfx->fillRect(xi, sy, wi, kBlockH, beColor(fr, fg, fb));
  gfx->drawRect(xi, sy, wi, kBlockH, ink);

  // per-tier decoration - each star level is a visibly different hotel
  const uint16_t goldc = beColor(186, 152, 40);
  const uint16_t brightGold = beColor(255, 205, 60);
  const uint16_t litc = beColor(255, 205, 60);

  if (tier == 5) {
    drawBalcony(xi, wi, sy, floorIdx, tier);
    // modern tower: full-height blue glass curtain wall with mullion grid
    for (int gx = xi + 2; gx + 10 < xi + wi - 2; gx += 12) {
      const int cell = (gx - xi) / 12;
      const bool lit = (floorHash(floorIdx, cell) & 3) != 0;
      gfx->fillRect(gx + 1, sy + 3, 10, 13,
                    lit ? beColor(150, 200, 255) : beColor(40, 75, 125));
      gfx->fillRect(gx + 1, sy + 19, 10, 13,
                    lit ? beColor(120, 175, 240) : beColor(34, 64, 110));
    }
    gfx->fillRect(xi, sy + 16, wi, 2, beColor(28, 52, 90));  // floor slab line
    drawFloorDoors(xi, wi, sy, floorIdx, tier);
    return;
  }

  for (int wx = xi + 8; wx + 12 < xi + wi - 6; wx += 20) {
    const int wnd = (wx - xi) / 20;
    const bool lit = (floorHash(floorIdx, wnd) & 7) < (uint32_t)(1 + tier);
    const uint16_t offc = (tier == 0) ? beColor(52, 50, 48) : beColor(70, 66, 62);
    gfx->fillRect(wx, sy + 7, 12, 9, lit ? litc : offc);
    gfx->fillRect(wx, sy + 21, 12, 9, lit ? litc : offc);

    switch (tier) {
      case 1:  // cream boutique: red awnings over every window
        gfx->fillRect(wx - 2, sy + 4, 16, 3, beColor(200, 50, 40));
        gfx->fillRect(wx - 1, sy + 7, 14, 2, beColor(150, 34, 28));
        gfx->fillRect(wx - 2, sy + 18, 16, 3, beColor(200, 50, 40));
        gfx->fillRect(wx - 1, sy + 21, 14, 2, beColor(150, 34, 28));
        break;
      case 2:  // terracotta grand: gold frames
        gfx->drawRect(wx - 1, sy + 6, 14, 11, goldc);
        gfx->drawRect(wx - 1, sy + 20, 14, 11, goldc);
        break;
      case 3:  // white residence: dark green shutters beside the windows
        gfx->fillRect(wx - 3, sy + 7, 2, 9, beColor(30, 90, 45));
        gfx->fillRect(wx + 13, sy + 7, 2, 9, beColor(30, 90, 45));
        gfx->fillRect(wx - 3, sy + 21, 2, 9, beColor(30, 90, 45));
        gfx->fillRect(wx + 13, sy + 21, 2, 9, beColor(30, 90, 45));
        break;
      case 4:  // marble palace: pilaster columns between windows
        gfx->fillRect(wx + 13, sy + 3, 4, kBlockH - 6, beColor(214, 200, 164));
        break;
      case 6:  // royal gold: white stars between window rows
        gfx->drawRect(wx - 1, sy + 6, 14, 11, beColor(255, 255, 255));
        gfx->drawRect(wx - 1, sy + 20, 14, 11, beColor(255, 255, 255));
        gfx->setTextSize(1);
        gfx->setTextColor(RGB565_WHITE);
        gfx->setCursor(wx + 3, sy + 16);
        gfx->print("*");
        break;
    }
  }

  drawBalcony(xi, wi, sy, floorIdx, tier);

  if (tier == 2) {  // gold base band
    gfx->fillRect(xi, sy + kBlockH - 3, wi, 3, goldc);
  }
  if (tier == 4) {  // double gold cornice
    gfx->fillRect(xi, sy, wi, 3, goldc);
    gfx->fillRect(xi, sy + kBlockH - 3, wi, 3, goldc);
  }
  if (tier == 6) {  // mirror shine + sparkle
    gfx->fillRect(xi, sy, wi, 2, brightGold);
    gfx->fillRect(xi, sy + kBlockH - 2, wi, 2, brightGold);
    const uint32_t h = floorHash(floorIdx, 99);
    gfx->drawPixel(xi + (h % (wi > 4 ? wi - 4 : 1)) + 2, sy + 4 + ((h >> 8) % 28),
                   RGB565_WHITE);
  }

  drawFloorDoors(xi, wi, sy, floorIdx, tier);
}

void drawTower() {
  for (int i = 0; i <= numFloors && i <= kMaxFloors; i++) {
    drawBlockRect(blocks[i].x, blocks[i].w, (float)i * kBlockH, i, false,
                  blocks[i].tier);
  }
  // falling slices keep the facade of the floor they broke off
  for (int i = 0; i < 4; i++) {
    if (slices[i].age >= 0) {
      const int sy = worldToScreenY(slices[i].worldY + kBlockH);
      const uint8_t t = slices[i].tier;
      gfx->fillRect((int)slices[i].x, sy, (int)slices[i].w, kBlockH,
                    beColor(kTierFacade[t][0] - 20, kTierFacade[t][1] - 20,
                            kTierFacade[t][2] - 20));
      gfx->drawRect((int)slices[i].x, sy, (int)slices[i].w, kBlockH,
                    beColor(30, 28, 26));
    }
  }
}

// hexagonal owl mark (the Hotel Universe logo, simplified)
void drawOwl(int cx, int cy, int r) {
  const uint16_t ink = beColor(24, 22, 20);
  int px[6], py[6];
  for (int i = 0; i < 6; i++) {
    const float a = PI / 6 + i * PI / 3;
    px[i] = cx + (int)(cosf(a) * r);
    py[i] = cy + (int)(sinf(a) * r);
  }
  for (int i = 0; i < 6; i++) {
    const int j = (i + 1) % 6;
    gfx->fillTriangle(cx, cy, px[i], py[i], px[j], py[j], ink);
  }
  const int er = r / 3;
  gfx->fillCircle(cx - r / 3, cy, er, beColor(250, 246, 236));
  gfx->fillCircle(cx + r / 3, cy, er, beColor(250, 246, 236));
  // left eye is closed (a ring), right eye open (a dot) - per the ubio mark
  gfx->fillCircle(cx - r / 3, cy, er / 2, ink);
  gfx->fillCircle(cx - r / 3, cy, er / 4, beColor(250, 246, 236));
  gfx->fillCircle(cx + r / 3, cy, er / 2, ink);
}

// ---- HUD / text helpers -------------------------------------------------

void centerText(const char *s, int y, uint8_t size, uint16_t color) {
  gfx->setTextSize(size);
  int16_t bx, by;
  uint16_t bw, bh;
  gfx->getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  gfx->setTextColor(color);
  gfx->setCursor((LCD_WIDTH - (int16_t)bw) / 2, y);
  gfx->print(s);
}

void drawStars(int cx, int y, int count, int size, uint16_t color) {
  // a glyph at textSize s is 6*s px wide; step 8*s leaves a 2*s gap
  const int step = size * 8;
  int x = cx - (count - 1) * step / 2;
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  for (int i = 0; i < count; i++) {
    gfx->setCursor(x - 3 * size, y);
    gfx->print("*");
    x += step;
  }
}

// 1 star per 20 floors, up to the coveted 6-star rating
int starRating(int floors) {
  const int s = floors / 20;
  return s > 6 ? 6 : s;
}

void drawHud() {
  char t[16];
  snprintf(t, sizeof(t), "%d", numFloors);
  const bool dark = cam > 700;
  centerText(t, 34, 5, dark ? beColor(250, 246, 236) : beColor(24, 22, 20));
  const int stars = starRating(numFloors);
  if (stars > 0) {
    drawStars(LCD_WIDTH / 2, 82, stars, 2, beColor(255, 205, 60));
  }
}

// ---- screens ------------------------------------------------------------

void drawMenu() {
  drawSky();
  drawTower();
  gfx->fillRoundRect(53, 118, 360, 230, 18, beColor(250, 246, 236));
  gfx->drawRoundRect(53, 118, 360, 230, 18, beColor(24, 22, 20));
  drawOwl(LCD_WIDTH / 2, 160, 24);
  centerText("HOTEL TOWER", 198, 4, beColor(24, 22, 20));
  centerText("BUILD THE HOTEL UNIVERSE", 234, 2, beColor(110, 104, 96));
  // brand-yellow highlight, like the website's marker stroke
  gfx->fillRect(93, 256, 280, 26, beColor(255, 205, 60));
  centerText("PLAY FOR FREE CREDITS", 262, 2, beColor(24, 22, 20));
  char t[32];
  snprintf(t, sizeof(t), "BEST: %lu FLOORS", (unsigned long)best);
  centerText(t, 294, 2, beColor(110, 104, 96));
  if ((millis() / 500) & 1) {
    centerText("TAP TO START", 322, 2, beColor(180, 130, 20));
  }
  centerText("hotel-universe.travel", 358, 2, cam > 700 ? beColor(160, 160, 170) : beColor(120, 114, 106));
}

void drawPromoSplash() {
  drawSky();
  drawTower();
  char code[16];
  makePromoCode(numFloors, code, sizeof(code));
  gfx->fillRoundRect(43, 90, 380, 266, 18, beColor(255, 205, 60));
  gfx->drawRoundRect(43, 90, 380, 266, 18, beColor(24, 22, 20));
  centerText("FLOOR 50!", 112, 4, beColor(24, 22, 20));
  drawStars(LCD_WIDTH / 2, 150, starRating(numFloors), 3, beColor(24, 22, 20));
  centerText("YOUR PROMO CODE:", 184, 2, beColor(90, 74, 20));
  centerText(code, 208, 3, beColor(24, 22, 20));
  centerText("KEEP BUILDING FOR MORE", 244, 2, beColor(90, 74, 20));
  centerText("STARS AND DISCOUNTS", 264, 2, beColor(90, 74, 20));
  if ((millis() / 500) & 1) {
    centerText("TAP TO KEEP BUILDING", 332, 2, beColor(90, 74, 20));
  }
}

void drawGameOver() {
  drawSky();
  drawTower();
  gfx->fillRoundRect(53, 96, 360, 274, 18, beColor(250, 246, 236));
  gfx->drawRoundRect(53, 96, 360, 274, 18, beColor(24, 22, 20));
  centerText("CLOSED FOR", 128, 3, beColor(24, 22, 20));
  centerText("RENOVATION", 158, 3, beColor(24, 22, 20));
  char t[36];
  snprintf(t, sizeof(t), "%d FLOORS", numFloors);
  centerText(t, 200, 4, beColor(24, 22, 20));
  const int stars = starRating(numFloors);
  if (stars > 0) {
    drawStars(LCD_WIDTH / 2, 244, stars, 2, beColor(180, 130, 20));
  }
  snprintf(t, sizeof(t), "BEST: %lu", (unsigned long)best);
  centerText(t, 274, 2, beColor(110, 104, 96));
  if (numFloors >= PROMO_FLOORS) {
    char code[16];
    makePromoCode(numFloors, code, sizeof(code));
    snprintf(t, sizeof(t), "PROMO: %s", code);
    centerText(t, 302, 2, beColor(180, 130, 20));
  }
  if (millis() - gameOverAt > 900 && ((millis() / 500) & 1)) {
    centerText("TAP TO CONTINUE", 336, 2, beColor(180, 130, 20));
  }
}

// ---- game logic ---------------------------------------------------------

void dropBlock() {
  if (numFloors >= kMaxFloors) return;  // array guard; nobody gets here honestly
  Block &prev = blocks[numFloors];
  const float dx = moveX - prev.x;

  if (fabsf(dx) <= kPerfectPx) {
    // perfect: snap, tiny width bonus back
    moveX = prev.x;
    moveW = prev.w + 4 > kBaseW ? kBaseW : prev.w + 4;
    flashFrames = 6;
    if (++perfectStreak >= 5) {
      // 5 in a row: PERFECT! and 5 bonus floors build themselves
      perfectStreak = 0;
      bonusFloors = 5;
      bonusNextMs = millis() + 200;
      perfectShowUntil = millis() + 1400;
    }
  } else {
    perfectStreak = 0;
    // compute overlap
    const float left = max(moveX, prev.x);
    const float right = min(moveX + moveW, prev.x + prev.w);
    const float overlap = right - left;
    if (overlap <= 4) {
      // total miss: whole block falls, game over
      spawnSlice(moveX, moveW, (float)(numFloors + 1) * kBlockH,
                 starRating(numFloors + 1));
      saveBestIfNeeded();
      gameOverAt = millis();
      armDelayUntil = millis() + 900;
      state = ST_GAMEOVER;
      return;
    }
    // slice the overhang
    if (moveX < left) {
      spawnSlice(moveX, left - moveX, (float)(numFloors + 1) * kBlockH,
                 starRating(numFloors + 1));
    } else if (moveX + moveW > right) {
      spawnSlice(right, moveX + moveW - right, (float)(numFloors + 1) * kBlockH,
                 starRating(numFloors + 1));
    }
    moveX = left;
    moveW = overlap;
  }

  numFloors++;
  blocks[numFloors].x = moveX;
  blocks[numFloors].w = moveW;
  blocks[numFloors].tier = starRating(numFloors);
  saveBestIfNeeded();

  if (starRating(numFloors) > starRating(numFloors - 1)) {
    fireworksStart();  // a new star!
  }

  if (numFloors == PROMO_FLOORS && !promoUnlocked) {
    promoUnlocked = true;
    // the full splash only on the first-ever unlock; repeat players just
    // keep playing (codes show in the HUD and on the game-over card)
    armDelayUntil = millis() + 700;
    state = ST_PROMO_SPLASH;
  }

  // next moving floor
  camTarget = max(0.0f, (float)(numFloors + 1) * kBlockH - kCameraLine);
  moveDir = -moveDir;
  moveX = moveDir > 0 ? 12 : LCD_WIDTH - 12 - moveW;
}

// auto-built streak-bonus floor: perfectly aligned on top of the stack
void placeBonusFloor() {
  if (numFloors >= kMaxFloors) return;
  const Block &prev = blocks[numFloors];
  numFloors++;
  blocks[numFloors].x = prev.x;
  blocks[numFloors].w = prev.w;
  blocks[numFloors].tier = starRating(numFloors);
  flashFrames = 4;
  saveBestIfNeeded();

  if (starRating(numFloors) > starRating(numFloors - 1)) {
    fireworksStart();
  }
  if (numFloors == PROMO_FLOORS && !promoUnlocked) {
    promoUnlocked = true;
    // the full splash only on the first-ever unlock; repeat players just
    // keep playing (codes show in the HUD and on the game-over card)
    armDelayUntil = millis() + 700;
    state = ST_PROMO_SPLASH;
  }
  camTarget = max(0.0f, (float)(numFloors + 1) * kBlockH - kCameraLine);
  moveX = moveDir > 0 ? 12 : LCD_WIDTH - 12 - moveW;
}

void updatePlaying() {
  // slide
  moveX += moveDir * blockSpeed();
  if (moveX < 8) { moveX = 8; moveDir = 1; }
  if (moveX + moveW > LCD_WIDTH - 8) { moveX = LCD_WIDTH - 8 - moveW; moveDir = -1; }

  // camera ease
  cam += (camTarget - cam) * 0.12f;

  // slices fall
  for (int i = 0; i < 4; i++) {
    if (slices[i].age >= 0) {
      slices[i].vy += 1.1f;
      slices[i].worldY -= slices[i].vy;
      if (++slices[i].age > 80 || worldToScreenY(slices[i].worldY) > LCD_HEIGHT + 40) {
        slices[i].age = -1;
      }
    }
  }
  if (flashFrames > 0) flashFrames--;
  updateFireworks();

  // streak bonus: floors build themselves, taps are ignored meanwhile
  if (bonusFloors > 0) {
    if (millis() >= bonusNextMs) {
      bonusFloors--;
      bonusNextMs = millis() + 130;
      placeBonusFloor();
    }
    pollTap();  // drain touch events so a tap doesn't fire after the bonus
    return;
  }

  if (pollTap()) {
    dropBlock();
  }
}

void drawPlaying() {
  drawSky();
  drawTower();
  // the sliding floor, previewed in the tier it will be built at
  // (hidden while the streak bonus is building floors on its own)
  if (bonusFloors == 0) {
    drawBlockRect(moveX, moveW, (float)(numFloors + 1) * kBlockH, numFloors + 1,
                  true, starRating(numFloors + 1));
  }
  if (flashFrames > 0) {
    // perfect-drop flash: yellow edge on the just-placed floor
    const int sy = worldToScreenY((float)(numFloors)*kBlockH + kBlockH);
    gfx->drawRect((int)blocks[numFloors].x - 2, sy - 2,
                  (int)blocks[numFloors].w + 4, kBlockH + 4, beColor(255, 205, 60));
  }
  drawFireworks();
  drawHud();
  if (millis() < perfectShowUntil) {
    if ((millis() / 180) & 1) {
      centerText("PERFECT!", 190, 5, beColor(255, 205, 60));
    } else {
      centerText("PERFECT!", 190, 5, RGB565_WHITE);
    }
    centerText("+5 FLOORS", 240, 3, beColor(255, 205, 60));
  }
}

// ---- arduino ------------------------------------------------------------

void setup() {
  launcherExitCheck();
  powerButtonInit();
  Serial.begin(115200);

  if (!gfx->begin(80000000)) {
    Serial.println("gfx->begin() failed!");
  }
  bufs[0] = gfx->getFramebuffer();
  bufs[1] = (uint16_t *)heap_caps_malloc(
      (size_t)LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
  flushQueue = xQueueCreate(1, sizeof(uint16_t *));
  renderReady = xSemaphoreCreateCounting(2, 2);
  xTaskCreatePinnedToCore(flushTask, "flush", 4096, nullptr, 2, nullptr, 0);
  panel->setBrightness(220);

  touch.setPins(TP_RESET, TP_INT);
  touchOk = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (touchOk) {
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchInterrupt, FALLING);
  }

  imuOk = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (imuOk) {
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_250Hz,
                            SensorQMI8658::LPF_MODE_0);
    qmi.enableAccelerometer();
  }
  Wire.setClock(400000);
  Wire.setTimeOut(20);  // never let a wedged bus stall the render loop

  Serial.println("Hotel Tower version: 2026-09-03-V2");
  prefs.begin("tower", false);
  best = prefs.getUInt("best", 0);

  resetGame();
  Serial.println("Hotel Tower ready");
}

void loop() {
  powerButtonTick();
  towerButtonTick();
  updateTilt();

  xSemaphoreTake(renderReady, portMAX_DELAY);

  switch (state) {
    case ST_MENU:
      drawMenu();
      if (pollTap()) {
        resetGame();
        armDelayUntil = millis() + 250;
        state = ST_PLAYING;
      }
      break;
    case ST_PLAYING:
      updatePlaying();
      drawPlaying();
      break;
    case ST_PROMO_SPLASH:
      drawPromoSplash();
      if (pollTap()) {
        armDelayUntil = millis() + 250;
        state = ST_PLAYING;
      }
      break;
    case ST_GAMEOVER:
      drawGameOver();
      if (millis() - gameOverAt > 900 && pollTap()) {
        resetGame();
        armDelayUntil = millis() + 250;
        state = ST_MENU;
      }
      break;
    case ST_RESET_CONFIRM:
      drawResetConfirm();
      if (pollTap()) {
        if (tapInYes()) {
          doFactoryReset();
          armDelayUntil = millis() + 400;
          state = ST_MENU;
        } else if (tapInNo()) {
          armDelayUntil = millis() + 250;
          state = ST_MENU;
        }
      }
      break;
  }

  buttonHoldOverlay();

  xQueueSend(flushQueue, &bufs[curBuf], portMAX_DELAY);
  curBuf ^= 1;
  gfx->setFB(bufs[curBuf]);
}
