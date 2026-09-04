#pragma once
#include <stdint.h>
#ifndef PROGMEM
#define PROGMEM
#endif

// Company logo lockup (icon + wordmark). Swap the bitmaps to rebrand.
static const int COMPANY_MARK_W = 107;
static const int COMPANY_MARK_H = 40;
static const uint16_t COMPANY_MARK_KEY = 0x07E0;

#include "company_mark_dark.h"
#include "company_mark_light.h"
