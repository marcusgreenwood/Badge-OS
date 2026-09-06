// On-screen keyboard sized for circular 466×466 AMOLED.
// Each row uses the local chord width so keys fill the round safe area.
#pragma once

#include <Arduino.h>
#include <math.h>
#include <string.h>

constexpr int KB_NONE = 0;
constexpr int KB_BACK = -1;
constexpr int KB_SHIFT = -2;
constexpr int KB_MODE = -3;
constexpr int KB_SPACE = -4;
constexpr int KB_OK = -5;

struct RoundKeyboard {
  char text[65];
  uint8_t len;
  bool shift;
  bool symbols;
};

struct KbKey {
  int16_t x, y, w, h;
  char label[8];
  int code;
};

static constexpr int kKbMaxKeys = 40;
static KbKey gKbKeys[kKbMaxKeys];
static int gKbKeyN = 0;

// Display geometry (matches ConferenceBadge kCx/kCy / LCD 466)
static constexpr int kKbCx = 233;
static constexpr int kKbCy = 233;
static constexpr int kKbSafeR = 222;

static inline void kbInit(RoundKeyboard *kb) { memset(kb, 0, sizeof(*kb)); }

static inline void kbClear(RoundKeyboard *kb) {
  kb->text[0] = 0;
  kb->len = 0;
}

static inline void kbSet(RoundKeyboard *kb, const char *s) {
  strncpy(kb->text, s ? s : "", sizeof(kb->text) - 1);
  kb->text[sizeof(kb->text) - 1] = 0;
  kb->len = (uint8_t)strlen(kb->text);
}

static void kbPush(int x, int y, int w, int h, const char *lab, int code) {
  if (gKbKeyN >= kKbMaxKeys) return;
  KbKey &k = gKbKeys[gKbKeyN++];
  k.x = (int16_t)x;
  k.y = (int16_t)y;
  k.w = (int16_t)w;
  k.h = (int16_t)h;
  strncpy(k.label, lab, sizeof(k.label) - 1);
  k.label[sizeof(k.label) - 1] = 0;
  k.code = code;
}

static int kbChordHalf(int yMid) {
  const int dy = abs(yMid - kKbCy);
  if (dy >= kKbSafeR) return 90;
  const float half =
      sqrtf((float)(kKbSafeR * kKbSafeR - dy * dy)) - 6.0f;
  return half > 90.0f ? (int)half : 90;
}

static void kbBuildLayout(const RoundKeyboard *kb, int originY) {
  gKbKeyN = 0;
  // Tall keys — 4 rows fill most of the lower circle
  const int rowH = 54;
  const int gap = 5;
  const int keyH = rowH - gap;

  auto addRow = [&](int row, const char *chars) {
    const int n = (int)strlen(chars);
    const int y = originY + row * rowH;
    const int half = kbChordHalf(y + keyH / 2);
    const int usable = half * 2;
    int keyW = (usable - (n - 1) * gap) / n;
    if (keyW < 28) keyW = 28;
    const int total = n * keyW + (n - 1) * gap;
    int x = kKbCx - total / 2;
    for (int i = 0; i < n; i++) {
      char lab[2] = {chars[i], 0};
      if (!kb->symbols && kb->shift && lab[0] >= 'a' && lab[0] <= 'z')
        lab[0] = (char)(lab[0] - 'a' + 'A');
      kbPush(x, y, keyW, keyH, lab, (unsigned char)lab[0]);
      x += keyW + gap;
    }
  };

  if (!kb->symbols) {
    addRow(0, "qwertyuiop");
    addRow(1, "asdfghjkl");
    addRow(2, "zxcvbnm");
  } else {
    addRow(0, "1234567890");
    addRow(1, "-/:;()$&@");
    addRow(2, ".,?!'\"#");
  }

  // Bottom action row — same chord width as letter rows
  const int y = originY + 3 * rowH;
  const int half = kbChordHalf(y + keyH / 2);
  const int usable = half * 2;
  // Proportions: MODE | SHIFT | SPACE | DEL | OK
  const int wMode = (int)(usable * 0.14f);
  const int wShift = (int)(usable * 0.13f);
  const int wDel = (int)(usable * 0.13f);
  const int wOk = (int)(usable * 0.18f);
  const int wSpace = usable - wMode - wShift - wDel - wOk - 4 * gap;
  int x = kKbCx - usable / 2;
  kbPush(x, y, wMode, keyH, kb->symbols ? "ABC" : "123", KB_MODE);
  x += wMode + gap;
  kbPush(x, y, wShift, keyH, "SH", KB_SHIFT);
  x += wShift + gap;
  kbPush(x, y, wSpace, keyH, "SPACE", KB_SPACE);
  x += wSpace + gap;
  kbPush(x, y, wDel, keyH, "DEL", KB_BACK);
  x += wDel + gap;
  kbPush(x, y, wOk, keyH, "OK", KB_OK);
}

static int kbHit(int tx, int ty) {
  for (int i = 0; i < gKbKeyN; i++) {
    const KbKey &k = gKbKeys[i];
    if (tx >= k.x && tx < k.x + k.w && ty >= k.y && ty < k.y + k.h)
      return k.code;
  }
  return KB_NONE;
}

static bool kbApply(RoundKeyboard *kb, int code) {
  if (code == KB_NONE) return false;
  if (code == KB_SHIFT) {
    kb->shift = !kb->shift;
    return true;
  }
  if (code == KB_MODE) {
    kb->symbols = !kb->symbols;
    kb->shift = false;
    return true;
  }
  if (code == KB_BACK) {
    if (kb->len > 0) kb->text[--kb->len] = 0;
    return true;
  }
  if (code == KB_SPACE) {
    if (kb->len + 1 < (int)sizeof(kb->text)) {
      kb->text[kb->len++] = ' ';
      kb->text[kb->len] = 0;
    }
    return true;
  }
  if (code == KB_OK) return true;
  if (code >= 32 && code < 127) {
    if (kb->len + 1 < (int)sizeof(kb->text)) {
      kb->text[kb->len++] = (char)code;
      kb->text[kb->len] = 0;
      if (kb->shift && !kb->symbols) kb->shift = false;
    }
    return true;
  }
  return false;
}
