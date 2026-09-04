// Shared Hotel Tower promo / challenge code helper (keep in sync with HotelTower).
#pragma once

#include <stdint.h>
#include <stdio.h>

// Promo codes are hu-{hash}: 8 chars from a legible alphabet (no 0/1/i/l/o),
// derived deterministically from the floor count via splitmix64 with a fixed
// salt. One code per floor >= 50 (and for challenge display at any best > 0).
static inline void makePromoCode(int floors, char *out, size_t sz) {
  uint64_t x = (uint64_t)floors * 0x9E3779B97F4A7C15ULL ^ 0x48554E4956455253ULL;
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBULL;
  x ^= x >> 31;
  static const char alpha[] = "23456789abcdefghjkmnpqrstuvwxyz";
  char h[9];
  for (int i = 0; i < 8; i++) {
    h[i] = alpha[x % 31];
    x /= 31;
  }
  h[8] = '\0';
  snprintf(out, sz, "hu-%s", h);
}
