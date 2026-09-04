// DEPRECATED — do not flash. The HU-POD / "Can it play Doom?" /
// "back to badge" menu is retired. ota_0 is erased by flash_badge_os.sh /
// flash_unified.sh. Doom is launched from Badge OS (hold gesture → ota_4).
//
// Legacy layout: Volcano & Fighter Jet icons, Doom fire middle button,
// "back to badge" at the bottom. Previously lived in ota_0.

#include <Arduino.h>
#include "launcher_exit.h"
#include "power_button.h"
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "TouchDrvCSTXXX.hpp"
#include "esp_ota_ops.h"
#include "esp_partition.h"

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

enum UiState { UI_MENU, UI_DOOM_ANIM };

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *panel = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

TouchDrvCST92xx touch;

UiState state = UI_MENU;
uint32_t animStartMs = 0;

// icon positions
constexpr int16_t kVolcX = 128, kVolcY = 134, kIconR = 58;
constexpr int16_t kJetX = 338, kJetY = 134;
// middle + bottom buttons
constexpr int16_t kDoomY = 226, kDoomH = 62;
constexpr int16_t kBackY = 316, kBackH = 54;

uint32_t lastTouchReportMs = 0;
bool armed = true;
volatile bool touchPending = false;

// ---- DOOM fire (chunky 8px cells, classic propagation) ------------------

constexpr int kFW = 58, kFH = 58;  // 58 * 8 = 464
uint8_t fire[kFW * kFH];

uint16_t firePal[13];

void fireInit() {
  // classic DOOM fire ramp: black -> red -> orange -> yellow -> white
  const uint8_t ramp[13][3] = {
      {0, 0, 0}, {31, 7, 7}, {71, 15, 7}, {103, 31, 7}, {143, 39, 7},
      {175, 63, 7}, {199, 83, 7}, {223, 117, 15}, {223, 151, 31},
      {239, 183, 47}, {239, 215, 95}, {247, 235, 167}, {255, 255, 255},
  };
  for (int i = 0; i < 13; i++) {
    firePal[i] = gfx->color565(ramp[i][0], ramp[i][1], ramp[i][2]);
  }
  memset(fire, 0, sizeof(fire));
  for (int x = 0; x < kFW; x++) {
    fire[(kFH - 1) * kFW + x] = 12;  // bottom row = max heat
  }
}

void fireStep() {
  for (int y = 1; y < kFH; y++) {
    for (int x = 0; x < kFW; x++) {
      const int src = y * kFW + x;
      const uint32_t r = esp_random();
      int dx = (int)(r & 3) - 1;  // -1..2 -> lateral drift
      if (dx > 1) dx = 0;
      int dst = src - kFW + dx;
      if (dst < 0) dst = src - kFW;
      int v = fire[src] - (int)((r >> 4) & 1);
      fire[dst] = v < 0 ? 0 : v;
    }
  }
}

void fireDraw() {
  for (int y = 0; y < kFH; y++) {
    for (int x = 0; x < kFW; x++) {
      const uint8_t v = fire[y * kFW + x];
      if (v > 0) {
        gfx->fillRect(1 + x * 8, 1 + y * 8, 8, 8, firePal[v]);
      }
    }
  }
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

int16_t tapX = 0, tapY = 0;

bool pollTap() {
  bool tapped = false;
  if (takeTouchInterrupt()) {
    int16_t x, y;
    if (touch.getPoint(&x, &y, 1) > 0) {
      lastTouchReportMs = millis();
      if (armed) {
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

bool tapInCircle(int16_t cx, int16_t cy, int16_t r) {
  const int32_t dx = tapX - cx, dy = tapY - cy;
  return dx * dx + dy * dy <= (int32_t)r * r;
}

bool tapInBand(int16_t y, int16_t h) {
  return tapY >= y - 8 && tapY <= y + h + 8;
}

// ---- boot helpers -------------------------------------------------------

void bootSubtype(esp_partition_subtype_t sub) {
  const esp_partition_t *p =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, sub, NULL);
  if (p && esp_ota_set_boot_partition(p) == ESP_OK) {
    delay(120);
    esp_restart();
  }
}

// ---- drawing ------------------------------------------------------------

void centerText(const char *s, int y, uint8_t size, uint16_t color) {
  gfx->setTextSize(size);
  int16_t bx, by;
  uint16_t bw, bh;
  gfx->getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  gfx->setTextColor(color);
  gfx->setCursor((LCD_WIDTH - (int16_t)bw) / 2, y);
  gfx->print(s);
}

void drawVolcanoIcon() {
  gfx->fillCircle(kVolcX, kVolcY, kIconR, gfx->color565(16, 18, 40));
  gfx->drawCircle(kVolcX, kVolcY, kIconR, gfx->color565(255, 140, 40));
  // cone
  gfx->fillTriangle(kVolcX - 42, kVolcY + 34, kVolcX + 42, kVolcY + 34,
                    kVolcX + 12, kVolcY - 18, gfx->color565(122, 104, 92));
  gfx->fillTriangle(kVolcX - 42, kVolcY + 34, kVolcX - 12, kVolcY - 18,
                    kVolcX + 12, kVolcY - 18, gfx->color565(122, 104, 92));
  // crater lava + splashes
  gfx->fillRect(kVolcX - 12, kVolcY - 22, 24, 6, gfx->color565(255, 90, 0));
  gfx->fillCircle(kVolcX - 8, kVolcY - 30, 4, gfx->color565(255, 160, 30));
  gfx->fillCircle(kVolcX + 6, kVolcY - 36, 3, gfx->color565(255, 90, 0));
  gfx->fillCircle(kVolcX + 16, kVolcY - 28, 3, gfx->color565(255, 200, 60));
  // lava streak down the slope
  gfx->fillTriangle(kVolcX - 2, kVolcY - 18, kVolcX + 6, kVolcY - 18,
                    kVolcX + 2, kVolcY + 12, gfx->color565(255, 90, 0));
}

// top-view F/A-18 silhouette, matching the 3D model in the FighterJet app:
// pointed radome, LEX strakes to the wing roots, trapezoid wings with tip
// missiles, twin canted tails, swept stabilators, twin nozzles
void drawJetIcon() {
  gfx->fillCircle(kJetX, kJetY, kIconR, gfx->color565(24, 30, 44));
  gfx->drawCircle(kJetX, kJetY, kIconR, gfx->color565(170, 170, 185));
  const int cx = kJetX, cy = kJetY;
  const uint16_t body = gfx->color565(188, 194, 202);
  const uint16_t dark = gfx->color565(140, 146, 156);
  const uint16_t ink = gfx->color565(90, 96, 106);

  // LEX strakes: thin blades from the nose back to the wing roots
  gfx->fillTriangle(cx - 2, cy - 34, cx - 11, cy - 2, cx - 4, cy - 2, dark);
  gfx->fillTriangle(cx + 2, cy - 34, cx + 11, cy - 2, cx + 4, cy - 2, dark);

  // main wings: trapezoids, straight-ish trailing edge (not deltas)
  gfx->fillTriangle(cx - 8, cy - 2, cx - 36, cy + 12, cx - 8, cy + 14, dark);
  gfx->fillTriangle(cx - 36, cy + 12, cx - 36, cy + 17, cx - 8, cy + 14, dark);
  gfx->fillTriangle(cx + 8, cy - 2, cx + 36, cy + 12, cx + 8, cy + 14, dark);
  gfx->fillTriangle(cx + 36, cy + 12, cx + 36, cy + 17, cx + 8, cy + 14, dark);
  // wingtip missiles
  gfx->fillRect(cx - 39, cy + 2, 3, 17, ink);
  gfx->fillRect(cx + 36, cy + 2, 3, 17, ink);

  // fuselage: radome -> body -> tail
  gfx->fillTriangle(cx, cy - 46, cx - 5, cy - 30, cx + 5, cy - 30, body);
  gfx->fillRect(cx - 5, cy - 30, 11, 44, body);
  gfx->fillRect(cx - 7, cy - 4, 15, 22, body);  // widens over the intakes

  // stabilators: swept rear trapezoids
  gfx->fillTriangle(cx - 7, cy + 20, cx - 22, cy + 30, cx - 7, cy + 32, dark);
  gfx->fillTriangle(cx + 7, cy + 20, cx + 22, cy + 30, cx + 7, cy + 32, dark);

  // twin canted vertical tails (seen from above: two short diagonals)
  gfx->fillTriangle(cx - 5, cy + 14, cx - 14, cy + 26, cx - 10, cy + 28, ink);
  gfx->fillTriangle(cx + 5, cy + 14, cx + 14, cy + 26, cx + 10, cy + 28, ink);

  // canopy
  gfx->fillCircle(cx, cy - 24, 4, gfx->color565(90, 160, 255));
  gfx->fillRect(cx - 3, cy - 24, 7, 7, gfx->color565(90, 160, 255));

  // twin nozzles with afterburner glow
  gfx->fillCircle(cx - 3, cy + 40, 3, gfx->color565(255, 160, 30));
  gfx->fillCircle(cx + 3, cy + 40, 3, gfx->color565(255, 160, 30));
  gfx->fillRect(cx - 6, cy + 36, 13, 4, gfx->color565(70, 66, 66));
}

void drawBattery() {
  // refresh the reading every couple of seconds
  static int pct = -2;
  static bool chg = false;
  static uint32_t lastRead = 0;
  if (pct == -2 || millis() - lastRead > 2000) {
    lastRead = millis();
    pct = powerBatteryPercent();
    chg = powerIsCharging();
  }
  if (pct < 0) return;  // no battery attached

  const int bx = LCD_WIDTH / 2 - 32, by = 52, bw = 36, bh = 16;
  const uint16_t frame = gfx->color565(150, 150, 160);
  const uint16_t fillc = pct <= 20 ? gfx->color565(220, 60, 50)
                       : pct <= 50 ? gfx->color565(255, 205, 60)
                                   : gfx->color565(80, 200, 120);
  gfx->drawRoundRect(bx, by, bw, bh, 3, frame);
  gfx->fillRect(bx + bw, by + 4, 3, 8, frame);  // terminal nub
  const int fw = (bw - 6) * pct / 100;
  if (fw > 0) gfx->fillRect(bx + 3, by + 3, fw, bh - 6, fillc);
  if (chg) {  // little lightning bolt over the fill
    gfx->fillTriangle(bx + 19, by + 2, bx + 12, by + 9, bx + 17, by + 9, RGB565_WHITE);
    gfx->fillTriangle(bx + 17, by + 13, bx + 24, by + 6, bx + 19, by + 6, RGB565_WHITE);
  }
  char t[8];
  snprintf(t, sizeof(t), "%d%%", pct);
  gfx->setTextSize(2);
  gfx->setTextColor(frame);
  gfx->setCursor(bx + bw + 10, by + 1);
  gfx->print(t);
}

void drawMenu() {
  gfx->fillScreen(RGB565_BLACK);
  centerText("HU-POD", 34, 2, gfx->color565(140, 146, 158));
  drawBattery();

  drawVolcanoIcon();
  drawJetIcon();
  gfx->setTextSize(2);
  gfx->setTextColor(gfx->color565(150, 150, 160));
  gfx->setCursor(kVolcX - 48, 200);
  gfx->print("VOLCANO");
  gfx->setCursor(kJetX - 24, 200);
  gfx->print("JET");

  // the big question
  gfx->fillRoundRect(63, kDoomY, 340, kDoomH, 16, gfx->color565(60, 12, 10));
  gfx->drawRoundRect(63, kDoomY, 340, kDoomH, 16, gfx->color565(220, 60, 50));
  centerText("Can it play Doom?", kDoomY + 22, 2, gfx->color565(255, 210, 200));

  gfx->fillRoundRect(83, kBackY, 300, kBackH, 16, gfx->color565(40, 36, 20));
  gfx->drawRoundRect(83, kBackY, 300, kBackH, 16, gfx->color565(255, 205, 60));
  centerText("back to badge", kBackY + 19, 2, gfx->color565(255, 205, 60));

  gfx->flush();
}

void drawDoomAnim() {
  gfx->fillScreen(RGB565_BLACK);
  fireStep();
  fireDraw();

  // "lets see" with an animated ellipsis
  const int dots = (millis() / 350) % 4;
  char msg[16] = "lets see";
  for (int i = 0; i < dots; i++) strcat(msg, ".");
  centerText(msg, 152, 4, RGB565_WHITE);

  gfx->flush();
}

// ---- arduino ------------------------------------------------------------

void setup() {
  launcherExitCheck();
  powerButtonInit();  // one-shot: a reset from here returns to the game
  Serial.begin(115200);

  if (!gfx->begin(80000000)) {
    Serial.println("gfx->begin() failed!");
  }
  panel->setBrightness(220);

  touch.setPins(TP_RESET, TP_INT);
  if (touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchInterrupt, FALLING);
  } else {
    Serial.println("touch init failed!");
  }

  Serial.println("LEGACY LOADER - OLD VERSION - DO NOT USE");
  drawMenu();
  Serial.println("Easter egg screen ready");
}

void loop() {
  powerButtonTick();
  launcherExitTick();

  if (state == UI_MENU) {
    if (pollTap()) {
      if (tapInCircle(kVolcX, kVolcY, kIconR + 12)) {
        Serial.println("boot volcano");
        bootSubtype(ESP_PARTITION_SUBTYPE_APP_OTA_2);
      } else if (tapInCircle(kJetX, kJetY, kIconR + 12)) {
        Serial.println("boot jet");
        bootSubtype(ESP_PARTITION_SUBTYPE_APP_OTA_3);
      } else if (tapInBand(kDoomY, kDoomH)) {
        Serial.println("can it play doom?");
        fireInit();
        animStartMs = millis();
        state = UI_DOOM_ANIM;
      } else if (tapInBand(kBackY, kBackH)) {
        Serial.println("back to badge");
        delay(80);
        esp_restart();  // otadata already cleared -> factory = ConferenceBadge
      }
    }
    // gentle redraw for the blinking bits (cheap enough)
    static uint32_t lastDraw = 0;
    if (millis() - lastDraw > 250) {
      lastDraw = millis();
      drawMenu();
    }
  } else {
    drawDoomAnim();
    if (millis() - animStartMs > 3800) {
      Serial.println("...yes it can");
      bootSubtype(ESP_PARTITION_SUBTYPE_APP_OTA_4);
    }
  }
  delay(5);
}
