/*
 * wave_detector.ino — Boat wave impact meter for the M5StickC PLUS
 *
 * Hardware : M5StickC PLUS (ESP32-PICO-D4, 1.14" 135×240 ST7789v2, MPU6886 IMU)
 * Toolchain: arduino-cli + esp32:esp32 core + M5Unified 0.2.17+
 *
 * What it does:
 *   The IMU accelerometer is sampled continuously. Every 2-second window, the
 *   peak g-force seen above resting (1 g) during that window is recorded as
 *   the "last wave" reading and pushed into a scrolling history graph — a new
 *   bar appears every 2 seconds whether or not the boat actually hit anything.
 *
 * Controls:
 *   BtnA (front "M5") — Clear the graph history and the recorded max wave
 *
 * Alerts:
 *   New biggest wave ever recorded -> screen flashes red/white for 3 seconds
 *
 * ─── STATE RESET ───────────────────────────────────────────────────────────
 *  All state lives in RAM (ordinary global variables). There is NO NVS,
 *  RTC, SPIFFS, or Preferences persistence anywhere in this file. A power
 *  cycle wipes the max wave and history. This is intentional.
 * ────────────────────────────────────────────────────────────────────────────
 *
 * Build:
 *   ACLI=~/Arduino/libraries/bin/arduino-cli
 *   $ACLI compile --fqbn esp32:esp32:m5stack_stickc_plus wave_detector
 *   $ACLI upload  -p /dev/ttyUSB0 --fqbn esp32:esp32:m5stack_stickc_plus wave_detector
 *   # FTDI fallback: append  :UploadSpeed=115200  to the FQBN above
 *   $ACLI monitor -p /dev/ttyUSB0 -c baudrate=115200
 */

#include <M5Unified.h>

// ════════════════════════════════════════════════════════
//  TUNING CONSTANTS
// ════════════════════════════════════════════════════════

// Graph / sampling window. Every WINDOW_MS, the peak g-force seen during that
// window becomes the newest "wave" reading — this drives both the graph
// cadence and what "LAST" means.
constexpr int HISTORY_SIZE = 40;             // bars kept for the scrolling graph
constexpr uint32_t WINDOW_MS = 2000;         // sampling window / redraw cadence
constexpr float GRAPH_MIN_SCALE = 0.5f;      // graph never scales tighter than this (g)

// New-record flash
constexpr uint32_t FLASH_TOTAL_MS = 3000;
constexpr uint32_t FLASH_TOGGLE_MS = 150;

constexpr int BAUD = 115200;

// ════════════════════════════════════════════════════════
//  STATE
// ════════════════════════════════════════════════════════

static M5Canvas   canvas(&M5.Display);
static bool       useSprite = false;
static LovyanGFX* screen;
static int W, H;

float waveHistory[HISTORY_SIZE] = {0};
int   historyCount = 0;   // how many valid entries (until buffer fills)
int   historyHead   = 0;   // index where the next entry will be written

float maxWave    = 0.0f;
float recentWave = 0.0f;

float    windowPeak        = 0.0f;   // running peak within the current window
uint32_t lastWindowMs       = 0;

bool     flashActive      = false;
uint32_t flashStartMs      = 0;

bool     forceRedraw       = true;

// Colours
constexpr uint16_t C_BG      = 0x0000u;
constexpr uint16_t C_TEXT    = 0xFFFFu;
constexpr uint16_t C_MAX     = 0xF800u;   // red
constexpr uint16_t C_LAST    = 0x07FFu;   // cyan
constexpr uint16_t C_AXIS    = 0x4A69u;
constexpr uint16_t C_BAR     = 0x07E0u;   // green
constexpr uint16_t C_BAR_MAX = 0xFFE0u;   // yellow — bar that IS the current max

// ════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════

void finalizeWindow(float peak, uint32_t now);
void clearStats();
void drawStats();
void drawFlashFrame(uint32_t now);

// ════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════

void setup() {
    Serial.begin(BAUD);

    auto cfg = M5.config();
    M5.begin(cfg);

    M5.Display.setRotation(1);   // landscape, 240x135 — easier to read on a moving boat
    W = M5.Display.width();
    H = M5.Display.height();
    M5.Display.fillScreen(C_BG);

    if (canvas.createSprite(W, H)) {
        screen    = &canvas;
        useSprite = true;
        Serial.println("[wave] sprite OK");
    } else {
        screen    = &M5.Display;
        useSprite = false;
        Serial.println("[wave] sprite FAIL — direct draw");
    }

    Serial.printf("[wave] %dx%d  BtnA=clear\n", W, H);
}

// ════════════════════════════════════════════════════════
//  MAIN LOOP
// ════════════════════════════════════════════════════════

void loop() {
    const uint32_t now = millis();
    M5.update();

    // ── Clear ─────────────────────────────────────────
    if (M5.BtnA.wasPressed()) {
        clearStats();
        windowPeak   = 0.0f;
        lastWindowMs = now;
        forceRedraw  = true;
    }

    // ── IMU sample ─────────────────────────────────────
    float ax = 0.0f, ay = 0.0f, az = 1.0f;
    M5.Imu.getAccel(&ax, &ay, &az);
    const float mag   = sqrtf(ax * ax + ay * ay + az * az);
    const float delta = fabsf(mag - 1.0f);
    if (delta > windowPeak) windowPeak = delta;

    // ── Window finalize (drives both the graph and "LAST") ────
    const bool windowElapsed = (now - lastWindowMs >= WINDOW_MS);
    if (windowElapsed) {
        finalizeWindow(windowPeak, now);
        windowPeak   = 0.0f;
        lastWindowMs = now;
    }

    // ── Render ─────────────────────────────────────────
    if (flashActive) {
        if (now - flashStartMs >= FLASH_TOTAL_MS) {
            flashActive = false;
            forceRedraw = true;
        } else {
            drawFlashFrame(now);
        }
    } else if (forceRedraw || windowElapsed) {
        forceRedraw = false;
        drawStats();
    }
}

// ════════════════════════════════════════════════════════
//  WINDOW HANDLING
// ════════════════════════════════════════════════════════

void finalizeWindow(float peak, uint32_t now) {
    recentWave = peak;

    waveHistory[historyHead] = peak;
    historyHead = (historyHead + 1) % HISTORY_SIZE;
    if (historyCount < HISTORY_SIZE) historyCount++;

    Serial.printf("[wave] last=%.2fg\n", peak);

    if (peak > maxWave) {
        maxWave     = peak;
        flashActive = true;
        flashStartMs = now;
        Serial.printf("[wave] NEW MAX=%.2fg\n", maxWave);
    }
}

void clearStats() {
    maxWave     = 0.0f;
    recentWave  = 0.0f;
    historyCount = 0;
    historyHead  = 0;
    for (int i = 0; i < HISTORY_SIZE; i++) waveHistory[i] = 0.0f;
    Serial.println("[wave] cleared");
}

// ════════════════════════════════════════════════════════
//  RENDERING — stats + graph
// ════════════════════════════════════════════════════════

void drawStats() {
    screen->fillScreen(C_BG);

    // ── Header stats ──────────────────────────────────
    screen->setTextSize(2);
    screen->setTextColor(C_MAX, C_BG);
    screen->setCursor(4, 4);
    screen->printf("MAX  %.2fg", maxWave);

    screen->setTextColor(C_LAST, C_BG);
    screen->setCursor(4, 26);
    screen->printf("LAST %.2fg", recentWave);

    // ── Graph area ─────────────────────────────────────
    const int graphX = 4;
    const int graphY = 52;
    const int graphW = W - 2 * graphX;
    const int graphH = H - graphY - 4;

    screen->drawFastHLine(graphX, graphY + graphH, graphW, C_AXIS);

    // Dynamic scale: the tallest bar CURRENTLY VISIBLE reaches the top of the
    // graph area — so detail stays readable even after a big wave scrolls off.
    float visibleMax = 0.0f;
    for (int i = 0; i < historyCount; i++) {
        int srcIdx = (historyHead - historyCount + i + HISTORY_SIZE) % HISTORY_SIZE;
        if (waveHistory[srcIdx] > visibleMax) visibleMax = waveHistory[srcIdx];
    }
    float scale = visibleMax > GRAPH_MIN_SCALE ? visibleMax : GRAPH_MIN_SCALE;

    const int barSlotW = graphW / HISTORY_SIZE;
    const int barW     = barSlotW > 2 ? barSlotW - 2 : 1;

    for (int i = 0; i < historyCount; i++) {
        // oldest-to-newest, left-to-right
        int srcIdx = (historyHead - historyCount + i + HISTORY_SIZE) % HISTORY_SIZE;
        float v = waveHistory[srcIdx];

        int barH = (int)((v / scale) * graphH);
        if (barH < 1) barH = 1;
        if (barH > graphH) barH = graphH;

        int bx = graphX + i * barSlotW;
        int by = graphY + graphH - barH;

        uint16_t col = (v >= maxWave - 0.001f && v > 0.0f) ? C_BAR_MAX : C_BAR;
        screen->fillRect(bx, by, barW, barH, col);
    }

    if (useSprite) canvas.pushSprite(0, 0);
}

// ════════════════════════════════════════════════════════
//  RENDERING — new-record flash (red/white strobe, ~3s)
// ════════════════════════════════════════════════════════

void drawFlashFrame(uint32_t now) {
    const uint32_t elapsed = now - flashStartMs;
    const bool showWhite = ((elapsed / FLASH_TOGGLE_MS) % 2) == 0;
    uint16_t col = showWhite ? TFT_WHITE : TFT_RED;

    screen->fillScreen(col);

    screen->setTextSize(2);
    screen->setTextColor(showWhite ? TFT_RED : TFT_WHITE, col);
    screen->setCursor(4, 4);
    screen->print("NEW BIG WAVE!");
    screen->setCursor(4, 26);
    screen->printf("%.2fg", maxWave);

    if (useSprite) canvas.pushSprite(0, 0);
}
