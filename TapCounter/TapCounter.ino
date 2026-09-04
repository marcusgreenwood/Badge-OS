// Tap counter test app for Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466 round)
// Display: CO5300 over QSPI  |  Touch: CST9217 over I2C

#include <Arduino.h>
#include "launcher_exit.h"
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "TouchDrvCSTXXX.hpp"

// Pin map from Waveshare's pin_config.h for this board
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

Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

TouchDrvCST92xx touch;

uint32_t counter = 0;
// The CST9217 interleaves empty frames while a finger is held, so a simple
// touched/not-touched edge detector over-counts. Instead, a tap only counts
// after a quiet period (no touch reports at all) — held fingers keep
// reporting every few tens of ms and stay disarmed until lift-off.
uint32_t lastTouchReportMs = 0;
bool armed = true;
constexpr uint32_t kQuietRearmMs = 80;
volatile bool touchPending = false;

void IRAM_ATTR onTouchInterrupt() {
  touchPending = true;
}

bool takeTouchInterrupt() {
  noInterrupts();
  const bool pending = touchPending;
  touchPending = false;
  interrupts();
  return pending;
}

void drawCounter() {
  char text[12];
  snprintf(text, sizeof(text), "%lu", (unsigned long)counter);

  gfx->setTextSize(12);
  int16_t bx, by;
  uint16_t bw, bh;
  gfx->getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);

  // Clear a band tall enough for the digits, then draw centered
  int16_t y = (LCD_HEIGHT - (int16_t)bh) / 2;
  gfx->fillRect(0, y - 20, LCD_WIDTH, bh + 40, RGB565_BLACK);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor((LCD_WIDTH - (int16_t)bw) / 2, y);
  gfx->print(text);
}

void drawLabel() {
  const char *label = "TAP TO COUNT";
  gfx->setTextSize(3);
  int16_t bx, by;
  uint16_t bw, bh;
  gfx->getTextBounds(label, 0, 0, &bx, &by, &bw, &bh);
  gfx->setTextColor(gfx->color565(120, 120, 120));
  gfx->setCursor((LCD_WIDTH - (int16_t)bw) / 2, LCD_HEIGHT - 110);
  gfx->print(label);
}

void setup() {
  launcherExitCheck();
  Serial.begin(115200);

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(200);

  touch.setPins(TP_RESET, TP_INT);
  if (!touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("CST9217 touch init failed!");
    gfx->setTextSize(3);
    gfx->setTextColor(RGB565_RED);
    gfx->setCursor(90, 220);
    gfx->print("TOUCH INIT FAILED");
    while (true) {
      delay(1000);
    }
  }
  Serial.printf("Touch controller: %s\n", touch.getModelName());

  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchInterrupt, FALLING);

  drawLabel();
  drawCounter();
  Serial.println("Tap counter ready");
}

void loop() {
  launcherExitTick();
  // Only read the controller when its INT line has fired; continuous polling
  // wedges the CST9217 and it stops reporting new touches.
  if (takeTouchInterrupt()) {
    int16_t x = 0, y = 0;
    const bool touched = touch.getPoint(&x, &y, 1) > 0;

    if (touched) {
      if (armed) {
        armed = false;
        counter++;
        drawCounter();
        Serial.printf("Tap %lu at (%d, %d)\n", (unsigned long)counter, x, y);
      }
      lastTouchReportMs = millis();
    }
  }

  if (!armed && millis() - lastTouchReportMs > kQuietRearmMs) {
    armed = true;
  }

  static uint32_t lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 2000) {
    lastHeartbeat = millis();
    Serial.printf("alive: counter=%lu\n", (unsigned long)counter);
  }
  delay(1);
}
