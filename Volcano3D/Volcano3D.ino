// 3D erupting volcano demo for Waveshare ESP32-S3-Touch-AMOLED-1.75 (466x466)
// Software 3D: flat-shaded triangle mesh (painter's algorithm) + lava particles.
// FPS shown on screen and logged over serial every 2s.

#include <Arduino.h>
#include "launcher_exit.h"
#include "power_button.h"
#include <Arduino_GFX_Library.h>

#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_RESET 39
#define LCD_WIDTH 466
#define LCD_HEIGHT 466

// ---- types (defined before any function: the Arduino preprocessor puts
// auto-generated prototypes right before the first function definition) ----

struct Vec3 {
  float x, y, z;
};

struct Tri {
  Vec3 a, b, c;
  uint8_t r, g, b_;
  bool emissive;  // crater lava: skip lighting, flicker instead
};

struct Particle {
  Vec3 pos, vel;
  float age, life;
  bool sliding;
};

struct Star {
  int16_t x, y;
  uint8_t bright;
};

struct DrawTri {
  int16_t x0, y0, x1, y1, x2, y2;
  float depth;
  uint16_t color;
};

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *panel = new Arduino_CO5300(
    bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

// Render into a PSRAM framebuffer; flush whole frames (CO5300 needs
// even-aligned writes, and full-frame push avoids flicker).
// Double-buffered: core 1 renders frame N+1 while core 0 DMAs frame N out.
class SwapCanvas : public Arduino_Canvas {
 public:
  using Arduino_Canvas::Arduino_Canvas;
  void setFB(uint16_t *f) { _framebuffer = f; }
};

SwapCanvas *gfx = new SwapCanvas(LCD_WIDTH, LCD_HEIGHT, panel);
uint16_t *bufs[2] = {nullptr, nullptr};
int curBuf = 0;
QueueHandle_t flushQueue;       // frames ready to push
SemaphoreHandle_t renderReady;  // given when the next buffer is free again

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

// ---- scene definition --------------------------------------------------


constexpr int kSegments = 20;
constexpr float kBaseRadius = 1.9f;
constexpr float kRimRadius = 0.5f;
constexpr float kPeakHeight = 1.25f;
constexpr float kCraterDepth = 0.95f;  // y of crater floor
constexpr float kCraterRadius = 0.28f;

// Mesh: outer slope (base ring -> rim ring) and crater bowl (rim -> floor ring)
constexpr int kNumTris = kSegments * 4;


Tri mesh[kNumTris];


constexpr int kNumParticles = 110;
Particle particles[kNumParticles];

constexpr int kNumStars = 60;
Star stars[kNumStars];

// The stock Canvas::flush() byte-swaps every pixel through a bounce buffer
// (~57ms/frame). Instead we keep all colors big-endian in the framebuffer and
// flush with draw16bitBeRGBBitmap, which DMAs the buffer straight out.
uint16_t beColor(uint8_t r, uint8_t g, uint8_t b) {
  const uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  return __builtin_bswap16(c);
}

float frand(float lo, float hi) {
  return lo + (hi - lo) * (float)esp_random() / (float)UINT32_MAX;
}

void spawnParticle(Particle &p, bool initial) {
  const float a = frand(0, 2 * PI);
  const float r = frand(0, kCraterRadius * 0.8f);
  p.pos = {cosf(a) * r, kPeakHeight, sinf(a) * r};
  const float side = frand(0.15f, 0.75f);
  p.vel = {cosf(a) * side, frand(2.2f, 3.6f), sinf(a) * side};
  p.age = initial ? frand(0, 2.5f) : 0;
  p.life = frand(2.0f, 3.5f);
  p.sliding = false;
}

// Height of the outer slope at radius r (linear cone side)
float slopeHeight(float r) {
  if (r >= kBaseRadius) return 0;
  if (r <= kRimRadius) return kPeakHeight;
  return kPeakHeight * (kBaseRadius - r) / (kBaseRadius - kRimRadius);
}

void buildMesh() {
  int t = 0;
  for (int i = 0; i < kSegments; i++) {
    const float a0 = i * 2 * PI / kSegments;
    const float a1 = (i + 1) * 2 * PI / kSegments;
    const Vec3 base0 = {cosf(a0) * kBaseRadius, 0, sinf(a0) * kBaseRadius};
    const Vec3 base1 = {cosf(a1) * kBaseRadius, 0, sinf(a1) * kBaseRadius};
    const Vec3 rim0 = {cosf(a0) * kRimRadius, kPeakHeight, sinf(a0) * kRimRadius};
    const Vec3 rim1 = {cosf(a1) * kRimRadius, kPeakHeight, sinf(a1) * kRimRadius};
    const Vec3 flr0 = {cosf(a0) * kCraterRadius, kCraterDepth, sinf(a0) * kCraterRadius};
    const Vec3 flr1 = {cosf(a1) * kCraterRadius, kCraterDepth, sinf(a1) * kCraterRadius};

    // outer slope quad -> 2 tris, rocky gray-brown
    mesh[t++] = {base0, rim0, base1, 122, 104, 92, false};
    mesh[t++] = {base1, rim0, rim1, 122, 104, 92, false};
    // crater bowl quad -> 2 tris, glowing lava
    mesh[t++] = {rim0, flr0, rim1, 255, 90, 0, true};
    mesh[t++] = {rim1, flr0, flr1, 255, 90, 0, true};
  }
}

// ---- tiny 3D pipeline --------------------------------------------------

float camSin, camCos;
constexpr float kCamPitch = -0.42f;  // look slightly down
const float pitchSin = sinf(kCamPitch), pitchCos = cosf(kCamPitch);
constexpr float kCamDist = 4.3f;
constexpr float kCamLift = 0.62f;  // world y that maps to screen centre
constexpr float kFocal = 315.0f;

// world -> camera space (model spins via camSin/camCos, fixed pitch + dolly)
Vec3 toCamera(const Vec3 &v, bool spin) {
  float x = v.x, z = v.z;
  if (spin) {
    x = v.x * camCos - v.z * camSin;
    z = v.x * camSin + v.z * camCos;
  }
  float y = v.y - kCamLift;
  const float y2 = y * pitchCos - z * pitchSin;
  const float z2 = y * pitchSin + z * pitchCos + kCamDist;
  return {x, y2, z2};
}

bool project(const Vec3 &cam, int16_t &sx, int16_t &sy) {
  if (cam.z < 0.4f) return false;
  sx = LCD_WIDTH / 2 + (int16_t)(kFocal * cam.x / cam.z);
  sy = LCD_HEIGHT / 2 - (int16_t)(kFocal * cam.y / cam.z);
  return true;
}


DrawTri drawList[kNumTris];
int drawCount = 0;

int compareDepth(const void *a, const void *b) {
  const float d = ((const DrawTri *)b)->depth - ((const DrawTri *)a)->depth;
  return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

const Vec3 kLightDir = {0.45f, 0.78f, -0.43f};  // pre-normalised-ish

void renderMountain(uint32_t nowMs) {
  drawCount = 0;
  for (int i = 0; i < kNumTris; i++) {
    const Tri &tri = mesh[i];
    const Vec3 a = toCamera(tri.a, true);
    const Vec3 b = toCamera(tri.b, true);
    const Vec3 c = toCamera(tri.c, true);

    // backface cull in camera space
    const Vec3 e1 = {b.x - a.x, b.y - a.y, b.z - a.z};
    const Vec3 e2 = {c.x - a.x, c.y - a.y, c.z - a.z};
    const Vec3 n = {e1.y * e2.z - e1.z * e2.y,
                    e1.z * e2.x - e1.x * e2.z,
                    e1.x * e2.y - e1.y * e2.x};
    if (n.x * a.x + n.y * a.y + n.z * a.z >= 0) continue;

    DrawTri &d = drawList[drawCount];
    if (!project(a, d.x0, d.y0) || !project(b, d.x1, d.y1) ||
        !project(c, d.x2, d.y2)) {
      continue;
    }
    d.depth = (a.z + b.z + c.z) / 3;

    if (tri.emissive) {
      // lava glow flicker
      const float f = 0.75f + 0.25f * sinf(nowMs * 0.013f + i * 1.7f);
      d.color = beColor((uint8_t)(tri.r * f), (uint8_t)(tri.g * f), 0);
    } else {
      const float len = sqrtf(n.x * n.x + n.y * n.y + n.z * n.z);
      float lambert = 0;
      if (len > 0) {
        lambert = -(n.x * kLightDir.x + n.y * kLightDir.y + n.z * kLightDir.z) / len;
      }
      const float shade = 0.25f + 0.75f * max(0.0f, lambert);
      d.color = beColor((uint8_t)(tri.r * shade), (uint8_t)(tri.g * shade),
                              (uint8_t)(tri.b_ * shade));
    }
    drawCount++;
  }

  qsort(drawList, drawCount, sizeof(DrawTri), compareDepth);
  for (int i = 0; i < drawCount; i++) {
    const DrawTri &d = drawList[i];
    gfx->fillTriangle(d.x0, d.y0, d.x1, d.y1, d.x2, d.y2, d.color);
  }
}

void updateAndRenderParticles(float dt) {
  for (int i = 0; i < kNumParticles; i++) {
    Particle &p = particles[i];
    p.age += dt;
    if (p.age > p.life) {
      spawnParticle(p, false);
    }

    p.vel.y -= 4.5f * dt;  // gravity
    p.pos.x += p.vel.x * dt;
    p.pos.y += p.vel.y * dt;
    p.pos.z += p.vel.z * dt;

    // land on the slope and flow downhill
    const float r = sqrtf(p.pos.x * p.pos.x + p.pos.z * p.pos.z);
    if (r > kCraterRadius && p.pos.y < slopeHeight(r)) {
      p.pos.y = slopeHeight(r);
      if (!p.sliding) {
        p.sliding = true;
        const float inv = r > 0.01f ? 1.0f / r : 0;
        const float speed = frand(0.35f, 0.7f);
        p.vel = {p.pos.x * inv * speed, -0.4f, p.pos.z * inv * speed};
      }
      if (r > kBaseRadius + 0.1f) {
        spawnParticle(p, false);
      }
    }

    // particles do not spin with the mountain (cone is symmetric anyway)
    const Vec3 cam = toCamera(p.pos, false);
    int16_t sx, sy;
    if (!project(cam, sx, sy)) continue;

    // colour cools with age: white-yellow -> orange -> deep red
    const float t = p.age / p.life;
    uint8_t red = 255;
    uint8_t green = t < 0.35f ? 220 - (uint8_t)(t * 340) : (t < 0.75f ? 100 - (uint8_t)((t - 0.35f) * 200) : 20);
    if (t > 0.85f) red = 140;
    const uint16_t color = beColor(red, green, t < 0.15f ? 120 : 0);

    const int16_t size = cam.z < 3.6f ? 4 : (cam.z < 4.6f ? 3 : 2);
    gfx->fillCircle(sx, sy, size, color);
  }
}

// ---- fps / main loop ---------------------------------------------------

uint32_t lastFrameMs = 0;
uint32_t fpsWindowStart = 0;
uint16_t fpsFrames = 0;
float fps = 0;
float angle = 0;

void setup() {
  launcherExitCheck();
  powerButtonInit();
  Serial.begin(115200);

  if (!gfx->begin(80000000)) {  // CO5300 handles 80MHz QSPI
    Serial.println("gfx->begin() failed!");
  }
  bufs[0] = gfx->getFramebuffer();
  bufs[1] = (uint16_t *)heap_caps_malloc(
      (size_t)LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
  if (!bufs[1]) {
    Serial.println("second framebuffer alloc failed!");
  }
  flushQueue = xQueueCreate(1, sizeof(uint16_t *));
  // 2 tokens: rendering may run up to 2 buffers ahead of completed flushes
  renderReady = xSemaphoreCreateCounting(2, 2);
  xTaskCreatePinnedToCore(flushTask, "flush", 4096, nullptr, 2, nullptr, 0);
  panel->setBrightness(230);

  buildMesh();
  for (int i = 0; i < kNumParticles; i++) {
    spawnParticle(particles[i], true);
  }
  // scatter of dim stars, kept away from centre where the volcano sits
  for (int i = 0; i < kNumStars; i++) {
    stars[i].x = (int16_t)frand(10, LCD_WIDTH - 10);
    stars[i].y = (int16_t)frand(10, LCD_HEIGHT - 10);
    stars[i].bright = (uint8_t)frand(70, 180);
  }

  lastFrameMs = millis();
  fpsWindowStart = lastFrameMs;
  Serial.println("Volcano3D ready");
}

void loop() {
  powerButtonTick();
  launcherExitTick();
  const uint32_t now = millis();
  float dt = (now - lastFrameMs) / 1000.0f;
  lastFrameMs = now;
  if (dt > 0.1f) dt = 0.1f;

  angle += 0.55f * dt;
  camSin = sinf(angle);
  camCos = cosf(angle);

  static uint32_t tClear = 0, tScene = 0, tFlush = 0;
  uint32_t t0 = micros();
  xSemaphoreTake(renderReady, portMAX_DELAY);  // wait until this buffer is free
  memset(bufs[curBuf], 0, (size_t)LCD_WIDTH * LCD_HEIGHT * 2);
  uint32_t t1 = micros();

  for (int i = 0; i < kNumStars; i++) {
    const uint8_t b = stars[i].bright;
    gfx->drawPixel(stars[i].x, stars[i].y, beColor(b, b, b));
  }

  renderMountain(now);
  updateAndRenderParticles(dt);

  char text[16];
  snprintf(text, sizeof(text), "%.0f FPS", fps);
  gfx->setTextSize(3);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(LCD_WIDTH / 2 - 45, 36);
  gfx->print(text);
  uint32_t t2 = micros();

  xQueueSend(flushQueue, &bufs[curBuf], portMAX_DELAY);
  curBuf ^= 1;
  gfx->setFB(bufs[curBuf]);
  uint32_t t3 = micros();

  tClear += t1 - t0;
  tScene += t2 - t1;
  tFlush += t3 - t2;

  fpsFrames++;
  if (now - fpsWindowStart >= 2000) {
    fps = fpsFrames * 1000.0f / (now - fpsWindowStart);
    Serial.printf("FPS: %.1f | per-frame ms: clear=%.1f scene=%.1f flush=%.1f\n",
                  fps, tClear / 1000.0f / fpsFrames, tScene / 1000.0f / fpsFrames,
                  tFlush / 1000.0f / fpsFrames);
    tClear = tScene = tFlush = 0;
    fpsFrames = 0;
    fpsWindowStart = now;
  }
}
