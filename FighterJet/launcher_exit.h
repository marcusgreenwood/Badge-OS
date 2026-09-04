// One-shot boot policy: every reset returns to the launcher (factory app).
// Call launcherExitCheck() first in setup() and launcherExitTick() in loop().
//
// launcherExitCheck erases otadata as soon as the app starts, so the current
// app selection applies to this boot only - the next reset (button, crash,
// or power cycle) always lands on the launcher.
// launcherExitTick polls the side BOOT button (GPIO0): held ~200ms it
// restarts, which lands on the launcher per the above.
#pragma once

#include <Arduino.h>
#include "esp_partition.h"
#include "esp_system.h"

static inline void launcherExitCheck(void) {
  const esp_partition_t *otadata = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
  if (otadata) {
    esp_partition_erase_range(otadata, 0, otadata->size);
  }
}

static inline void launcherExitTick(void) {
  static uint32_t pressStart = 0;
  static bool inited = false;
  if (!inited) {
    inited = true;
    pinMode(0, INPUT_PULLUP);
  }
  if (digitalRead(0) == LOW) {
    if (pressStart == 0) {
      pressStart = millis();
    } else if (millis() - pressStart > 200) {
      esp_restart();
    }
  } else {
    pressStart = 0;
  }
}
