// F/A-18 Hornet in 3D for Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466)
// Two control modes, toggled by the on-screen button at the bottom:
//   TILT - QMI8658 accelerometer: tilt the board to pitch & roll the jet
//   DRAG - drag on the touchscreen to spin it (with inertia)
// Render pipeline: PSRAM double-buffer, big-endian colors, DMA flush on
// core 0 while core 1 renders. Faces are drawn two-sided via painter's sort.

#include <Arduino.h>
#include "launcher_exit.h"
#include "power_button.h"
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "SensorQMI8658.hpp"
#include "TouchDrvCSTXXX.hpp"

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_RESET 39
#define LCD_WIDTH 466
#define LCD_HEIGHT 466

#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 11
#define TP_RESET 40

// ---- types (before any function: the Arduino preprocessor inserts
// auto-generated prototypes right before the first function definition) ----

struct Vec3 {
  float x, y, z;
};

struct Tri {
  Vec3 a, b, c;
  uint8_t r, g, b_;
};

struct DrawTri {
  int16_t x0, y0, x1, y1, x2, y2;
  float depth;
  uint16_t color;
};

// hexagonal fuselage cross-section: flat deck, angled sides, flat belly
struct Section {
  float z, cy, w, h;
};

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *panel = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

class SwapCanvas : public Arduino_Canvas {
 public:
  using Arduino_Canvas::Arduino_Canvas;
  void setFB(uint16_t *f) { _framebuffer = f; }
};

SwapCanvas *gfx = new SwapCanvas(LCD_WIDTH, LCD_HEIGHT, panel);
uint16_t *bufs[2] = {nullptr, nullptr};
int curBuf = 0;
QueueHandle_t flushQueue;
SemaphoreHandle_t renderReady;

SensorQMI8658 qmi;
TouchDrvCST92xx touch;
bool imuOk = false;
bool touchOk = false;

void flushTask(void *) {
  uint16_t *frame;
  for (;;) {
    if (xQueueReceive(flushQueue, &frame, portMAX_DELAY) == pdTRUE) {
      panel->draw16bitBeRGBBitmap(0, 0, frame, LCD_WIDTH, LCD_HEIGHT);
      xSemaphoreGive(renderReady);
      vTaskDelay(1);  // let IDLE0 run so the task watchdog stays fed
    }
  }
}

volatile bool touchPending = false;
void IRAM_ATTR onTouchInterrupt() { touchPending = true; }

bool takeTouchInterrupt() {
  noInterrupts();
  const bool pending = touchPending;
  touchPending = false;
  interrupts();
  return pending;
}

// ---- model -------------------------------------------------------------

constexpr int kMaxTris = 260;
Tri mesh[kMaxTris];
int numTris = 0;

uint16_t beColor(uint8_t r, uint8_t g, uint8_t b) {
  const uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  return __builtin_bswap16(c);
}

void addTri(Vec3 a, Vec3 b, Vec3 c, uint8_t r, uint8_t g, uint8_t bl) {
  if (numTris < kMaxTris) {
    mesh[numTris++] = {a, b, c, r, g, bl};
  }
}

void addQuad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint8_t r, uint8_t g, uint8_t bl) {
  addTri(a, b, c, r, g, bl);
  addTri(a, c, d, r, g, bl);
}

Vec3 mirrorX(Vec3 v) { return {-v.x, v.y, v.z}; }

void addQuadM(Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint8_t r, uint8_t g, uint8_t bl) {
  addQuad(a, b, c, d, r, g, bl);
  addQuad(mirrorX(a), mirrorX(b), mirrorX(c), mirrorX(d), r, g, bl);
}

void addTriM(Vec3 a, Vec3 b, Vec3 c, uint8_t r, uint8_t g, uint8_t bl) {
  addTri(a, b, c, r, g, bl);
  addTri(mirrorX(a), mirrorX(b), mirrorX(c), r, g, bl);
}

void sectionPoints(const Section &s, Vec3 out[6]) {
  out[0] = {-0.45f * s.w, s.cy + s.h, s.z};  // deck left
  out[1] = {0.45f * s.w, s.cy + s.h, s.z};   // deck right
  out[2] = {s.w, s.cy, s.z};                 // right side
  out[3] = {0.45f * s.w, s.cy - s.h, s.z};   // belly right
  out[4] = {-0.45f * s.w, s.cy - s.h, s.z};  // belly left
  out[5] = {-s.w, s.cy, s.z};                // left side
}

// Coordinates: +x right wing, +y up, +z nose.
void buildJet() {
  const uint8_t AR = 172, AG = 178, AB = 186;  // upper airframe gray
  const uint8_t BLR = 132, BLG = 138, BLB = 148;  // belly gray
  const uint8_t RR = 105, RG = 108, RB = 115;  // radome
  const uint8_t CR = 55, CG = 90, CB = 150;    // canopy glass
  const uint8_t NR = 66, NG = 64, NB = 66;     // nozzle metal
  const uint8_t IR = 25, IG = 25, IB = 30;     // intake mouth

  // ---- fuselage loft ----
  const Section st[] = {
      {2.05f, 0.02f, 0.085f, 0.085f},  // radome
      {1.45f, 0.03f, 0.15f, 0.13f},    // fwd fuselage
      {0.70f, 0.02f, 0.22f, 0.16f},    // cockpit / LEX root
      {-0.15f, 0.00f, 0.27f, 0.17f},   // wing root
      {-0.85f, -0.01f, 0.25f, 0.15f},  // aft
      {-1.45f, 0.00f, 0.19f, 0.12f},   // between the nozzles
  };
  constexpr int kSt = sizeof(st) / sizeof(st[0]);
  Vec3 p0[6], p1[6];
  const Vec3 noseTip = {0, 0.02f, 2.35f};
  sectionPoints(st[0], p0);
  for (int i = 0; i < 6; i++) {  // radome fan
    addTri(noseTip, p0[i], p0[(i + 1) % 6], RR, RG, RB);
  }
  for (int s = 0; s + 1 < kSt; s++) {
    sectionPoints(st[s], p0);
    sectionPoints(st[s + 1], p1);
    for (int i = 0; i < 6; i++) {
      const int j = (i + 1) % 6;
      const bool belly = (i == 3);  // p3->p4 is the flat bottom
      const bool deck = (i == 0);
      addQuad(p0[i], p0[j], p1[j], p1[i],
              deck ? AR : (belly ? BLR : AR - 18),
              deck ? AG : (belly ? BLG : AG - 18),
              deck ? AB : (belly ? BLB : AB - 16));
    }
  }
  sectionPoints(st[kSt - 1], p0);  // tail cap
  addQuad(p0[0], p0[1], p0[2], p0[3], NR, NG, NB);
  addQuad(p0[0], p0[3], p0[4], p0[5], NR, NG, NB);

  // ---- canopy: windscreen wedge + main bubble, on the fwd deck ----
  addTriM({0.09f, 0.16f, 1.30f}, {0.11f, 0.17f, 1.02f}, {0.0f, 0.33f, 0.98f}, CR, CG, CB);
  addTri({-0.09f, 0.16f, 1.30f}, {0.09f, 0.16f, 1.30f}, {0.0f, 0.33f, 0.98f}, CR + 25, CG + 25, CB + 25);
  addQuadM({0.11f, 0.17f, 1.02f}, {0.12f, 0.18f, 0.42f},
           {0.0f, 0.36f, 0.38f}, {0.0f, 0.33f, 0.98f}, CR, CG, CB);
  addTri({-0.12f, 0.18f, 0.42f}, {0.12f, 0.18f, 0.42f}, {0.0f, 0.36f, 0.38f}, CR + 20, CG + 20, CB + 20);

  // ---- dorsal spine behind the canopy ----
  addQuadM({0.10f, 0.17f, 0.40f}, {0.11f, 0.16f, -0.60f},
           {0.0f, 0.26f, -0.65f}, {0.0f, 0.30f, 0.36f}, AR - 10, AG - 10, AB - 10);

  // ---- LEX strakes: slim blades from the nose back over the intakes ----
  addQuadM({0.13f, 0.05f, 1.55f}, {0.24f, 0.05f, 0.80f},
           {0.44f, 0.03f, 0.15f}, {0.26f, 0.05f, 0.55f}, BLR, BLG, BLB);
  addTriM({0.24f, 0.05f, 0.80f}, {0.44f, 0.03f, 0.15f}, {0.30f, 0.04f, 0.55f}, BLR, BLG, BLB);

  // ---- intakes: boxes tucked under the LEX ----
  addQuadM({0.27f, -0.02f, 0.45f}, {0.38f, -0.02f, 0.40f},
           {0.36f, -0.02f, -0.30f}, {0.26f, -0.02f, -0.30f}, AR - 25, AG - 25, AB - 25);  // top shelf
  addQuadM({0.38f, -0.02f, 0.40f}, {0.38f, -0.15f, 0.38f},
           {0.36f, -0.16f, -0.30f}, {0.36f, -0.02f, -0.30f}, BLR - 15, BLG - 15, BLB - 15);  // outer wall
  addQuadM({0.38f, -0.15f, 0.38f}, {0.27f, -0.16f, 0.42f},
           {0.26f, -0.17f, -0.30f}, {0.36f, -0.16f, -0.30f}, BLR - 25, BLG - 25, BLB - 25);  // floor
  addQuadM({0.27f, -0.02f, 0.45f}, {0.38f, -0.02f, 0.40f},
           {0.38f, -0.15f, 0.38f}, {0.27f, -0.16f, 0.42f}, IR, IG, IB);  // dark mouth

  // ---- main wings (thin plates; two-sided lighting handles undersides) ----
  addQuadM({0.27f, 0.0f, 0.40f},    // root leading edge
           {1.28f, 0.0f, -0.12f},   // tip leading edge
           {1.28f, 0.0f, -0.50f},   // tip trailing edge
           {0.27f, 0.0f, -0.55f},   // root trailing edge
           AR, AG, AB);
  // trailing-edge flaps, slightly darker
  addQuadM({0.27f, 0.0f, -0.55f}, {1.00f, 0.0f, -0.46f},
           {1.00f, -0.005f, -0.60f}, {0.27f, -0.005f, -0.70f}, AR - 22, AG - 22, AB - 22);
  // wingtip launch rails + sidewinders
  addQuadM({1.28f, -0.03f, 0.05f}, {1.33f, -0.03f, 0.02f},
           {1.33f, -0.03f, -0.52f}, {1.28f, -0.03f, -0.52f}, BLR - 20, BLG - 20, BLB - 20);
  addTriM({1.305f, -0.03f, 0.30f}, {1.33f, -0.06f, 0.0f}, {1.28f, -0.06f, 0.0f}, 210, 210, 214);  // missile nose
  addQuadM({1.33f, -0.06f, 0.0f}, {1.28f, -0.06f, 0.0f},
           {1.28f, -0.06f, -0.55f}, {1.33f, -0.06f, -0.55f}, 200, 200, 205);  // missile body

  // ---- stabilators (swept rear tails) ----
  addQuadM({0.17f, -0.01f, -1.02f}, {0.74f, -0.01f, -1.38f},
           {0.74f, -0.01f, -1.60f}, {0.17f, -0.01f, -1.50f}, AR - 8, AG - 8, AB - 8);

  // ---- twin vertical tails, canted ~25 deg outward, swept ----
  addQuadM({0.16f, 0.12f, -0.55f},   // base front
           {0.17f, 0.10f, -1.18f},   // base rear
           {0.43f, 0.68f, -1.38f},   // top rear
           {0.43f, 0.70f, -1.05f},   // top front
           AR - 20, AG - 20, AB - 20);
  // red flash along the tail tip
  addQuadM({0.415f, 0.62f, -1.05f}, {0.43f, 0.70f, -1.05f},
           {0.43f, 0.68f, -1.38f}, {0.415f, 0.60f, -1.36f}, 180, 40, 40);

  // ---- engine nozzles: twin frustums ----
  const float nx = 0.10f;  // nozzle center offset
  for (int sgn = -1; sgn <= 1; sgn += 2) {
    const float cx = sgn * nx;
    const Vec3 f[4] = {{cx - 0.08f, 0.06f, -1.45f}, {cx + 0.08f, 0.06f, -1.45f},
                       {cx + 0.08f, -0.10f, -1.45f}, {cx - 0.08f, -0.10f, -1.45f}};
    const Vec3 r[4] = {{cx - 0.06f, 0.04f, -1.68f}, {cx + 0.06f, 0.04f, -1.68f},
                       {cx + 0.06f, -0.08f, -1.68f}, {cx - 0.06f, -0.08f, -1.68f}};
    for (int i = 0; i < 4; i++) {
      const int j = (i + 1) % 4;
      addQuad(f[i], f[j], r[j], r[i], NR, NG, NB);
    }
    addQuad(r[0], r[1], r[2], r[3], IR + 20, IG, IB);  // hot dark opening
  }
}

// ---- 3D pipeline -------------------------------------------------------

constexpr float kCamDist = 4.4f;
constexpr float kFocal = 360.0f;

float rotM[3][3];

void buildRotation(float yaw, float pitch, float roll) {
  const float cy = cosf(yaw), sy = sinf(yaw);
  const float cp = cosf(pitch), sp = sinf(pitch);
  const float cr = cosf(roll), sr = sinf(roll);
  rotM[0][0] = cy * cr + sy * sp * sr;
  rotM[0][1] = -cy * sr + sy * sp * cr;
  rotM[0][2] = sy * cp;
  rotM[1][0] = cp * sr;
  rotM[1][1] = cp * cr;
  rotM[1][2] = -sp;
  rotM[2][0] = -sy * cr + cy * sp * sr;
  rotM[2][1] = sy * sr + cy * sp * cr;
  rotM[2][2] = cy * cp;
}

Vec3 transform(const Vec3 &v) {
  return {rotM[0][0] * v.x + rotM[0][1] * v.y + rotM[0][2] * v.z,
          rotM[1][0] * v.x + rotM[1][1] * v.y + rotM[1][2] * v.z,
          rotM[2][0] * v.x + rotM[2][1] * v.y + rotM[2][2] * v.z + kCamDist};
}

bool project(const Vec3 &cam, int16_t &sx, int16_t &sy) {
  if (cam.z < 0.4f) return false;
  sx = LCD_WIDTH / 2 + (int16_t)(kFocal * cam.x / cam.z);
  sy = LCD_HEIGHT / 2 - (int16_t)(kFocal * cam.y / cam.z);
  return true;
}

DrawTri drawList[kMaxTris];
int drawCount = 0;

int compareDepth(const void *a, const void *b) {
  const float d = ((const DrawTri *)b)->depth - ((const DrawTri *)a)->depth;
  return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

const Vec3 kLightDir = {0.35f, 0.85f, -0.4f};

void renderJet() {
  drawCount = 0;
  for (int i = 0; i < numTris; i++) {
    const Tri &tri = mesh[i];
    const Vec3 a = transform(tri.a);
    const Vec3 b = transform(tri.b);
    const Vec3 c = transform(tri.c);

    DrawTri &d = drawList[drawCount];
    if (!project(a, d.x0, d.y0) || !project(b, d.x1, d.y1) ||
        !project(c, d.x2, d.y2)) {
      continue;
    }
    d.depth = (a.z + b.z + c.z) / 3;

    const Vec3 e1 = {b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 e2 = {c.x - a.x, c.y - a.y, c.z - a.z};
    const Vec3 n = {e1.y * e2.z - e1.z * e2.y,
                    e1.z * e2.x - e1.x * e2.z,
                    e1.x * e2.y - e1.y * e2.x};
    const float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
    float lambert = 0;
    if (len > 0) {
      lambert = fabsf(n.x * kLightDir.x + n.y * kLightDir.y + n.z * kLightDir.z) / len;
    }
    const float shade = 0.30f + 0.70f * lambert;
    d.color = beColor((uint8_t)(tri.r * shade), (uint8_t)(tri.g * shade),
                      (uint8_t)(tri.b_ * shade));
    drawCount++;
  }

  qsort(drawList, drawCount, sizeof(DrawTri), compareDepth);
  for (int i = 0; i < drawCount; i++) {
    const DrawTri &d = drawList[i];
    gfx->fillTriangle(d.x0, d.y0, d.x1, d.y1, d.x2, d.y2, d.color);
  }
}

// ---- controls ----------------------------------------------------------

enum Mode { MODE_TILT, MODE_DRAG };
Mode mode = MODE_TILT;

float pitch = 0, roll = 0, yaw = -0.65f;
float yawVel = 0, pitchVel = 0;  // drag inertia

// mode toggle button (bottom center of the round screen)
constexpr int16_t kBtnX = LCD_WIDTH / 2;
constexpr int16_t kBtnY = 414;
constexpr int16_t kBtnR = 44;

bool inButton(int16_t x, int16_t y) {
  const int32_t dx = x - kBtnX, dy = y - kBtnY;
  return dx * dx + dy * dy <= (int32_t)kBtnR * kBtnR;
}

uint32_t lastTouchReportMs = 0;
bool armed = true;
bool dragActive = false;
bool pendingToggle = false;
int16_t dragX = 0, dragY = 0;

// The CST9217 reports taps on the bottom-center of the panel as a "home
// button" gesture (zero touch points), so the mode button also listens to
// the driver's home-button callback.
volatile bool homePressed = false;
uint32_t lastToggleMs = 0;

void onHomeButton(void *) { homePressed = true; }

void toggleMode() {
  const uint32_t now = millis();
  if (now - lastToggleMs < 400) return;
  lastToggleMs = now;
  mode = (mode == MODE_TILT) ? MODE_DRAG : MODE_TILT;
  yawVel = pitchVel = 0;
  Serial.printf("Mode -> %s\n", mode == MODE_TILT ? "TILT" : "DRAG");
}

// Sustained reads can wedge the CST9217. Recovery must stay lightweight:
// tearing down Wire / interrupts here overflows the tiny IPC task stack
// (ipc1 stack canary panic). A hardware reset of the chip via its reset
// pin plus a driver re-init is enough, and touches nothing else.
void recoverTouch() {
  Serial.println("touch: recovering (chip reset)");
  pinMode(TP_RESET, OUTPUT);
  digitalWrite(TP_RESET, LOW);
  delay(5);
  digitalWrite(TP_RESET, HIGH);
  delay(60);
  touchOk = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (touchOk) {
    touch.setCoverScreenCallback(onHomeButton);
    Wire.setClock(400000);
    Wire.setTimeOut(20);
  }
  Serial.printf("touch: recovery %s\n", touchOk ? "ok" : "FAILED");
}

uint32_t lastFrameMs = 0, fpsWindowStart = 0;
uint16_t fpsFrames = 0;
float fps = 0;

void setup() {
  launcherExitCheck();
  powerButtonInit();
  Serial.begin(115200);

  if (!gfx->begin(80000000)) {
    Serial.println("gfx->begin() failed!");
  }
  bufs[0] = gfx->getFramebuffer();
  bufs[1] = (uint16_t *)heap_caps_malloc(
      (size_t)LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
  flushQueue = xQueueCreate(1, sizeof(uint16_t *));
  renderReady = xSemaphoreCreateCounting(2, 2);
  xTaskCreatePinnedToCore(flushTask, "flush", 4096, nullptr, 2, nullptr, 0);
  panel->setBrightness(230);

  imuOk = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (imuOk) {
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                            SensorQMI8658::ACC_ODR_250Hz,
                            SensorQMI8658::LPF_MODE_0);
    qmi.enableAccelerometer();
  } else {
    Serial.println("QMI8658 init failed");
    mode = MODE_DRAG;
  }

  touch.setPins(TP_RESET, TP_INT);
  touchOk = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (touchOk) {
    touch.setCoverScreenCallback(onHomeButton);
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(TP_INT), onTouchInterrupt, FALLING);
  } else {
    Serial.println("CST9217 init failed");
  }

  // keep I2C snappy: 400kHz, and fail fast instead of stalling the render
  // loop for the default 1s when the bus wedges
  Wire.setClock(400000);
  Wire.setTimeOut(20);

  // Pin the LP-IO clock on permanently (unused RTC-capable pin, never touched
  // again). Without a standing user, drivers toggling GPIO pulls free/realloc
  // the IO-mux interrupt; when that realloc arrives over IPC from core 0 it
  // mallocs on the tiny ipc1 stack -> stack canary panic (the 5-10s crashes).
  gpio_pullup_en(GPIO_NUM_21);

  buildJet();
  Serial.printf("Jet mesh: %d triangles\n", numTris);

  lastFrameMs = millis();
  fpsWindowStart = lastFrameMs;
}

void loop() {
  powerButtonTick();
  launcherExitTick();
  const uint32_t now = millis();
  float dt = (now - lastFrameMs) / 1000.0f;
  lastFrameMs = now;
  if (dt > 0.1f) dt = 0.1f;

  static uint32_t i2cUs = 0, i2cMaxUs = 0;
  uint32_t i2cT0;

  // ---- touch: drag rotation + mode button ----
  static uint32_t lastTouchReadMs = 0;
  static int touchFaults = 0;
  // time gate BEFORE consuming the interrupt flag, so a rate-limited event
  // stays pending instead of being dropped
  if (touchOk && now - lastTouchReadMs >= 5 && takeTouchInterrupt()) {
    lastTouchReadMs = now;
    int16_t tx = 0, ty = 0;
    i2cT0 = micros();
    const uint8_t points = touch.getPoint(&tx, &ty, 1);
    const uint32_t d = micros() - i2cT0;
    i2cUs += d;
    if (d > i2cMaxUs) i2cMaxUs = d;
    if (d > 15000) {  // a healthy read is ~1ms; this chip is wedged
      if (++touchFaults >= 3) {
        touchFaults = 0;
        recoverTouch();
      }
    } else {
      touchFaults = 0;
    }
    if (points > 0) {
      ty = LCD_HEIGHT - 1 - ty;  // panel Y axis is inverted vs the display
      lastTouchReportMs = now;
      if (armed) {
        armed = false;  // new press
        Serial.printf("touch: press at (%d,%d) inButton=%d\n", tx, ty, inButton(tx, ty));
        if (inButton(tx, ty)) {
          pendingToggle = true;  // fires on release
        } else if (mode == MODE_DRAG) {
          dragActive = true;
          dragX = tx;
          dragY = ty;
        }
      } else if (dragActive) {
        const float dx = tx - dragX, dy = ty - dragY;
        dragX = tx;
        dragY = ty;
        yaw += dx * 0.010f;
        pitch += dy * 0.010f;
        if (dt > 0) {
          yawVel = dx * 0.010f / dt;
          pitchVel = dy * 0.010f / dt;
        }
      }
    }
  }
  // home-button gesture from the driver (bottom-center tap)
  if (homePressed) {
    homePressed = false;
    Serial.println("touch: home button event");
    toggleMode();
  }
  // release = no touch reports for a quiet period (CST9217 interleaves
  // empty frames while held, so a single empty read is not a release)
  if (!armed && now - lastTouchReportMs > 100) {
    armed = true;
    dragActive = false;
    if (pendingToggle) {
      pendingToggle = false;
      toggleMode();
    }
  }

  // ---- attitude update (IMU polled at 10Hz to keep the I2C bus quiet) ----
  static uint32_t lastImuMs = 0;
  static float rollTarget = 0, pitchTarget = 0;
  static int imuFails = 0;
  if (mode == MODE_TILT && imuOk) {
    if (now - lastImuMs >= 100) {
      lastImuMs = now;
      float ax, ay, az;
      i2cT0 = micros();
      const bool got = qmi.getDataReady() && qmi.getAccelerometer(ax, ay, az);
      const uint32_t d = micros() - i2cT0;
      i2cUs += d;
      if (d > i2cMaxUs) i2cMaxUs = d;
      if (got) {
        imuFails = 0;
        // az reads ~-1g with the screen facing up, hence the sign flip
        rollTarget = atan2f(ax, -az);
        pitchTarget = atan2f(ay, -az);
      } else if (++imuFails >= 10) {
        // bus looks wedged: reinitialise the IMU once per second at most
        Serial.println("IMU stalled - reinitialising");
        imuFails = 0;
        qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
        qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                                SensorQMI8658::ACC_ODR_250Hz,
                                SensorQMI8658::LPF_MODE_0);
        qmi.enableAccelerometer();
        Wire.setClock(400000);
        Wire.setTimeOut(20);
      }
    }
    roll += 0.12f * (rollTarget - roll);
    pitch += 0.12f * (pitchTarget - pitch);
  } else if (mode == MODE_DRAG && !dragActive) {
    // inertia spin-down after release
    yaw += yawVel * dt;
    pitch += pitchVel * dt;
    yawVel *= 0.93f;
    pitchVel *= 0.93f;
    roll += 0.06f * (0 - roll);  // level the wings gently
  }

  buildRotation(yaw, pitch, roll);

  // ---- render ----
  xSemaphoreTake(renderReady, portMAX_DELAY);
  memset(bufs[curBuf], 0, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);

  renderJet();

  char text[40];
  snprintf(text, sizeof(text), "%.0f FPS", fps);
  gfx->setTextSize(3);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(LCD_WIDTH / 2 - 45, 36);
  gfx->print(text);

  // mode button
  gfx->fillCircle(kBtnX, kBtnY, kBtnR, beColor(40, 44, 52));
  gfx->drawCircle(kBtnX, kBtnY, kBtnR, beColor(110, 116, 128));
  gfx->setTextSize(2);
  gfx->setTextColor(RGB565_WHITE);
  const char *label = (mode == MODE_TILT) ? "TILT" : "DRAG";
  gfx->setCursor(kBtnX - 23, kBtnY - 8);
  gfx->print(label);

  xQueueSend(flushQueue, &bufs[curBuf], portMAX_DELAY);
  curBuf ^= 1;
  gfx->setFB(bufs[curBuf]);

  fpsFrames++;
  if (now - fpsWindowStart >= 2000) {
    fps = fpsFrames * 1000.0f / (now - fpsWindowStart);
    Serial.printf("FPS: %.1f | mode=%s pitch=%.0f roll=%.0f yaw=%.0f | i2c avg=%.2fms max=%.2fms\n",
                  fps, mode == MODE_TILT ? "TILT" : "DRAG",
                  pitch * 180 / PI, roll * 180 / PI, yaw * 180 / PI,
                  i2cUs / 1000.0f / fpsFrames, i2cMaxUs / 1000.0f);
    i2cUs = i2cMaxUs = 0;
    fpsFrames = 0;
    fpsWindowStart = now;
  }
}
