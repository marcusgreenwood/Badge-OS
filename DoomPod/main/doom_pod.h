// Shared handles between app_main and the DOOM glue layer
#pragma once

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern esp_lcd_panel_handle_t doom_panel;
extern esp_lcd_touch_handle_t doom_touch;

// draw_bitmap is async; take this before every draw call (given back by the
// on_color_trans_done callback when the previous transfer finishes)
extern SemaphoreHandle_t doom_flush_done;

// touch_input.c
void touchInputPoll(void);  // called from I_StartTic on the game thread

// intro.c: "can it run DOOM?" + controls screens, shown before the engine
void doom_intro_show(void);
