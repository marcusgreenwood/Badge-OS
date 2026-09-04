/* i_video for the Waveshare ESP32-S3-Touch-AMOLED-1.75.
 * DOOM renders 320x240 8-bit palette frames; we upscale 1.4x with a
 * nearest-neighbour LUT to 448x336 RGB565 (big-endian for the CO5300)
 * and push centered onto the 466x466 panel via esp_lcd.
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "m_argv.h"
#include "doomstat.h"
#include "doomdef.h"
#include "doomtype.h"
#include "v_video.h"
#include "r_draw.h"
#include "d_main.h"
#include "d_event.h"
#include "i_video.h"
#include "z_zone.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "st_stuff.h"
#include "lprintf.h"

#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "doom_pod.h"

// panel geometry
#define PANEL_RES   466
#define OUT_W       448   // 320 * 1.4
#define OUT_H       336   // 240 * 1.4
#define OUT_X       8     // (466-448)/2 = 9, rounded down to even for CO5300
#define OUT_Y       64    // (466-336)/2 = 65, rounded down to even
#define BAND_ROWS   32    // rows per draw_bitmap call; ESP32-S3 SPI caps a
                          // single transaction at 32KB (448*32*2 = 28KB)

int use_fullscreen = 0;
int use_doublebuffer = 0;

static uint16_t lcdpal[256];
static unsigned char *screenbuf;      // 320x240 8-bit, internal RAM
static uint16_t *outbuf;              // 448x336 RGB565, PSRAM
static uint16_t xmap[OUT_W];
static uint8_t ymap[OUT_H + 1];       // src row per out row (0..239)

void I_StartTic(void)
{
    touchInputPoll();
}

static void I_InitInputs(void)
{
}

void I_ShutdownGraphics(void)
{
}

void I_UpdateNoBlit(void)
{
}

void I_StartFrame(void)
{
}

int I_StartDisplay(void)
{
    return true;
}

void I_EndDisplay(void)
{
}

void I_FinishUpdate(void)
{
    const uint8_t *src = (const uint8_t *)screens[0].data;
    if (!src || !outbuf || !doom_panel) return;

    for (int y = 0; y < OUT_H; y++) {
        const uint8_t *srow = src + (int)ymap[y] * SCREENPITCH;
        uint16_t *orow = outbuf + y * OUT_W;
        for (int x = 0; x < OUT_W; x += 4) {
            orow[x] = lcdpal[srow[xmap[x]]];
            orow[x + 1] = lcdpal[srow[xmap[x + 1]]];
            orow[x + 2] = lcdpal[srow[xmap[x + 2]]];
            orow[x + 3] = lcdpal[srow[xmap[x + 3]]];
        }
    }

    for (int y = 0; y < OUT_H; y += BAND_ROWS) {
        const int h = (y + BAND_ROWS <= OUT_H) ? BAND_ROWS : (OUT_H - y);
        xSemaphoreTake(doom_flush_done, portMAX_DELAY);
        if (esp_lcd_panel_draw_bitmap(doom_panel, OUT_X, OUT_Y + y,
                                      OUT_X + OUT_W, OUT_Y + y + h,
                                      outbuf + y * OUT_W) != ESP_OK) {
            xSemaphoreGive(doom_flush_done);  // failed draws never call back
        }
    }
}

void I_SetPalette(int pal)
{
    int pplump = W_GetNumForName("PLAYPAL");
    const byte *palette = W_CacheLumpNum(pplump);
    palette += pal * (3 * 256);
    for (int i = 0; i < 256; i++) {
        uint16_t v = ((palette[0] >> 3) << 11) | ((palette[1] >> 2) << 5) | (palette[2] >> 3);
        lcdpal[i] = (v >> 8) | (v << 8);  // big-endian for the panel
        palette += 3;
    }
    W_UnlockLumpNum(pplump);
}

void I_PreInitGraphics(void)
{
    lprintf(LO_INFO, "I_PreInitGraphics (internal free %u, largest %u)\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    screenbuf = heap_caps_malloc(SCREENWIDTH * SCREENHEIGHT,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (screenbuf == NULL) {
        // internal RAM too fragmented (the intro screens shift the layout);
        // PSRAM is slower for the renderer but entirely playable
        lprintf(LO_INFO, "internal alloc failed, render buffer in PSRAM\n");
        screenbuf = heap_caps_malloc(SCREENWIDTH * SCREENHEIGHT,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    assert(screenbuf);
    outbuf = heap_caps_malloc(OUT_W * OUT_H * 2, MALLOC_CAP_SPIRAM);
    assert(outbuf);

    for (int x = 0; x < OUT_W; x++) {
        xmap[x] = x * SCREENWIDTH / OUT_W;
    }
    for (int y = 0; y < OUT_H; y++) {
        ymap[y] = y * SCREENHEIGHT / OUT_H;
    }
}

void I_SetRes(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        screens[i].width = SCREENWIDTH;
        screens[i].height = SCREENHEIGHT;
        screens[i].byte_pitch = SCREENPITCH;
        screens[i].short_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE16);
        screens[i].int_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE32);
    }
    screens[4].width = SCREENWIDTH;
    screens[4].height = (ST_SCALED_HEIGHT + 1);
    screens[4].byte_pitch = SCREENPITCH;
    screens[4].short_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE16);
    screens[4].int_pitch = SCREENPITCH / V_GetModePixelDepth(VID_MODE32);

    screens[0].not_on_heap = true;
    screens[0].data = screenbuf;
    assert(screens[0].data);

    lprintf(LO_INFO, "I_SetRes: Using resolution %dx%d\n", SCREENWIDTH, SCREENHEIGHT);
}

void I_InitGraphics(void)
{
    static int firsttime = 1;
    if (firsttime) {
        firsttime = 0;
        atexit(I_ShutdownGraphics);
        lprintf(LO_INFO, "I_InitGraphics: %dx%d\n", SCREENWIDTH, SCREENHEIGHT);
        I_UpdateVideoMode();
        I_InitInputs();
    }
}

void I_UpdateVideoMode(void)
{
    lprintf(LO_INFO, "I_UpdateVideoMode: %dx%d\n", SCREENWIDTH, SCREENHEIGHT);

    V_InitMode(VID_MODE8);
    V_DestroyUnusedTrueColorPalettes();
    V_FreeScreens();
    I_SetRes();
    V_AllocScreens();
    R_InitBuffer(SCREENWIDTH, SCREENHEIGHT);
}
