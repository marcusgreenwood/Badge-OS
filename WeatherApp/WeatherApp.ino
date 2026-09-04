// Weather app for Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466 round)
// Display: CO5300 over QSPI  |  Touch: CST9217 over I2C
// Weather: Open-Meteo (no API key), location auto-detected via ip-api.com
// Tap screen to refresh; auto-refreshes every 10 minutes.

#include <Arduino.h>
#include "launcher_exit.h"
#include "power_button.h"
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
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

const char *WIFI_SSID = "7LPR-wifi";
const char *WIFI_PASSWORD = "leof1nlay";

constexpr uint32_t kRefreshIntervalMs = 10 * 60 * 1000;
constexpr uint32_t kQuietRearmMs = 80;

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *panel = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

// The CO5300 needs even-aligned pixel writes; drawing primitives directly
// leaves streak artifacts. Render to a PSRAM framebuffer and flush whole
// frames instead.
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

TouchDrvCST92xx touch;

// Tap detection (see TapCounter: CST9217 interleaves empty frames while held,
// so re-arm only after a quiet period instead of edge-detecting)
uint32_t lastTouchReportMs = 0;
bool armed = true;
volatile bool touchPending = false;

struct WeatherData {
  bool valid = false;
  float latitude = 0;
  float longitude = 0;
  String city;
  float temperature = 0;
  float feelsLike = 0;
  int humidity = 0;
  float windSpeed = 0;
  int weatherCode = 0;
  String updatedTime;  // HH:MM local, from Open-Meteo timezone=auto
};

WeatherData weather;
uint32_t lastFetchMs = 0;

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

const char *weatherDescription(int code) {
  switch (code) {
    case 0: return "CLEAR SKY";
    case 1: return "MAINLY CLEAR";
    case 2: return "PARTLY CLOUDY";
    case 3: return "OVERCAST";
    case 45: case 48: return "FOG";
    case 51: case 53: case 55: case 56: case 57: return "DRIZZLE";
    case 61: case 63: return "RAIN";
    case 65: return "HEAVY RAIN";
    case 66: case 67: return "FREEZING RAIN";
    case 71: case 73: return "SNOW";
    case 75: case 77: return "HEAVY SNOW";
    case 80: case 81: return "SHOWERS";
    case 82: return "HEAVY SHOWERS";
    case 85: case 86: return "SNOW SHOWERS";
    case 95: return "THUNDERSTORM";
    case 96: case 99: return "THUNDER + HAIL";
    default: return "UNKNOWN";
  }
}

// ---- drawing helpers ---------------------------------------------------

void drawCenteredText(const char *text, int16_t y, uint8_t size, uint16_t color) {
  gfx->setTextSize(size);
  int16_t bx, by;
  uint16_t bw, bh;
  gfx->getTextBounds(text, 0, 0, &bx, &by, &bw, &bh);
  gfx->setTextColor(color);
  gfx->setCursor((LCD_WIDTH - (int16_t)bw) / 2, y);
  gfx->print(text);
}

void drawCloud(int16_t cx, int16_t cy, uint16_t color) {
  gfx->fillCircle(cx - 22, cy + 6, 16, color);
  gfx->fillCircle(cx, cy - 6, 22, color);
  gfx->fillCircle(cx + 24, cy + 6, 16, color);
  gfx->fillRect(cx - 22, cy + 6, 47, 17, color);
}

void drawWeatherIcon(int16_t cx, int16_t cy, int code) {
  const uint16_t sun = gfx->color565(255, 200, 40);
  const uint16_t cloud = gfx->color565(170, 180, 190);
  const uint16_t darkCloud = gfx->color565(110, 120, 130);
  const uint16_t rain = gfx->color565(80, 150, 255);
  const uint16_t snow = RGB565_WHITE;
  const uint16_t bolt = gfx->color565(255, 220, 0);

  if (code == 0 || code == 1) {
    // sun with rays
    gfx->fillCircle(cx, cy, 26, sun);
    for (int i = 0; i < 8; i++) {
      const float a = i * PI / 4;
      gfx->drawLine(cx + cosf(a) * 34, cy + sinf(a) * 34,
                    cx + cosf(a) * 44, cy + sinf(a) * 44, sun);
      gfx->drawLine(cx + cosf(a) * 34 + 1, cy + sinf(a) * 34,
                    cx + cosf(a) * 44 + 1, cy + sinf(a) * 44, sun);
    }
  } else if (code == 2) {
    // sun peeking behind cloud
    gfx->fillCircle(cx + 14, cy - 14, 18, sun);
    drawCloud(cx - 4, cy + 8, cloud);
  } else if (code == 3 || code == 45 || code == 48) {
    drawCloud(cx, cy, cloud);
    if (code != 3) {  // fog lines
      for (int i = 0; i < 3; i++) {
        gfx->fillRect(cx - 34, cy + 30 + i * 9, 68, 3, darkCloud);
      }
    }
  } else if (code >= 71 && code <= 86 && code != 80 && code != 81 && code != 82) {
    // snow
    drawCloud(cx, cy - 8, cloud);
    for (int i = 0; i < 3; i++) {
      const int16_t sx = cx - 20 + i * 20;
      gfx->fillCircle(sx, cy + 32, 3, snow);
      gfx->drawLine(sx - 5, cy + 32, sx + 5, cy + 32, snow);
      gfx->drawLine(sx, cy + 27, sx, cy + 37, snow);
    }
  } else if (code >= 95) {
    // thunderstorm
    drawCloud(cx, cy - 8, darkCloud);
    gfx->fillTriangle(cx + 4, cy + 12, cx - 12, cy + 34, cx - 1, cy + 32, bolt);
    gfx->fillTriangle(cx - 1, cy + 32, cx + 10, cy + 30, cx - 8, cy + 52, bolt);
  } else {
    // rain / drizzle / showers
    drawCloud(cx, cy - 8, code >= 80 ? darkCloud : cloud);
    for (int i = 0; i < 3; i++) {
      const int16_t rx = cx - 18 + i * 18;
      gfx->drawLine(rx, cy + 26, rx - 6, cy + 40, rain);
      gfx->drawLine(rx + 1, cy + 26, rx - 5, cy + 40, rain);
    }
  }
}

void drawStatusScreen(const char *line1, const char *line2, uint16_t color) {
  gfx->fillScreen(RGB565_BLACK);
  drawCenteredText(line1, 210, 3, color);
  if (line2 && line2[0]) {
    drawCenteredText(line2, 260, 2, gfx->color565(140, 140, 140));
  }
  gfx->flush();
}

void drawWeather() {
  gfx->fillScreen(RGB565_BLACK);

  if (!weather.valid) {
    drawStatusScreen("NO DATA", "TAP TO RETRY", RGB565_RED);
    return;
  }

  const uint16_t gray = gfx->color565(150, 150, 150);
  const uint16_t dimGray = gfx->color565(110, 110, 110);

  drawCenteredText(weather.city.c_str(), 74, 3, gray);

  drawWeatherIcon(LCD_WIDTH / 2, 175, weather.weatherCode);

  // big temperature, centered, with a drawn degree ring
  char tempText[16];
  snprintf(tempText, sizeof(tempText), "%.0f", weather.temperature);
  gfx->setTextSize(10);
  int16_t bx, by;
  uint16_t bw, bh;
  gfx->getTextBounds(tempText, 0, 0, &bx, &by, &bw, &bh);
  const int16_t tx = (LCD_WIDTH - (int16_t)bw - 24) / 2;
  const int16_t ty = 248;
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(tx, ty);
  gfx->print(tempText);
  gfx->drawCircle(tx + bw + 16, ty + 8, 8, RGB565_WHITE);
  gfx->drawCircle(tx + bw + 16, ty + 8, 7, RGB565_WHITE);

  drawCenteredText(weatherDescription(weather.weatherCode), 338, 3,
                   gfx->color565(200, 200, 200));

  char stats[48];
  snprintf(stats, sizeof(stats), "FEELS %.0fC  HUM %d%%  WIND %.0f",
           weather.feelsLike, weather.humidity, weather.windSpeed);
  drawCenteredText(stats, 378, 2, gray);

  char footer[32];
  snprintf(footer, sizeof(footer), "UPDATED %s - TAP", weather.updatedTime.c_str());
  drawCenteredText(footer, 408, 2, dimGray);

  gfx->flush();
}

// ---- data fetching -----------------------------------------------------

bool httpGetJson(const String &url, JsonDocument &doc) {
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(url)) {
    Serial.printf("http.begin failed: %s\n", url.c_str());
    return false;
  }
  const int status = http.GET();
  if (status != 200) {
    Serial.printf("GET %s -> %d\n", url.c_str(), status);
    http.end();
    return false;
  }
  const DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) {
    Serial.printf("JSON parse error: %s\n", err.c_str());
    return false;
  }
  return true;
}

bool fetchLocation() {
  JsonDocument doc;
  if (!httpGetJson("http://ip-api.com/json/?fields=status,city,lat,lon", doc)) {
    return false;
  }
  if (String(doc["status"] | "") != "success") {
    Serial.println("ip-api returned non-success");
    return false;
  }
  weather.latitude = doc["lat"] | 0.0f;
  weather.longitude = doc["lon"] | 0.0f;
  weather.city = String(doc["city"] | "UNKNOWN");
  weather.city.toUpperCase();
  Serial.printf("Location: %s (%.3f, %.3f)\n", weather.city.c_str(),
                weather.latitude, weather.longitude);
  return true;
}

bool fetchWeather() {
  String url = "http://api.open-meteo.com/v1/forecast?latitude=" +
               String(weather.latitude, 4) + "&longitude=" +
               String(weather.longitude, 4) +
               "&current=temperature_2m,apparent_temperature,relative_humidity_2m,"
               "weather_code,wind_speed_10m&timezone=auto";
  JsonDocument doc;
  if (!httpGetJson(url, doc)) {
    return false;
  }
  JsonObject current = doc["current"];
  if (current.isNull()) {
    Serial.println("no 'current' object in response");
    return false;
  }
  weather.temperature = current["temperature_2m"] | 0.0f;
  weather.feelsLike = current["apparent_temperature"] | 0.0f;
  weather.humidity = current["relative_humidity_2m"] | 0;
  weather.weatherCode = current["weather_code"] | -1;
  weather.windSpeed = current["wind_speed_10m"] | 0.0f;
  const String isoTime = String(current["time"] | "");  // e.g. 2026-07-20T18:45
  weather.updatedTime = isoTime.length() >= 16 ? isoTime.substring(11, 16) : "--:--";
  weather.valid = true;
  Serial.printf("Weather: %.1fC (feels %.1f), hum %d%%, wind %.1f, code %d, %s\n",
                weather.temperature, weather.feelsLike, weather.humidity,
                weather.windSpeed, weather.weatherCode, weather.updatedTime.c_str());
  return true;
}

bool ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  Serial.printf("Connecting to %s", WIFI_SSID);
  drawStatusScreen("CONNECTING WIFI", WIFI_SSID, RGB565_WHITE);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connect failed");
    return false;
  }
  Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
  return true;
}

void refresh() {
  lastFetchMs = millis();
  if (!ensureWifi()) {
    drawStatusScreen("WIFI FAILED", "TAP TO RETRY", RGB565_RED);
    return;
  }
  drawStatusScreen("UPDATING...", weather.city.length() ? weather.city.c_str() : "", RGB565_WHITE);
  const bool haveLocation = weather.city.length() > 0 || fetchLocation();
  if (!haveLocation || !fetchWeather()) {
    if (weather.valid) {
      drawWeather();  // keep showing stale data on a failed refresh
    } else {
      drawStatusScreen("FETCH FAILED", "TAP TO RETRY", RGB565_RED);
    }
    return;
  }
  drawWeather();
}

// ---- arduino entry points ----------------------------------------------

void setup() {
  launcherExitCheck();
  powerButtonInit();
  Serial.begin(115200);

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  gfx->fillScreen(RGB565_BLACK);
  gfx->flush();
  panel->setBrightness(200);

  touch.setPins(TP_RESET, TP_INT);
  if (!touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
    Serial.println("CST9217 touch init failed!");
  } else {
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchInterrupt, FALLING);
  }

  refresh();
}

void loop() {
  powerButtonTick();
  launcherExitTick();
  bool tapped = false;
  if (takeTouchInterrupt()) {
    int16_t x = 0, y = 0;
    if (touch.getPoint(&x, &y, 1) > 0) {
      if (armed) {
        armed = false;
        tapped = true;
        Serial.printf("Tap at (%d, %d) -> refresh\n", x, y);
      }
      lastTouchReportMs = millis();
    }
  }
  if (!armed && millis() - lastTouchReportMs > kQuietRearmMs) {
    armed = true;
  }

  if (tapped || millis() - lastFetchMs > kRefreshIntervalMs) {
    refresh();
  }

  static uint32_t lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= 5000) {
    lastHeartbeat = millis();
    Serial.printf("alive: valid=%d wifi=%d temp=%.1f\n",
                  weather.valid, WiFi.status() == WL_CONNECTED, weather.temperature);
  }
  delay(1);
}
