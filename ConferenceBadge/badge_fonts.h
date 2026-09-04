#pragma once
#include "badge_smooth_font.h"

#define kSmoothFontSizeCount 2

#include "fonts/font_helvetica_28.h"
#include "fonts/font_helvetica_42.h"

#include "fonts/font_archivo_28.h"
#include "fonts/font_archivo_42.h"

#include "fonts/font_spacegrotesk_28.h"
#include "fonts/font_spacegrotesk_42.h"

#include "fonts/font_chivo_28.h"
#include "fonts/font_chivo_42.h"

#include "fonts/font_plexmono_28.h"
#include "fonts/font_plexmono_42.h"

static const BadgeSmoothFont *const kSmoothFonts[5][kSmoothFontSizeCount] = {
  { &font_helvetica_28, &font_helvetica_42 },
  { &font_archivo_28, &font_archivo_42 },
  { &font_spacegrotesk_28, &font_spacegrotesk_42 },
  { &font_chivo_28, &font_chivo_42 },
  { &font_plexmono_28, &font_plexmono_42 },
};

