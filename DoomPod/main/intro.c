// Pre-game intro for DoomPod: the "can it run DOOM?" meme, then the touch
// controls. Rendered with an embedded 8x8 font straight into the panel via
// the same banded DMA path the game uses. Each screen advances on tap.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "doom_pod.h"
#include "font8x8_basic.h"

#define W 466
#define H 466
#define BAND 32  // rows per draw_bitmap call (ESP32-S3 SPI 32KB transaction cap)

static uint16_t *fb;

static uint16_t be(uint8_t r, uint8_t g, uint8_t b) {
  const uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  return (uint16_t)((c >> 8) | (c << 8));
}

static void fbClear(uint16_t c) {
  for (int i = 0; i < W * H; i++) fb[i] = c;
}

static int textWidth(const char *s, int scale) {
  return (int)strlen(s) * 8 * scale;
}

static void drawText(const char *s, int x, int y, int scale, uint16_t color) {
  for (; *s; s++, x += 8 * scale) {
    const unsigned ch = (unsigned char)*s;
    if (ch < 32 || ch > 127) continue;
    const char *glyph = font8x8_basic[ch];
    for (int gy = 0; gy < 8; gy++) {
      const uint8_t row = (uint8_t)glyph[gy];
      for (int gx = 0; gx < 8; gx++) {
        if (!(row & (1 << gx))) continue;
        for (int sy = 0; sy < scale; sy++) {
          const int py = y + gy * scale + sy;
          if (py < 0 || py >= H) continue;
          uint16_t *dst = fb + py * W + x + gx * scale;
          for (int sx = 0; sx < scale; sx++) {
            const int px = x + gx * scale + sx;
            if (px >= 0 && px < W) dst[sx] = color;
          }
        }
      }
    }
  }
}

static void centerText(const char *s, int y, int scale, uint16_t color) {
  drawText(s, (W - textWidth(s, scale)) / 2, y, scale, color);
}

static void fillRect(int x, int y, int w, int h, uint16_t c) {
  for (int py = y; py < y + h && py < H; py++) {
    if (py < 0) continue;
    uint16_t *row = fb + py * W;
    for (int px = x; px < x + w && px < W; px++) {
      if (px >= 0) row[px] = c;
    }
  }
}

static void flush(void) {
  for (int y = 0; y < H; y += BAND) {
    xSemaphoreTake(doom_flush_done, portMAX_DELAY);
    if (esp_lcd_panel_draw_bitmap(doom_panel, 0, y, W, y + BAND, fb + y * W) != ESP_OK) {
      xSemaphoreGive(doom_flush_done);
    }
  }
}

// wait for a fresh tap (press after a quiet gap, then release); falls back
// to a timed wait when touch is unavailable
static void waitTap(int timeoutMs) {
  if (doom_touch == NULL) {
    vTaskDelay(pdMS_TO_TICKS(4000));
    return;
  }
  uint16_t tx[2], ty[2];
  uint8_t n = 0;
  int quiet = 0, pressed = 0;
  const int64_t limit = timeoutMs > 0 ? timeoutMs : 0x7fffffff;
  for (int64_t t = 0; t < limit; t += 20) {
    esp_lcd_touch_read_data(doom_touch);
    const bool down =
        esp_lcd_touch_get_coordinates(doom_touch, tx, ty, NULL, &n, 2) && n > 0;
    if (!pressed) {
      if (down && quiet > 35) pressed = 1;  // fresh press after >700ms quiet
      quiet = down ? 0 : quiet + 1;
    } else if (!down) {
      return;  // released
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static const uint16_t INK_ = 0;  // avoid clashing with any macro
#define C_BG be(14, 10, 10)
#define C_RED be(214, 48, 32)
#define C_CREAM be(250, 246, 236)
#define C_GRAY be(150, 142, 132)
#define C_YELLOW be(255, 205, 60)

static void screenMeme(void) {
  fbClear(C_BG);
  fillRect(0, 60, W, 4, C_RED);
  centerText("CAN IT RUN", 92, 4, C_CREAM);
  centerText("DOOM?", 132, 6, C_RED);
  fillRect(0, 196, W, 4, C_RED);

  centerText("SINCE 1993 DOOM HAS RUN ON", 222, 2, C_GRAY);
  centerText("OSCILLOSCOPES. TRACTORS.", 248, 2, C_CREAM);
  centerText("PREGNANCY TESTS. A POTATO.", 274, 2, C_CREAM);
  centerText("AND NOW: A 1.75 INCH", 306, 2, C_GRAY);
  centerText("ROUND WATCH SCREEN", 328, 2, C_YELLOW);

  centerText("TAP TO ANSWER THE QUESTION", 384, 2, C_RED);
  flush();
}

static void screenControls(void) {
  fbClear(C_BG);
  centerText("HOW TO RIP AND TEAR", 66, 3, C_YELLOW);
  fillRect(93, 100, 280, 3, C_RED);

  int y = 130;
  centerText("DRAG UP/DOWN = MOVE", y, 2, C_CREAM); y += 30;
  centerText("DRAG LEFT/RIGHT = TURN", y, 2, C_CREAM); y += 30;
  centerText("SECOND FINGER = FIRE", y, 2, C_CREAM); y += 38;
  centerText("TAP RIGHT SIDE = FIRE", y, 2, C_CREAM); y += 30;
  centerText("TAP LEFT SIDE = OPEN DOORS", y, 2, C_CREAM); y += 30;
  centerText("HOLD STILL = MENU", y, 2, C_CREAM); y += 38;

  centerText("SIDE BUTTON: HOLD 4S = OFF", y, 2, C_GRAY); y += 26;
  centerText("RESET = BACK TO HOTEL TOWER", y, 2, C_GRAY);

  centerText("TAP TO ENTER HELL", 396, 2, C_RED);
  flush();
}

void doom_intro_show(void) {
  fb = heap_caps_malloc((size_t)W * H * 2, MALLOC_CAP_SPIRAM);
  if (fb == NULL) return;

  // Ignore residual contact from the badge's 10s hold that launched us.
  vTaskDelay(pdMS_TO_TICKS(700));
  for (int i = 0; i < 25 && doom_touch; i++) {
    uint16_t tx[2], ty[2];
    uint8_t n = 0;
    esp_lcd_touch_read_data(doom_touch);
    if (!(esp_lcd_touch_get_coordinates(doom_touch, tx, ty, NULL, &n, 2) &&
          n > 0))
      break;
    vTaskDelay(pdMS_TO_TICKS(40));
  }

  screenMeme();
  waitTap(30000);
  screenControls();
  waitTap(30000);

  fbClear(0);
  flush();
  // wait for the last band before freeing the buffer
  xSemaphoreTake(doom_flush_done, portMAX_DELAY);
  xSemaphoreGive(doom_flush_done);
  free(fb);
  fb = NULL;
}
