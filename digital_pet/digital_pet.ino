/*
 * digital_pet.ino — A Tamagotchi-style digital pet for the M5StickC PLUS
 *
 * Hardware : M5StickC PLUS (ESP32-PICO-D4, 1.14" 135×240 ST7789v2, MPU6886 IMU)
 * Toolchain: arduino-cli + esp32:esp32 core + M5Unified 0.2.17+
 *
 * Controls:
 *   BtnA (front "M5") — Feed the pet
 *   BtnB (side)       — Clean all poop
 *   No movement 5 min — Screen sleeps; any button or shake wakes it
 *
 * ─── STATE RESET ───────────────────────────────────────────────────────────
 *  All pet state lives in RAM (ordinary global variables). There is NO NVS,
 *  RTC, SPIFFS, or Preferences persistence anywhere in this file. A power
 *  cycle resets millis() to 0, which also resets the alive timer. This is
 *  intentional and expected. Do NOT add persistence calls here.
 * ────────────────────────────────────────────────────────────────────────────
 *
 * Build:
 *   ACLI=~/Arduino/libraries/bin/arduino-cli
 *   $ACLI compile --fqbn esp32:esp32:m5stack_stickc_plus digital_pet
 *   $ACLI upload  -p /dev/ttyUSB0 --fqbn esp32:esp32:m5stack_stickc_plus digital_pet
 *   # FTDI fallback: append  :UploadSpeed=115200  to the FQBN above
 *   $ACLI monitor -p /dev/ttyUSB0 -c baudrate=115200
 */

#include <M5Unified.h>

// ════════════════════════════════════════════════════════
//  TUNING CONSTANTS
// ════════════════════════════════════════════════════════

constexpr int   PET_R           = 22;

// Physics
constexpr float BOUNCE          = 0.88f;
constexpr float FRICTION        = 0.995f;
constexpr float GRAV_SCALE      = 0.09f;
constexpr float BASE_GRAVITY_Y  = 0.04f;
constexpr float WANDER_KICK     = 1.5f;
constexpr float MIN_WANDER_SPD  = 1.2f;
constexpr uint32_t WANDER_MS    = 1500;

// Flip to -1 if the pet falls the wrong direction
constexpr float GRAV_SIGN_X     = -1.0f;
constexpr float GRAV_SIGN_Y     =  1.0f;

// Extra headroom above the face centre for accessories (hat/mohawk)
constexpr int   ACC_HEADROOM    = 22;

// Upside-down
constexpr float UPSIDE_AZ_THRESH = -0.5f;

// Shake / agitation
constexpr float SHAKE_THRESH    = 0.9f;
constexpr float AGIT_DECAY      = 0.80f;
constexpr float ANGRY_THRESH    = 2.5f;
constexpr uint32_t ANGRY_HOLD_MS = 30000;
constexpr int   SHAKE_HZ        = 880;
constexpr int   SHAKE_MS        = 40;

// Hunger
constexpr int   TARGET_FPS      = 40;
constexpr float HUNGER_RATE     = 1.0f / (3600.0f * TARGET_FPS);
constexpr float FEED_AMOUNT     = 0.35f;
constexpr float HUNGRY_THRESH   = 0.65f;
constexpr int   FEED_HZ         = 1047;
constexpr int   FEED_MS         = 80;

// Poop
constexpr int MAX_POOPS         = 12;

// Weather — cycles every 2 minutes: SUNNY → CLOUDY → RAINY → …
constexpr uint32_t WEATHER_MS   = 2UL * 60UL * 1000UL;
constexpr int NUM_RAINDROPS     = 20;
constexpr int RAIN_SPD_Y        = 5;
constexpr int RAIN_SPD_X        = -1;   // slight slant

// Accessories — cycles every 3 minutes: NONE → GLASSES → MOHAWK → HAT → …
constexpr uint32_t ACC_MS       = 3UL * 60UL * 1000UL;

// Power save
constexpr uint32_t IDLE_SLEEP_MS = 5UL * 60UL * 1000UL;
constexpr float    MOVE_THRESH   = 0.12f;
constexpr float    WAKE_THRESH   = 0.35f;

// Frame rate / serial
constexpr int      FRAME_MS      = 1000 / TARGET_FPS;
constexpr int      BAUD          = 115200;
constexpr uint32_t PRINT_INT_MS  = 500;

// ════════════════════════════════════════════════════════
//  PET STATE — all RAM, wiped on every power cycle
// ════════════════════════════════════════════════════════

float    petX, petY;
float    velX = 2.0f;
float    velY = 1.2f;
float    agitation    = 0.0f;
float    hunger       = 0.0f;
uint32_t angryUntilMs = 0;
uint32_t lastMoveMs   = 0;

enum Mood : uint8_t { MOOD_HAPPY, MOOD_SAD, MOOD_ANGRY, MOOD_DIZZY };
Mood currentMood = MOOD_HAPPY;

// Poop
struct Poop { int16_t x, y; };
Poop poops[MAX_POOPS];
int  poopCount  = 0;
bool poop50Done = false, poop100Done = false;

// Weather
enum Weather : uint8_t { WEATHER_SUNNY, WEATHER_CLOUDY, WEATHER_RAINY };
Weather  currentWeather  = WEATHER_SUNNY;
uint32_t lastWeatherMs   = 0;
struct Drop { int16_t x, y; };
Drop raindrops[NUM_RAINDROPS];

// Accessories
enum Accessory : uint8_t { ACC_NONE, ACC_GLASSES, ACC_MOHAWK, ACC_HAT };
Accessory currentAcc  = ACC_NONE;
uint32_t  lastAccMs   = 0;

// ════════════════════════════════════════════════════════
//  DISPLAY / CANVAS
// ════════════════════════════════════════════════════════

static M5Canvas   canvas(&M5.Display);
static bool       useSprite = false;
static LovyanGFX* screen;
static int W, H;

// Colour palette (RGB565)
constexpr uint16_t C_HAPPY    = 0xFFE0u;
constexpr uint16_t C_SAD      = 0x7BCFu;
constexpr uint16_t C_ANGRY    = 0xF800u;
constexpr uint16_t C_DIZZY    = 0x07FFu;
constexpr uint16_t C_BLACK    = 0x0000u;
constexpr uint16_t C_WHITE    = 0xFFFFu;
constexpr uint16_t C_BAR_BG   = 0x2104u;
constexpr uint16_t C_BAR_SEP  = 0x4A69u;
constexpr uint16_t C_POOP     = 0x8A22u;
// Weather
constexpr uint16_t C_SUN      = 0xFFE0u;
constexpr uint16_t C_SKY_CLD  = 0x18C3u;  // dark grey-blue (cloudy sky)
constexpr uint16_t C_SKY_RAIN = 0x0C49u;  // dark blue (rainy sky)
constexpr uint16_t C_CLOUD_LT = 0xCE59u;  // light grey cloud
constexpr uint16_t C_CLOUD_DK = 0x5ADBu;  // dark blue-grey rain cloud
constexpr uint16_t C_RAIN     = 0x5D9Fu;  // rain drop blue
// Accessories
constexpr uint16_t C_GLASS    = 0xFEA0u;  // gold glasses
constexpr uint16_t C_MOHAWK   = 0xF81Fu;  // magenta mohawk
constexpr uint16_t C_MOH_DK   = 0x8010u;  // dark magenta outline
constexpr uint16_t C_HAT      = 0x2965u;  // dark navy hat
constexpr uint16_t C_HAT_BAND = 0xF800u;  // red hat band

constexpr int HUD_H  = 28;
constexpr int SKY_H  = 48;   // height of sky/weather area at top of screen

// ════════════════════════════════════════════════════════
//  FORWARD DECLARATIONS
// ════════════════════════════════════════════════════════
void alertBeep5();
void addPoop();
void enterPowerSave();
void renderCloud(int cx, int cy, uint16_t col);
void renderWeatherBG();
void renderWeatherFG();
void renderPoops();
void renderFace(int cx, int cy, Mood m, bool upsideDown);
void renderAccessory(int cx, int cy, Accessory acc);
void renderHUD(uint32_t aliveMs, Mood m, float h, Weather w, Accessory acc);

// ════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════

void setup() {
    Serial.begin(BAUD);

    auto cfg = M5.config();
    M5.begin(cfg);

    M5.Display.setRotation(0);
    W = M5.Display.width();
    H = M5.Display.height();
    M5.Display.fillScreen(TFT_BLACK);

    if (canvas.createSprite(W, H)) {
        screen    = &canvas;
        useSprite = true;
        Serial.println("[pet] sprite OK");
    } else {
        screen    = &M5.Display;
        useSprite = false;
        Serial.println("[pet] sprite FAIL — direct draw");
    }

    petX = W * 0.5f;
    petY = (H - HUD_H) * 0.7f;

    // Scatter raindrops across the screen from the start
    for (int i = 0; i < NUM_RAINDROPS; i++) {
        raindrops[i].x = (int16_t)random(0, W);
        raindrops[i].y = (int16_t)random(0, H - HUD_H);
    }

    lastMoveMs   = millis();
    lastWeatherMs = millis();
    lastAccMs     = millis();

    Serial.printf("[pet] %d×%d  BtnA=feed  BtnB=clean\n", W, H);
}

// ════════════════════════════════════════════════════════
//  MAIN LOOP
// ════════════════════════════════════════════════════════

void loop() {
    const uint32_t frameStart = millis();

    M5.update();

    // ── IMU ───────────────────────────────────────────
    float ax = 0.0f, ay = 0.0f, az = 1.0f;
    M5.Imu.getAccel(&ax, &ay, &az);
    const float mag    = sqrtf(ax*ax + ay*ay + az*az);
    const float excess = mag - 1.0f;

    if (fabsf(excess) > MOVE_THRESH) lastMoveMs = frameStart;

    // ── Serial telemetry ──────────────────────────────
    static uint32_t lastPrint = 0;
    if (frameStart - lastPrint >= PRINT_INT_MS) {
        lastPrint = frameStart;
        Serial.printf("[imu] ax=%6.3f ay=%6.3f az=%6.3f | agit=%5.2f hunger=%4.2f "
                      "mood=%d poop=%d wx=%d acc=%d alive=%lus\n",
                      ax, ay, az, agitation, hunger,
                      (int)currentMood, poopCount,
                      (int)currentWeather, (int)currentAcc,
                      frameStart / 1000UL);
    }

    // ── Upside-down ───────────────────────────────────
    const bool upsideDown = (az < UPSIDE_AZ_THRESH);

    // ── Shake / agitation ─────────────────────────────
    if (excess > SHAKE_THRESH) {
        agitation += excess;
        if (agitation > ANGRY_THRESH * 0.4f) M5.Speaker.tone(SHAKE_HZ, SHAKE_MS);
    }
    agitation *= AGIT_DECAY;
    if (agitation < 0.01f) agitation = 0.0f;
    if (agitation >= ANGRY_THRESH) {
        angryUntilMs = frameStart + ANGRY_HOLD_MS;
        lastMoveMs   = frameStart;
    }

    // ── Hunger ────────────────────────────────────────
    hunger += HUNGER_RATE;
    if (hunger > 1.0f) hunger = 1.0f;

    if (hunger >= 0.50f && !poop50Done)  { addPoop(); poop50Done  = true; }
    if (hunger >= 1.00f && !poop100Done) { addPoop(); poop100Done = true; }
    if (hunger < 0.50f) poop50Done  = false;
    if (hunger < 0.95f) poop100Done = false;

    // ── BtnA = Feed ───────────────────────────────────
    if (M5.BtnA.wasPressed()) {
        hunger    -= FEED_AMOUNT;
        if (hunger < 0.0f) hunger = 0.0f;
        agitation *= 0.55f;
        lastMoveMs = frameStart;
        M5.Speaker.tone(FEED_HZ, FEED_MS);
    }

    // ── BtnB = Clean poop ─────────────────────────────
    if (M5.BtnB.wasPressed()) {
        if (poopCount > 0) {
            poopCount  = 0;
            lastMoveMs = frameStart;
            M5.Speaker.tone(1319, 60);
        }
    }

    // ── Mood ──────────────────────────────────────────
    if (upsideDown)                                      currentMood = MOOD_DIZZY;
    else if (angryUntilMs && frameStart < angryUntilMs)  currentMood = MOOD_ANGRY;
    else if (hunger >= HUNGRY_THRESH)                    currentMood = MOOD_SAD;
    else                                                 currentMood = MOOD_HAPPY;

    // ── Weather cycle (every 2 minutes) ───────────────
    if (frameStart - lastWeatherMs >= WEATHER_MS) {
        lastWeatherMs  = frameStart;
        currentWeather = (Weather)((currentWeather + 1) % 3);
    }

    // ── Accessory cycle (every 3 minutes) ─────────────
    if (frameStart - lastAccMs >= ACC_MS) {
        lastAccMs  = frameStart;
        currentAcc = (Accessory)((currentAcc + 1) % 4);
    }

    // ── Update raindrops ──────────────────────────────
    if (currentWeather == WEATHER_RAINY) {
        for (int i = 0; i < NUM_RAINDROPS; i++) {
            raindrops[i].y += RAIN_SPD_Y;
            raindrops[i].x += RAIN_SPD_X;
            if (raindrops[i].y >= H - HUD_H || raindrops[i].x < 0) {
                raindrops[i].y = (int16_t)random(0, 10);
                raindrops[i].x = (int16_t)random(0, W);
            }
        }
    }

    // ── Physics ───────────────────────────────────────
    velX += ax * GRAV_SIGN_X * GRAV_SCALE;
    velY += ay * GRAV_SIGN_Y * GRAV_SCALE;
    velY += BASE_GRAVITY_Y;
    velX *= FRICTION;
    velY *= FRICTION;

    static uint32_t lastWander = 0;
    if (frameStart - lastWander >= WANDER_MS) {
        lastWander = frameStart;
        if (sqrtf(velX*velX + velY*velY) < MIN_WANDER_SPD) {
            velX += (random(0, 2) ? 1.0f : -1.0f) * WANDER_KICK;
            velY -= WANDER_KICK * 1.2f;
        }
    }

    petX += velX;
    petY += velY;

    const float minX = (float)PET_R;
    const float maxX = (float)(W - PET_R);
    const float minY = (float)(PET_R + ACC_HEADROOM);  // room for hat/mohawk above head
    const float maxY = (float)(H - HUD_H - PET_R);

    if (petX < minX) { petX = minX; velX =  fabsf(velX) * BOUNCE; }
    if (petX > maxX) { petX = maxX; velX = -fabsf(velX) * BOUNCE; }
    if (petY < minY) { petY = minY; velY =  fabsf(velY) * BOUNCE; }
    if (petY > maxY) { petY = maxY; velY = -fabsf(velY) * BOUNCE; }

    // ── Idle / power save ─────────────────────────────
    if (frameStart - lastMoveMs >= IDLE_SLEEP_MS) enterPowerSave();

    // ── Render ────────────────────────────────────────
    screen->fillScreen(TFT_BLACK);
    renderWeatherBG();             // sky tint + sun / cloud
    renderPoops();
    renderFace((int)petX, (int)petY, currentMood, upsideDown);
    renderAccessory((int)petX, (int)petY, currentAcc);
    renderWeatherFG();             // rain in front of pet
    renderHUD(frameStart, currentMood, hunger, currentWeather, currentAcc);
    if (useSprite) canvas.pushSprite(0, 0);

    const uint32_t elapsed = millis() - frameStart;
    if (elapsed < (uint32_t)FRAME_MS) delay(FRAME_MS - elapsed);
}

// ════════════════════════════════════════════════════════
//  POWER SAVE
// ════════════════════════════════════════════════════════

void enterPowerSave() {
    Serial.println("[pet] idle — screen off");
    const uint8_t savedBright = M5.Display.getBrightness();
    M5.Display.setBrightness(0);
    while (true) {
        delay(150);
        M5.update();
        float ax, ay, az;
        M5.Imu.getAccel(&ax, &ay, &az);
        if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed() ||
            fabsf(sqrtf(ax*ax + ay*ay + az*az) - 1.0f) > WAKE_THRESH) break;
    }
    M5.Display.setBrightness(savedBright);
    lastMoveMs = millis();
    Serial.println("[pet] awake");
}

// ════════════════════════════════════════════════════════
//  ALERT BEEP  (blocks ~1.25 s)
// ════════════════════════════════════════════════════════

void alertBeep5() {
    for (int i = 0; i < 5; i++) {
        M5.Speaker.tone(880, 120);
        delay(250);
    }
}

// ════════════════════════════════════════════════════════
//  POOP
// ════════════════════════════════════════════════════════

void addPoop() {
    alertBeep5();
    if (poopCount >= MAX_POOPS) return;
    poops[poopCount].x = (int16_t)random(14, W - 14);
    poops[poopCount].y = (int16_t)random(SKY_H + 10, H - HUD_H - 18);
    poopCount++;
}

void drawPoop(int px, int py) {
    screen->fillCircle(px, py,      5, C_POOP);
    screen->fillCircle(px, py - 5,  4, C_POOP);
    screen->fillCircle(px, py - 9,  2, C_POOP);
    screen->drawPixel(px + 1, py - 10, C_WHITE);
}

void renderPoops() {
    for (int i = 0; i < poopCount; i++) drawPoop(poops[i].x, poops[i].y);
}

// ════════════════════════════════════════════════════════
//  WEATHER — background (sky + sun/cloud)
// ════════════════════════════════════════════════════════

void renderCloud(int cx, int cy, uint16_t col) {
    screen->fillCircle(cx - 8, cy + 3, 8, col);
    screen->fillCircle(cx,     cy,     9, col);
    screen->fillCircle(cx + 9, cy + 3, 7, col);
    screen->fillCircle(cx - 2, cy - 6, 7, col);
    screen->fillCircle(cx + 5, cy - 5, 6, col);
}

void renderWeatherBG() {
    switch (currentWeather) {

    case WEATHER_SUNNY: {
        // Yellow sun with 8 rays in the top-right corner
        const int sx = W - 18, sy = 18, sr = 9, rl = 7;
        for (int i = 0; i < 8; i++) {
            const float a = i * 0.7854f;
            screen->drawLine(
                sx + (int)((sr + 2) * cosf(a)), sy + (int)((sr + 2) * sinf(a)),
                sx + (int)((sr + rl) * cosf(a)), sy + (int)((sr + rl) * sinf(a)),
                C_SUN);
        }
        screen->fillCircle(sx, sy, sr, C_SUN);
        break;
    }

    case WEATHER_CLOUDY:
        screen->fillRect(0, 0, W, SKY_H, C_SKY_CLD);
        renderCloud(W / 2, 22, C_CLOUD_LT);
        // Second smaller cloud offset to the left
        renderCloud(W / 4, 32, 0xAD55u);
        break;

    case WEATHER_RAINY:
        screen->fillRect(0, 0, W, SKY_H, C_SKY_RAIN);
        renderCloud(W / 2, 20, C_CLOUD_DK);
        break;
    }
}

// ════════════════════════════════════════════════════════
//  WEATHER — foreground (rain falls in front of the pet)
// ════════════════════════════════════════════════════════

void renderWeatherFG() {
    if (currentWeather != WEATHER_RAINY) return;
    for (int i = 0; i < NUM_RAINDROPS; i++) {
        const int rx = raindrops[i].x, ry = raindrops[i].y;
        screen->drawLine(rx, ry, rx + RAIN_SPD_X * 2, ry + 6, C_RAIN);
    }
}

// ════════════════════════════════════════════════════════
//  RENDERING — pet face
// ════════════════════════════════════════════════════════

void renderFace(int cx, int cy, Mood m, bool upsideDown) {
    const int eOffX = PET_R / 3;
    const int eOffY = PET_R / 5;
    const int eRad  = PET_R / 6;
    const int mOffY = PET_R / 4;
    const int mHW   = (PET_R * 6) / 10;

    uint16_t faceCol;
    switch (m) {
        case MOOD_SAD:   faceCol = C_SAD;   break;
        case MOOD_ANGRY: faceCol = C_ANGRY; break;
        case MOOD_DIZZY: faceCol = C_DIZZY; break;
        default:         faceCol = C_HAPPY; break;
    }
    screen->fillCircle(cx, cy, PET_R,     faceCol);
    screen->drawCircle(cx, cy, PET_R + 1, C_WHITE);

    const int lx = cx - eOffX, rx = cx + eOffX, ey = cy - eOffY;

    if (m == MOOD_DIZZY) {
        const int s = eRad + 1;
        screen->drawLine(lx-s, ey-s, lx+s, ey+s, C_BLACK);
        screen->drawLine(lx+s, ey-s, lx-s, ey+s, C_BLACK);
        screen->drawLine(rx-s, ey-s, rx+s, ey+s, C_BLACK);
        screen->drawLine(rx+s, ey-s, rx-s, ey+s, C_BLACK);
    } else if (m == MOOD_ANGRY) {
        screen->fillCircle(lx, ey, eRad, C_BLACK);
        screen->fillCircle(rx, ey, eRad, C_BLACK);
        const int bs = eRad + 3;
        screen->drawLine(lx - bs, ey - bs, lx + bs, ey - 1, C_BLACK);
        screen->drawLine(rx - bs, ey - 1,  rx + bs, ey - bs, C_BLACK);
    } else {
        screen->fillCircle(lx, ey, eRad, C_BLACK);
        screen->fillCircle(rx, ey, eRad, C_BLACK);
        screen->fillCircle(lx + 1, ey - 1, 1, C_WHITE);
        screen->fillCircle(rx + 1, ey - 1, 1, C_WHITE);
    }

    const int my0 = cy + mOffY;
    if (m == MOOD_DIZZY) {
        for (int i = -mHW; i <= mHW; i++) {
            const float t  = (float)i / mHW * 6.2832f;
            screen->drawPixel(cx + i, my0 + (int)(3.0f * sinf(t)),     C_BLACK);
            screen->drawPixel(cx + i, my0 + (int)(3.0f * sinf(t)) + 1, C_BLACK);
        }
    } else {
        const int amp = PET_R / 4;
        for (int i = -mHW; i <= mHW; i++) {
            const float t  = (float)i / mHW;
            const int   my = (m == MOOD_HAPPY)
                           ? my0 + (int)(amp * (1.0f - t * t))
                           : my0 + (int)(amp * t * t);
            screen->drawPixel(cx + i, my,     C_BLACK);
            screen->drawPixel(cx + i, my + 1, C_BLACK);
        }
    }

    if (upsideDown) {
        screen->setTextColor(C_WHITE, TFT_BLACK);
        screen->setTextSize(1);
        screen->setCursor(cx - 28, cy + PET_R + 4);
        screen->print("Upside down!");
    }
}

// ════════════════════════════════════════════════════════
//  RENDERING — accessories (drawn on top of face)
// ════════════════════════════════════════════════════════

void renderAccessory(int cx, int cy, Accessory acc) {
    // Shared geometry (same as renderFace)
    const int eOffX = PET_R / 3;
    const int eOffY = PET_R / 5;
    const int eRad  = PET_R / 6;
    const int lx = cx - eOffX, rx = cx + eOffX, ey = cy - eOffY;

    switch (acc) {

    case ACC_GLASSES: {
        // Gold oval frames around each eye + bridge + arms
        const int fr = eRad + 3;   // frame radius
        // Lens frames (double-thick circle)
        screen->drawCircle(lx, ey, fr,     C_GLASS);
        screen->drawCircle(lx, ey, fr - 1, C_GLASS);
        screen->drawCircle(rx, ey, fr,     C_GLASS);
        screen->drawCircle(rx, ey, fr - 1, C_GLASS);
        // Bridge between lenses
        const int bx1 = lx + fr, bx2 = rx - fr;
        if (bx2 > bx1) screen->drawFastHLine(bx1, ey, bx2 - bx1, C_GLASS);
        // Temple arms (extend beyond lens to the sides)
        screen->drawFastHLine(lx - fr - 7, ey, 7, C_GLASS);
        screen->drawFastHLine(rx + fr,     ey, 7, C_GLASS);
        break;
    }

    case ACC_MOHAWK: {
        // 5 hot-pink spikes along the top of the head, taller in the centre
        const int baseY    = cy - PET_R + 3;
        const int spikeXs[5] = { cx - 12, cx - 6, cx, cx + 6, cx + 12 };
        const int spikeHs[5] = { 10, 16, 21, 16, 10 };
        for (int i = 0; i < 5; i++) {
            const int bx = spikeXs[i], top = baseY - spikeHs[i];
            screen->fillTriangle(bx - 4, baseY, bx + 4, baseY, bx, top, C_MOHAWK);
            screen->drawTriangle(bx - 4, baseY, bx + 4, baseY, bx, top, C_MOH_DK);
        }
        break;
    }

    case ACC_HAT: {
        // Classic top hat: crown + brim + red band
        const int crownW = PET_R + 2;
        const int crownH = 14;
        const int brimW  = PET_R + 12;
        const int brimH  = 4;
        const int brimY  = cy - PET_R - 1;   // sits right on top of the head
        const int crownY = brimY - crownH;

        // Crown
        screen->fillRect(cx - crownW / 2, crownY, crownW, crownH, C_HAT);
        screen->drawRect(cx - crownW / 2, crownY, crownW, crownH, C_BLACK);
        // Band (red stripe just above brim)
        screen->drawFastHLine(cx - crownW / 2, brimY - 2, crownW, C_HAT_BAND);
        screen->drawFastHLine(cx - crownW / 2, brimY - 3, crownW, C_HAT_BAND);
        // Brim
        screen->fillRect(cx - brimW / 2, brimY, brimW, brimH, C_HAT);
        screen->drawRect(cx - brimW / 2, brimY, brimW, brimH, C_BLACK);
        break;
    }

    default: break;
    }
}

// ════════════════════════════════════════════════════════
//  RENDERING — HUD
// ════════════════════════════════════════════════════════

void renderHUD(uint32_t aliveMs, Mood m, float h, Weather w, Accessory acc) {
    const int barY = H - HUD_H;
    screen->fillRect(0, barY, W, HUD_H, C_BAR_BG);
    screen->drawFastHLine(0, barY, W, C_BAR_SEP);

    // Alive timer (millis from boot; resets on power cycle by design)
    {
        const uint32_t s = aliveMs / 1000;
        char buf[10];
        snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", s/3600, (s%3600)/60, s%60);
        screen->setTextColor(TFT_CYAN, C_BAR_BG);
        screen->setTextSize(1);
        screen->setCursor(2, barY + 3);
        screen->print(buf);
    }

    // Mood label
    {
        const char* label; uint16_t col;
        switch (m) {
            case MOOD_SAD:   label = "Hungry"; col = TFT_YELLOW;  break;
            case MOOD_ANGRY: label = "Mad!";   col = TFT_RED;     break;
            case MOOD_DIZZY: label = "Dizzy";  col = TFT_MAGENTA; break;
            default:         label = "Happy";  col = TFT_GREEN;   break;
        }
        screen->setTextColor(col, C_BAR_BG);
        screen->setTextSize(1);
        screen->setCursor(2, barY + 16);
        screen->print(label);
    }

    // Hunger bar
    {
        const int hx = 54, hy = barY + 5, hw = W - hx - 3, hh = 7;
        screen->drawRect(hx - 1, hy - 1, hw + 2, hh + 2, C_BAR_SEP);
        const int fillW = (int)(h * hw);
        if (fillW > 0) {
            screen->fillRect(hx, hy, fillW, hh,
                (h > HUNGRY_THRESH) ? (uint16_t)TFT_RED : (uint16_t)TFT_GREEN);
        }
        screen->setTextColor(C_WHITE, C_BAR_BG);
        screen->setTextSize(1);
        screen->setCursor(hx, barY + 16);
        screen->print("Hunger");
    }
}
