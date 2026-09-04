// PWR side button = on/off switch, via the AXP2101 PMU.
// Call powerButtonInit() in setup() and powerButtonTick() in loop().
//
//   short press          -> clean power-off (PMU cuts the rails)
//   press while off      -> hardware power-on (PMU boots the board)
//   hold ~4s             -> hard cutoff, handled inside the PMU itself,
//                           works even if the firmware is wedged
//
// The PMU registers persist across warm reboots, so the 4s failsafe set
// here also covers apps that don't poll (e.g. DOOM).
#pragma once

#include <Arduino.h>
#include <Wire.h>
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

static XPowersPMU __pmu;
static bool __pmuOk = false;

static inline void powerButtonInit() {
  __pmuOk = __pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, 15, 14);
  if (!__pmuOk) {
    Serial.println("AXP2101 not found - power button disabled");
    return;
  }
  __pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  __pmu.clearIrqStatus();
  __pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
  __pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
  // fuel gauge / battery telemetry
  __pmu.enableBattDetection();
  __pmu.enableBattVoltageMeasure();
  __pmu.enableVbusVoltageMeasure();
}

// -1 if no PMU or no battery attached, else 0-100
static inline int powerBatteryPercent() {
  if (!__pmuOk || !__pmu.isBatteryConnect()) return -1;
  return __pmu.getBatteryPercent();
}

static inline bool powerIsCharging() {
  return __pmuOk && __pmu.isCharging();
}

static inline void powerButtonTick() {
  if (!__pmuOk) return;
  static uint32_t lastPoll = 0;
  if (millis() - lastPoll < 150) return;
  lastPoll = millis();
  __pmu.getIrqStatus();
  if (__pmu.isPekeyShortPressIrq()) {
    __pmu.clearIrqStatus();
    Serial.println("power key: shutting down");
    delay(50);
    __pmu.shutdown();
  }
  __pmu.clearIrqStatus();
}
