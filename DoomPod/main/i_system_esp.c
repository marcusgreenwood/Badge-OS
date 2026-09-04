/* i_system for ESP-IDF 5.x - based on espressif/esp32-doom's compat layer.
 * WAD access is a flash partition (type 66, subtype 6) memory-mapped on
 * demand via esp_partition_mmap. Timing comes from gettimeofday.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <errno.h>

#include "config.h"
#include "m_argv.h"
#include "lprintf.h"
#include "doomtype.h"
#include "doomdef.h"
#include "m_fixed.h"
#include "r_fps.h"
#include "i_system.h"
#include "i_joy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"

int realtime = 0;

void I_uSleep(unsigned long usecs)
{
    unsigned long ms = usecs / 1000;
    vTaskDelay(ms > 0 ? pdMS_TO_TICKS(ms) : 1);
}

static unsigned long getMsTicks(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_usec / 1000 + tv.tv_sec * 1000;
}

int I_GetTime_RealTime(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec * TICRATE + (tv.tv_usec * TICRATE) / 1000000);
}

const int displaytime = 0;

fixed_t I_GetTimeFrac(void)
{
    unsigned long now = getMsTicks();
    fixed_t frac;

    if (tic_vars.step == 0) {
        return FRACUNIT;
    }
    frac = (fixed_t)((now - tic_vars.start + displaytime) * FRACUNIT / tic_vars.step);
    if (frac < 0) frac = 0;
    if (frac > FRACUNIT) frac = FRACUNIT;
    return frac;
}

void I_GetTime_SaveMS(void)
{
    if (!movement_smooth) return;
    tic_vars.start = getMsTicks();
    tic_vars.next = (unsigned int)((tic_vars.start * tic_vars.msec + 1.0f) / tic_vars.msec);
    tic_vars.step = tic_vars.next - tic_vars.start;
}

unsigned long I_GetRandomTimeSeed(void)
{
    return 4;  // per https://xkcd.com/221/
}

const char *I_GetVersionString(char *buf, size_t sz)
{
    snprintf(buf, sz, "%s v%s (http://prboom.sourceforge.net/)", PACKAGE, VERSION);
    return buf;
}

const char *I_SigString(char *buf, size_t sz, int signum)
{
    return buf;
}

// ---- WAD partition "file" access ----------------------------------------

typedef struct {
    const esp_partition_t *part;
    int offset;
    int size;
} FileDesc;

static FileDesc fds[32];

// The whole WAD partition is mapped once; every "mmap" is then just
// pointer arithmetic (IDF 5's MMU rejects/spams on overlapping mappings).
static const uint8_t *wadBase = NULL;

int I_Open(const char *wad, int flags)
{
    int x = 3;
    while (fds[x].part != NULL) x++;
    if (strcasecmp(wad, "DOOM1.WAD") == 0) {
        fds[x].part = esp_partition_find_first(66, 6, NULL);
        if (fds[x].part == NULL) {
            lprintf(LO_ERROR, "I_Open: WAD partition not found!\n");
            return -1;
        }
        fds[x].offset = 0;
        fds[x].size = fds[x].part->size;
        if (wadBase == NULL) {
            spi_flash_mmap_handle_t h;
            esp_err_t err = esp_partition_mmap(fds[x].part, 0, fds[x].part->size,
                                               ESP_PARTITION_MMAP_DATA,
                                               (const void **)&wadBase, &h);
            if (err != ESP_OK) {
                lprintf(LO_ERROR, "I_Open: whole-partition mmap failed: %x\n", err);
                return -1;
            }
            lprintf(LO_INFO, "I_Open: WAD mapped at %p\n", wadBase);
        }
    } else {
        lprintf(LO_INFO, "I_Open: open %s failed\n", wad);
        return -1;
    }
    return x;
}

int I_Lseek(int ifd, off_t offset, int whence)
{
    if (whence == SEEK_SET) {
        fds[ifd].offset = offset;
    } else if (whence == SEEK_CUR) {
        fds[ifd].offset += offset;
    } else if (whence == SEEK_END) {
        lprintf(LO_INFO, "I_Lseek: SEEK_END unimplemented\n");
    }
    return fds[ifd].offset;
}

int I_Filelength(int ifd)
{
    return fds[ifd].size;
}

void I_Close(int fd)
{
    fds[fd].part = NULL;
}

typedef struct {
    spi_flash_mmap_handle_t handle;
    void *addr;
    int offset;
    size_t len;
    int used;
} MmapHandle;

#define NO_MMAP_HANDLES 128
static MmapHandle mmapHandle[NO_MMAP_HANDLES];

static int nextHandle = 0;

static int getFreeHandle(void)
{
    int n = NO_MMAP_HANDLES;
    while (mmapHandle[nextHandle].used != 0 && n != 0) {
        nextHandle++;
        if (nextHandle == NO_MMAP_HANDLES) nextHandle = 0;
        n--;
    }
    if (n == 0) {
        lprintf(LO_ERROR, "I_Mmap: More mmaps than NO_MMAP_HANDLES!");
        exit(0);
    }
    if (mmapHandle[nextHandle].addr) {
        spi_flash_munmap(mmapHandle[nextHandle].handle);
        mmapHandle[nextHandle].addr = NULL;
    }
    int r = nextHandle;
    nextHandle++;
    if (nextHandle == NO_MMAP_HANDLES) nextHandle = 0;
    return r;
}

static void freeUnusedMmaps(void)
{
    for (int i = 0; i < NO_MMAP_HANDLES; i++) {
        if (mmapHandle[i].used == 0 && mmapHandle[i].addr != NULL) {
            spi_flash_munmap(mmapHandle[i].handle);
            mmapHandle[i].addr = NULL;
        }
    }
}

void *I_Mmap(void *addr, size_t length, int prot, int flags, int ifd, off_t offset)
{
    (void)addr; (void)prot; (void)flags; (void)ifd;
    if (wadBase == NULL) return NULL;
    return (void *)(wadBase + offset);
}

int I_Munmap(void *addr, size_t length)
{
    return 0;
}

void I_Read(int ifd, void *vbuf, size_t sz)
{
    memcpy(vbuf, wadBase + fds[ifd].offset, sz);
    fds[ifd].offset += sz;
}

const char *I_DoomExeDir(void)
{
    return "";
}

char *I_FindFile(const char *wfname, const char *ext)
{
    return NULL;
}

void I_SetAffinityMask(void)
{
}
