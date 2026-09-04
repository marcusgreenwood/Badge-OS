// DOOM on the Waveshare ESP32-S3-Touch-AMOLED-1.75
// PrBoom engine from espressif/esp32-doom, re-glued to ESP-IDF 5.4 with the
// official Waveshare BSP (CO5300 display + CST9217 touch).

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "doom_pod.h"

static const char *TAG = "doompod";

// One-shot boot policy: otadata is erased as soon as the app starts, so the
// launcher's selection applies to this boot only - any reset (button, crash,
// power cycle) lands back on the launcher. The side BOOT button (GPIO0)
// held ~200ms simply restarts, which goes home per the above.
static void launcherExitTask(void *arg)
{
    int heldMs = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(0) == 0) {
            heldMs += 50;
            if (heldMs >= 200) {
                esp_restart();
            }
        } else {
            heldMs = 0;
        }
    }
}

static void launcherExitCheck(void)
{
    const esp_partition_t *otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (otadata) {
        esp_partition_erase_range(otadata, 0, otadata->size);
    }
    // configure the BOOT button pin here on the main task's big stack:
    // gpio_config's first-time IO-mux interrupt allocation is too deep for
    // a small task stack (see the FighterJet ipc1 lesson)
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&btn);
    xTaskCreate(launcherExitTask, "lexit", 4096, NULL, 1, NULL);
}

esp_lcd_panel_handle_t doom_panel = NULL;
esp_lcd_touch_handle_t doom_touch = NULL;
SemaphoreHandle_t doom_flush_done = NULL;

static bool onColorTransDone(esp_lcd_panel_io_handle_t io,
                             esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(doom_flush_done, &woken);
    return woken == pdTRUE;
}

extern int doom_main(int argc, char const *const *argv);

static void doomEngineTask(void *pvParameters)
{
    char const *argv[] = {"doom", "-cout", "ICWEFDA", NULL};
    doom_main(3, argv);
    vTaskDelete(NULL);
}

void app_main(void)
{
    launcherExitCheck();

    const esp_partition_t *part = esp_partition_find_first(66, 6, NULL);
    if (part == NULL) {
        ESP_LOGE(TAG, "WAD partition (type 66 subtype 6) not found!");
    } else {
        ESP_LOGI(TAG, "WAD partition: %lu bytes @ 0x%lx", part->size, part->address);
    }

    bsp_display_config_t disp_cfg = {0};
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_ERROR_CHECK(bsp_display_new(&disp_cfg, &doom_panel, &io));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(doom_panel, true));
    bsp_display_brightness_init();
    bsp_display_brightness_set(90);

    doom_flush_done = xSemaphoreCreateBinary();
    xSemaphoreGive(doom_flush_done);
    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = onColorTransDone};
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL));

    // clear panel to black
    {
        const int rows = 8;
        uint16_t *line = heap_caps_calloc(BSP_LCD_H_RES * rows, 2, MALLOC_CAP_DMA);
        for (int y = 0; y < BSP_LCD_V_RES; y += rows) {
            xSemaphoreTake(doom_flush_done, portMAX_DELAY);
            esp_lcd_panel_draw_bitmap(doom_panel, 0, y, BSP_LCD_H_RES, y + rows, line);
        }
        xSemaphoreTake(doom_flush_done, portMAX_DELAY);
        free(line);
        xSemaphoreGive(doom_flush_done);
    }

    bsp_touch_config_t touch_cfg = {0};
    if (bsp_touch_new(&touch_cfg, &doom_touch) != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed - controls will be dead");
        doom_touch = NULL;
    }

    doom_intro_show();

    xTaskCreatePinnedToCore(&doomEngineTask, "doomEngine", 32768, NULL, 5, NULL, 1);
}
