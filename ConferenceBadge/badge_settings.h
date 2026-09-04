// Badge OS initial settings — edit this file to configure the badge.
// Identity fields come from profile.h (tools/scrape_linkedin_badge.py).
#pragma once

#include "profile.h"

// ---- Theme defaults (Settings screen can override at runtime via NVS) ----
// Surface: 0 = Black, 1 = Sand
#ifndef BADGE_SURFACE
#define BADGE_SURFACE 0
#endif
// Palette: index into kAccents[] (Orange, Cobalt, Yellow, … Achromatic)
#ifndef BADGE_PALETTE
#define BADGE_PALETTE 1
#endif
// Dial ticks around the rim
#ifndef BADGE_TICKS
#define BADGE_TICKS 1
#endif
// Type: 0 Helvetica, 1 Archivo, 2 Space Grotesk, 3 Chivo, 4 Plex Mono
#ifndef BADGE_FONT
#define BADGE_FONT 0
#endif

// Name split for Identity (falls back to BADGE_NAME if unset)
#ifndef BADGE_NAME_L1
#define BADGE_NAME_L1 "MARCUS"
#endif
#ifndef BADGE_NAME_L2
#define BADGE_NAME_L2 "GREENWOOD"
#endif

#ifndef BADGE_WIFI_LABEL
#define BADGE_WIFI_LABEL "CONF-5G"
#endif

#ifndef BADGE_BOOK_URL
#define BADGE_BOOK_URL "https://cal.com/marcusgreenwood/15min"
#endif

#ifndef BADGE_BLE_NAME
#define BADGE_BLE_NAME BADGE_NAME_L1 "/" BADGE_COMPANY
#endif

// ---- Schedule (demo / static until live sync exists) ----
#ifndef BADGE_SESSION_TIME
#define BADGE_SESSION_TIME "14:30"
#endif
#ifndef BADGE_SESSION_IN
#define BADGE_SESSION_IN "IN 12 MIN"
#endif
#ifndef BADGE_SESSION_TITLE
#define BADGE_SESSION_TITLE "AGENTIC BROWSING AT SCALE"
#endif
// Progress around schedule ring (0..100)
#ifndef BADGE_SESSION_PCT
#define BADGE_SESSION_PCT 20
#endif

// ---- Status labels ----
static const char *const kStatusLabels[] = {
    "OPEN TO CHAT",
    "IN A MEETING",
    "HEADS DOWN",
    "COFFEE RUN",
};
static const int kStatusN = sizeof(kStatusLabels) / sizeof(kStatusLabels[0]);
// Tone: 0=accent, 1=dim, else packed RGB as 0xRRGGBB in kStatusToneRgb
static const uint8_t kStatusTone[] = {0, 2, 1, 3};
static const uint32_t kStatusToneRgb[] = {0, 0, 0xC2341A, 0xC98A00};

// ---- Icebreakers ----
static const char *const kIcebreakers[] = {
    "WHY WE KILLED OUR ROADMAP",
    "BROWSER AGENTS THAT DO NOT BREAK",
    "HIRING ENGINEER No. 1",
    "THE WORST DEMO I EVER GAVE",
};
static const int kIcebreakerN = sizeof(kIcebreakers) / sizeof(kIcebreakers[0]);

// ---- Inbox demo queue ----
struct BadgeNotif {
  const char *name;
  const char *body;
};
static const BadgeNotif kNotifs[] = {
    {"PRIYA RAO", "wants to swap contacts"},
    {"LUCAS BERG", "invites you to lunch, 12:45"},
    {"STAGE 2", "your mic check is in 20 min"},
};
static const int kNotifN = sizeof(kNotifs) / sizeof(kNotifs[0]);

// ---- Radar demo blips (angle deg, radius px from centre of 300px radar) ----
struct BadgeRadarBlip {
  const char *name;
  int16_t angleDeg;
  int16_t radius;
};
static const BadgeRadarBlip kRadar[] = {
    {"PRIYA R.", -62, 112},
    {"TOM K.", 34, 84},
    {"AYA N.", 134, 130},
    {"LUCAS B.", -146, 98},
};
static const int kRadarN = sizeof(kRadar) / sizeof(kRadar[0]);

// ---- Screen metadata ----
enum BadgeFace : uint8_t {
  FACE_IDENTITY = 0,
  FACE_CONNECT,
  FACE_SCHEDULE,
  FACE_STATUS,
  FACE_INBOX,
  FACE_SYSTEM,
  FACE_ICEBREAKER,
  FACE_ARCADE,
  FACE_RADAR,
  FACE_SETTINGS,
  FACE_COUNT
};

static const char *const kFaceCode[FACE_COUNT] = {
    "IDENTITY", "CONNECT", "SCHEDULE", "STATUS", "INBOX",
    "SYSTEM",   "ICEBREAKER", "ARCADE", "RADAR", "SETTINGS",
};

static const char *const kFaceShort[FACE_COUNT] = {
    "ID", "QR", "NEXT", "STAT", "INBOX", "SYS", "ASK", "PLAY", "NEAR", "SET",
};

static const char *const kFaceTop[FACE_COUNT] = {
    "BADGE OS",        "SCAN TO CONNECT", "NEXT SESSION", "AVAILABILITY",
    "INBOX",           "SYSTEM",          "ASK ME ABOUT", "HOTEL TOWER",
    "NEARBY",          "SETTINGS",
};

#ifndef BADGE_CONNECT_BOT
#define BADGE_CONNECT_BOT BADGE_NAME_L1 " - " BADGE_COMPANY
#endif

static const char *const kFaceBot[FACE_COUNT] = {
    "SPEAKER - STAGE 2", BADGE_CONNECT_BOT, "STAGE 2 - YOU SPEAK",
    "SET BY MARCUS - 09:12", "BLE - DELIVERED 09:41", "14H REMAINING",
    "TAP TO SHUFFLE", "CONFERENCE LEADERBOARD", "BLE PROXIMITY - LIVE",
    "STORED ON DEVICE",
};
