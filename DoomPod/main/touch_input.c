/* Touch controls for DOOM on the round AMOLED.
 *
 *   drag anywhere      -> virtual joystick: up/down = move, left/right = turn
 *   second finger down -> fire (while dragging)
 *   quick tap right    -> fire + menu-select
 *   quick tap left     -> use / open doors
 *   long press (still) -> escape (menu open/close)
 *
 * Polled from I_StartTic on the game thread; posts key events exactly like
 * the original esp32-doom gamepad shim (bitmask diff -> D_PostEvent).
 */

#include <stdlib.h>
#include <string.h>

#include "doomdef.h"
#include "doomtype.h"
#include "m_argv.h"
#include "d_event.h"
#include "g_game.h"
#include "d_main.h"
#include "lprintf.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_touch.h"
#include "doom_pod.h"

// The panel reports Y inverted relative to the display (board quirk)
#define TOUCH_FLIP_Y 1
#define SCREEN_RES 466

// virtual button bits
#define VB_UP     0x01
#define VB_DOWN   0x02
#define VB_LEFT   0x04
#define VB_RIGHT  0x08
#define VB_FIRE   0x10
#define VB_USE    0x20
#define VB_ESC    0x40
#define VB_ENTER  0x80

#define DRAG_DEADZONE 30   // px from press origin before a direction engages
#define MOVE_THRESHOLD 25  // px before a press counts as a drag, not a tap
#define TAP_MAX_MS 400
#define LONGPRESS_MS 600
#define PULSE_MS 120

// these keyboard-emulation globals must live somewhere (see gamepad.c)
int usejoystick = 0;
int joyleft, joyright, joyup, joydown;

typedef struct {
    int mask;
    int *key;
} VbKeyMap;

static const VbKeyMap keymap[] = {
    {VB_UP, &key_up},
    {VB_DOWN, &key_down},
    {VB_LEFT, &key_left},
    {VB_RIGHT, &key_right},
    {VB_FIRE, &key_fire},
    {VB_ENTER, &key_menu_enter},
    {VB_USE, &key_use},
    {VB_ESC, &key_escape},
    {0, NULL},
};

static bool wasTouched = false;
static bool dragging = false;
static bool escSent = false;
static int16_t startX, startY;
static int64_t pressStartUs = 0;
static int64_t pulseEndUs = 0;
static int pulseMask = 0;

static int readTouch(int16_t *x, int16_t *y, uint8_t *count)
{
    uint16_t tx[2], ty[2];
    uint8_t n = 0;
    if (doom_touch == NULL) return 0;
    esp_lcd_touch_read_data(doom_touch);
    bool pressed = esp_lcd_touch_get_coordinates(doom_touch, tx, ty, NULL, &n, 2);
    if (!pressed || n == 0) {
        *count = 0;
        return 0;
    }
    *x = (int16_t)tx[0];
#if TOUCH_FLIP_Y
    *y = (int16_t)(SCREEN_RES - 1 - ty[0]);
#else
    *y = (int16_t)ty[0];
#endif
    *count = n;
    return 1;
}

void touchInputPoll(void)
{
    static int oldMask = 0;
    int mask = 0;

    const int64_t now = esp_timer_get_time();
    int16_t x = 0, y = 0;
    uint8_t fingers = 0;
    const bool touched = readTouch(&x, &y, &fingers);

    if (touched && !wasTouched) {
        // new press
        startX = x;
        startY = y;
        pressStartUs = now;
        dragging = false;
        escSent = false;
    }

    if (touched) {
        const int dx = x - startX;
        const int dy = y - startY;
        if (!dragging && (abs(dx) > MOVE_THRESHOLD || abs(dy) > MOVE_THRESHOLD)) {
            dragging = true;
        }
        if (dragging) {
            if (dy < -DRAG_DEADZONE) mask |= VB_UP;      // drag up = forward
            if (dy > DRAG_DEADZONE) mask |= VB_DOWN;
            if (dx < -DRAG_DEADZONE) mask |= VB_LEFT;
            if (dx > DRAG_DEADZONE) mask |= VB_RIGHT;
            if (fingers >= 2) mask |= VB_FIRE;           // second finger = fire
        } else if (!escSent && now - pressStartUs > (int64_t)LONGPRESS_MS * 1000) {
            escSent = true;
            pulseMask = VB_ESC;
            pulseEndUs = now + PULSE_MS * 1000;
        }
    } else if (wasTouched) {
        // release
        const int64_t heldMs = (now - pressStartUs) / 1000;
        if (!dragging && !escSent && heldMs < TAP_MAX_MS) {
            pulseMask = (startX >= SCREEN_RES / 2) ? (VB_FIRE | VB_ENTER) : VB_USE;
            pulseEndUs = now + PULSE_MS * 1000;
        }
        dragging = false;
    }
    wasTouched = touched;

    if (pulseMask && now < pulseEndUs) {
        mask |= pulseMask;
    } else {
        pulseMask = 0;
    }

    // diff -> key events
    for (int i = 0; keymap[i].key != NULL; i++) {
        if ((oldMask ^ mask) & keymap[i].mask) {
            event_t ev;
            ev.type = (mask & keymap[i].mask) ? ev_keydown : ev_keyup;
            ev.data1 = *keymap[i].key;
            D_PostEvent(&ev);
        }
    }
    oldMask = mask;
}
