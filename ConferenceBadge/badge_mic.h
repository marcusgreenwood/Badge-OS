// Onboard dual-mic capture via ES7210 + I2S TDM, WAV to microSD.
// Pins match Waveshare ESP32-S3-Touch-AMOLED-1.75 hardware reference.
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <FS.h>
#include <SD_MMC.h>
#include <math.h>
#include "ESP_I2S.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace BadgeMic {

constexpr uint8_t kEs7210Addr = 0x40;
constexpr int kI2sBclk = 9;
constexpr int kI2sWs = 45;
constexpr int kI2sDin = 10;
constexpr int kI2sMclk = 42;
constexpr int kSdClk = 2;
constexpr int kSdCmd = 1;
constexpr int kSdD0 = 3;

constexpr uint32_t kSampleRate = 16000;
constexpr int kTdmSlots = 4;  // MIC1, MIC3(ref), MIC2, MIC4
constexpr int kBars = 9;

static I2SClass i2s;
static bool codecOk = false;
static bool i2sOk = false;
static bool sdOk = false;
static bool sdTried = false;
static uint32_t sdNextTryMs = 0;
static bool running = false;
static volatile bool recording = false;
static File recFile;
static uint32_t recDataBytes = 0;
static uint16_t recIndex = 0;
static char recPath[48] = "";
static char lastPath[48] = "";
static char statusMsg[40] = "MIC OFF";

static float barLevels[kBars] = {0};
static float peakLevel = 0;
static SemaphoreHandle_t levelMutex = nullptr;
static TaskHandle_t micTaskHandle = nullptr;

static bool esWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(kEs7210Addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool esRead(uint8_t reg, uint8_t *val) {
  Wire.beginTransmission(kEs7210Addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)kEs7210Addr, 1) != 1) return false;
  *val = Wire.read();
  return true;
}

static bool esUpdate(uint8_t reg, uint8_t mask, uint8_t data) {
  uint8_t v = 0;
  if (!esRead(reg, &v)) return false;
  return esWrite(reg, (uint8_t)((v & ~mask) | (data & mask)));
}

// 16 kHz @ MCLK = 256 * LRCK (ESPHome coefficient table)
static bool esConfigSampleRate16k() {
  // adc_div=3, doubler=1, dll=1, osr=0x20, lrck_h=0x03, lrck_l=0x00
  // → mclk 12288000 / lrck 16000
  if (!esWrite(0x02, (uint8_t)(0x03 | (1 << 6) | (1 << 7)))) return false;
  if (!esWrite(0x07, 0x20)) return false;
  if (!esWrite(0x04, 0x03)) return false;
  if (!esWrite(0x05, 0x00)) return false;
  return true;
}

static bool esInitCodec() {
  // Probe
  uint8_t probe = 0;
  if (!esRead(0x00, &probe)) {
    snprintf(statusMsg, sizeof(statusMsg), "NO ES7210");
    return false;
  }

  // Software reset + bring-up (ESPHome ES7210 sequence)
  if (!esWrite(0x00, 0xFF)) return false;
  delay(10);
  if (!esWrite(0x00, 0x32)) return false;
  if (!esWrite(0x01, 0x3F)) return false;
  if (!esWrite(0x09, 0x30)) return false;
  if (!esWrite(0x0A, 0x30)) return false;
  if (!esWrite(0x23, 0x2A)) return false;
  if (!esWrite(0x22, 0x0A)) return false;
  if (!esWrite(0x20, 0x0A)) return false;
  if (!esWrite(0x21, 0x2A)) return false;
  if (!esUpdate(0x08, 0x01, 0x00)) return false;
  if (!esWrite(0x40, 0xC3)) return false;
  if (!esWrite(0x41, 0x70)) return false;
  if (!esWrite(0x42, 0x70)) return false;

  // 16-bit + TDM
  if (!esWrite(0x11, 0x60)) return false;
  if (!esWrite(0x12, 0x02)) return false;
  if (!esConfigSampleRate16k()) return false;

  // Mic PGA ~34.5 dB (reg nibble 0x0C) — more sensitive level meters
  const uint8_t gain = 0x10 | 0x0C;
  if (!esWrite(0x43, gain)) return false;
  if (!esWrite(0x44, gain)) return false;
  if (!esWrite(0x45, 0x10 | 0x00)) return false;  // AEC ref quieter
  if (!esWrite(0x46, 0x10 | 0x00)) return false;

  if (!esWrite(0x47, 0x08)) return false;
  if (!esWrite(0x48, 0x08)) return false;
  if (!esWrite(0x49, 0x08)) return false;
  if (!esWrite(0x4A, 0x08)) return false;
  if (!esWrite(0x06, 0x04)) return false;
  if (!esWrite(0x4B, 0x00)) return false;
  if (!esWrite(0x4C, 0x0F)) return false;

  // Enable all ADC clocks for TDM
  if (!esWrite(0x01, 0x00)) return false;
  if (!esWrite(0x00, 0x71)) return false;
  if (!esWrite(0x00, 0x41)) return false;

  snprintf(statusMsg, sizeof(statusMsg), "MIC READY");
  return true;
}

static bool mountSd() {
  if (sdOk) return true;
  const uint32_t now = millis();
  if (sdTried && (int32_t)(now - sdNextTryMs) < 0) return false;
  sdTried = true;
  sdNextTryMs = now + 5000;  // retry at most every 5s

  if (!SD_MMC.setPins(kSdClk, kSdCmd, kSdD0)) {
    snprintf(statusMsg, sizeof(statusMsg), "SD PIN FAIL");
    return false;
  }
  // 1-bit SDMMC
  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT, 1)) {
    snprintf(statusMsg, sizeof(statusMsg), "NO SD CARD");
    return false;
  }
  sdOk = true;
  SD_MMC.mkdir("/BadgeOS");
  snprintf(statusMsg, sizeof(statusMsg), "SD READY");
  return true;
}

static void writeWavHeader(File &f, uint32_t dataBytes) {
  const uint32_t sampleRate = kSampleRate;
  const uint16_t channels = 2;
  const uint16_t bits = 16;
  const uint32_t byteRate = sampleRate * channels * (bits / 8);
  const uint16_t blockAlign = channels * (bits / 8);
  const uint32_t riffSize = 36 + dataBytes;

  auto w16 = [&](uint16_t v) {
    f.write((uint8_t)(v & 0xFF));
    f.write((uint8_t)(v >> 8));
  };
  auto w32 = [&](uint32_t v) {
    f.write((uint8_t)(v & 0xFF));
    f.write((uint8_t)((v >> 8) & 0xFF));
    f.write((uint8_t)((v >> 16) & 0xFF));
    f.write((uint8_t)((v >> 24) & 0xFF));
  };

  f.seek(0);
  f.write((const uint8_t *)"RIFF", 4);
  w32(riffSize);
  f.write((const uint8_t *)"WAVE", 4);
  f.write((const uint8_t *)"fmt ", 4);
  w32(16);
  w16(1);  // PCM
  w16(channels);
  w32(sampleRate);
  w32(byteRate);
  w16(blockAlign);
  w16(bits);
  f.write((const uint8_t *)"data", 4);
  w32(dataBytes);
}

static float clampf01(float v) {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static void updateLevels(const int16_t *tdm, size_t frames) {
  float bins[kBars] = {0};
  float peak = 0;
  for (size_t i = 0; i < frames; i++) {
    // Slot order: MIC1, MIC3, MIC2, MIC4
    const int32_t m1 = tdm[i * kTdmSlots + 0];
    const int32_t m2 = tdm[i * kTdmSlots + 2];
    const float a1 = fabsf((float)m1) / 32768.0f;
    const float a2 = fabsf((float)m2) / 32768.0f;
    const float a = a1 > a2 ? a1 : a2;
    if (a > peak) peak = a;
    const int bin = (int)((i * kBars) / frames);
    if (bin >= 0 && bin < kBars && a > bins[bin]) bins[bin] = a;
  }
  if (xSemaphoreTake(levelMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    for (int i = 0; i < kBars; i++) {
      // Hot gain so quiet speech fills the meter
      const float target = clampf01(bins[i] * 10.0f);
      if (target > barLevels[i]) barLevels[i] = target;
      else barLevels[i] = barLevels[i] * 0.78f + target * 0.22f;
    }
    const float pk = clampf01(peak * 10.0f);
    if (pk > peakLevel) peakLevel = pk;
    else peakLevel = peakLevel * 0.88f + pk * 0.12f;
    xSemaphoreGive(levelMutex);
  }
}

static void micTask(void *) {
  constexpr size_t kFrames = 128;
  int16_t tdm[kFrames * kTdmSlots];
  int16_t stereo[kFrames * 2];

  while (true) {
    if (!running || !i2sOk) {
      vTaskDelay(pdMS_TO_TICKS(80));
      continue;
    }

    const size_t want = sizeof(tdm);
    const size_t got = i2s.readBytes((char *)tdm, want);
    if (got < kTdmSlots * sizeof(int16_t)) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    const size_t frames = got / (kTdmSlots * sizeof(int16_t));
    // Only meter when UI needs it or we're writing audio
    if (recording || running) updateLevels(tdm, frames);

    if (recording && recFile) {
      for (size_t i = 0; i < frames; i++) {
        stereo[i * 2 + 0] = tdm[i * kTdmSlots + 0];  // MIC1
        stereo[i * 2 + 1] = tdm[i * kTdmSlots + 2];  // MIC2
      }
      const size_t bytes = frames * 2 * sizeof(int16_t);
      const size_t wr = recFile.write((const uint8_t *)stereo, bytes);
      recDataBytes += wr;
    } else {
      // Metering-only: yield so WiFi/BLE get CPU
      vTaskDelay(pdMS_TO_TICKS(8));
    }
  }
}

inline bool begin() {
  if (!levelMutex) levelMutex = xSemaphoreCreateMutex();

  codecOk = esInitCodec();
  if (!codecOk) {
    Serial.println("BadgeMic: ES7210 init failed");
    return false;
  }

  i2s.setPins(kI2sBclk, kI2sWs, -1, kI2sDin, kI2sMclk);
  // Don't start I2S / SD until capture or record — saves clock + bus power.
  i2sOk = false;
  running = false;
  if (!micTaskHandle) {
    xTaskCreatePinnedToCore(micTask, "badgeMic", 4096, nullptr, 2,
                            &micTaskHandle, 0);
  }
  snprintf(statusMsg, sizeof(statusMsg), "MIC READY");
  Serial.println("BadgeMic: codec ready (I2S/SD on demand)");
  return true;
}

inline bool ensureI2s() {
  if (i2sOk) return true;
  if (!codecOk) return false;
  const int8_t slotMask =
      (int8_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3);
  i2sOk = i2s.begin(I2S_MODE_TDM, kSampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                    I2S_SLOT_MODE_STEREO, slotMask);
  if (!i2sOk) {
    snprintf(statusMsg, sizeof(statusMsg), "I2S FAIL");
    Serial.println("BadgeMic: I2S TDM begin failed");
  }
  return i2sOk;
}

inline void resumeCapture() {
  if (!codecOk) return;
  if (!ensureI2s()) return;
  running = true;
}

inline void pauseCapture() {
  if (recording) return;  // never stop mid-file
  running = false;
  if (i2sOk) {
    i2s.end();
    i2sOk = false;
  }
}

inline void getLevels(float *outBars, int n, float *outPeak) {
  if (xSemaphoreTake(levelMutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    for (int i = 0; i < n && i < kBars; i++) outBars[i] = barLevels[i];
    if (outPeak) *outPeak = peakLevel;
    xSemaphoreGive(levelMutex);
  } else {
    for (int i = 0; i < n; i++) outBars[i] = 0;
    if (outPeak) *outPeak = 0;
  }
}

inline bool isRecording() { return recording; }
inline const char *message() { return statusMsg; }
inline const char *lastFile() { return lastPath; }
inline bool storageOk() { return sdOk; }
inline bool storageAvailable() { return sdOk || mountSd(); }

static uint16_t nextRecIndexFromSd() {
  uint16_t maxIdx = 0;
  File dir = SD_MMC.open("/BadgeOS");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 1;
  }
  File f = dir.openNextFile();
  while (f) {
    const char *name = f.name();
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    unsigned n = 0;
    if (sscanf(base, "rec_%u", &n) == 1 && n > maxIdx && n < 65535)
      maxIdx = (uint16_t)n;
    f.close();
    f = dir.openNextFile();
  }
  dir.close();
  return (uint16_t)(maxIdx + 1);
}

inline bool startRecording() {
  if (!codecOk) {
    snprintf(statusMsg, sizeof(statusMsg), "MIC NOT READY");
    return false;
  }
  resumeCapture();
  if (!i2sOk) {
    snprintf(statusMsg, sizeof(statusMsg), "MIC NOT READY");
    return false;
  }
  if (recording) return true;
  if (!mountSd()) return false;

  recIndex = nextRecIndexFromSd();
  snprintf(recPath, sizeof(recPath), "/BadgeOS/rec_%04u.wav",
           (unsigned)recIndex);
  recFile = SD_MMC.open(recPath, FILE_WRITE);
  if (!recFile) {
    snprintf(statusMsg, sizeof(statusMsg), "FILE OPEN FAIL");
    return false;
  }
  recDataBytes = 0;
  writeWavHeader(recFile, 0);
  recording = true;
  snprintf(statusMsg, sizeof(statusMsg), "REC %s", recPath);
  Serial.printf("BadgeMic: recording %s\n", recPath);
  return true;
}

inline bool stopRecording() {
  if (!recording) return false;
  recording = false;
  delay(20);  // let task finish current write
  if (recFile) {
    writeWavHeader(recFile, recDataBytes);
    recFile.flush();
    recFile.close();
  }
  strncpy(lastPath, recPath, sizeof(lastPath) - 1);
  lastPath[sizeof(lastPath) - 1] = 0;
  snprintf(statusMsg, sizeof(statusMsg), "SAVED %s", lastPath);
  Serial.printf("BadgeMic: saved %s (%lu bytes)\n", lastPath,
                (unsigned long)recDataBytes);
  return true;
}

}  // namespace BadgeMic
