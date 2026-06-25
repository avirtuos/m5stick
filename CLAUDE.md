# M5StickC PLUS — Development Reference

Everything needed to write, build, and deploy Arduino programs to this device.

---

## Hardware Overview

| Item | Detail |
|------|--------|
| **Model** | M5StickC PLUS (v1.1) |
| **MCU** | ESP32-PICO-D4 (dual-core Xtensa LX6 @ 240 MHz) |
| **Flash** | 4 MB (in-package SiP) |
| **SRAM** | 520 KB on-chip; 4 MB PSRAM available via SPIRAM |
| **Display** | 1.14" ST7789v2 TFT, **135×240 pixels**, SPI |
| **IMU** | MPU6886 — 6-axis (3-axis accel + 3-axis gyro), I2C @ 0x68 |
| **PMU** | AXP192 — LiPo charger / power rail management, I2C @ 0x34 |
| **Battery** | 200 mAh LiPo (internal) |
| **Buzzer** | Passive piezo, GPIO2 |
| **IR LED** | Transmit-only, GPIO9 |
| **Red LED** | GPIO10 (active-low) |
| **Microphone** | SPM1423 PDM — CLK GPIO0, DATA GPIO34 |
| **USB-Serial** | FTDI FT232 (this specific unit) — exposes `/dev/ttyUSB0` on Linux |
| **Grove port** | Red HY2.0-4P — I2C + 5V + GND (see pinout) |

---

## Pinout

### Internal connections (managed by M5Unified — do not reconfigure)

| Peripheral | Signal | GPIO |
|------------|--------|------|
| Display (SPI) | MOSI | G15 |
| Display (SPI) | SCLK | G13 |
| Display (SPI) | DC | G23 |
| Display (SPI) | RST | G18 |
| Display (SPI) | CS | G5 |
| Display backlight | — | via AXP192 LDO3 |
| IMU + PMU (I2C) | SDA | G21 |
| IMU + PMU (I2C) | SCL | G22 |
| Button A (front "M5") | active-low | G37 |
| Button B (right side) | active-low | G39 |
| Power button | — | AXP192 PEK |
| Passive buzzer | PWM | G2 |
| IR LED | — | G9 |
| Red status LED | active-low | G10 |
| PDM mic CLK | — | G0 |
| PDM mic DATA | — | G34 |

### Grove port (external, red connector)

| Pin | Signal |
|-----|--------|
| 1 | SDA → **G32** |
| 2 | SCL → **G33** |
| 3 | 5V (from USB or AXP192 EXTEN) |
| 4 | GND |

### Bottom expansion header

Exposes G0, G26, G36/G25 — verify against your unit's silkscreen before use.
G0 is also the PDM clock; G36 is input-only.

---

## Software Stack

### Toolchain

- **arduino-cli** — installed at `~/Arduino/libraries/bin/arduino-cli`
- **Core:** `esp32:esp32` version **3.3.7** (Espressif Arduino Core for ESP32)
- **FQBN:** `esp32:esp32:m5stack_stickc_plus`

### Libraries (already installed at `~/Arduino/libraries/`)

| Library | Version | Purpose |
|---------|---------|---------|
| **M5Unified** | 0.2.17 | Board-agnostic driver: display, IMU, buttons, speaker, power. Auto-detects the variant at runtime. |
| **M5GFX** | 0.2.23 | Graphics backend (used by M5Unified). `M5Canvas` for off-screen sprites. |

Use **M5Unified** for all new sketches — it replaces the older `M5StickCPlus.h` and
works identically on other M5 boards if you ever switch hardware.

---

## Build, Flash & Monitor

```bash
# Convenience aliases (add to ~/.bashrc or paste before use)
ACLI=~/Arduino/libraries/bin/arduino-cli
FQBN=esp32:esp32:m5stack_stickc_plus
PORT=/dev/ttyUSB0

# Compile only (no device needed)
$ACLI compile --fqbn $FQBN <sketch-folder>

# Compile + upload
$ACLI upload -p $PORT --fqbn $FQBN <sketch-folder>

# If upload fails at 1.5 Mbaud (common with this FTDI chip), drop to 115200:
$ACLI upload -p $PORT --fqbn $FQBN:UploadSpeed=115200 <sketch-folder>

# Serial monitor (Ctrl-C to exit)
$ACLI monitor -p $PORT -c baudrate=115200
```

Compile and upload together in one command:

```bash
$ACLI compile --fqbn $FQBN <sketch-folder> && \
$ACLI upload  -p $PORT --fqbn $FQBN <sketch-folder>
```

---

## Key M5Unified API Cheatsheet

```cpp
#include <M5Unified.h>

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);                          // initialise everything

    M5.Display.setRotation(1);             // 0-3; 0 = portrait (135×240 tall)
    M5.Display.fillScreen(BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(0, 0);
    M5.Display.println("Hello!");

    // Off-screen sprite (eliminates flicker)
    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    // draw into canvas…
    canvas.pushSprite(0, 0);               // blit to screen
}

void loop() {
    M5.update();                           // MUST call every frame — updates buttons & IMU

    // Accelerometer (values in g, ±8 g default range)
    float ax, ay, az;
    if (M5.Imu.getAccel(&ax, &ay, &az)) { /* valid reading */ }

    // Gyroscope (degrees/sec)
    float gx, gy, gz;
    M5.Imu.getGyro(&gx, &gy, &gz);

    // Buttons
    if (M5.BtnA.wasPressed())  { /* front "M5" button */ }
    if (M5.BtnB.wasPressed())  { /* side button */ }
    if (M5.BtnA.isPressed())   { /* held */ }

    // Speaker / buzzer
    M5.Speaker.tone(440, 100);             // freq (Hz), duration (ms)
    M5.Speaker.stop();

    // Power / battery
    int pct = M5.Power.getBatteryLevel();  // 0-100
    bool charging = M5.Power.isCharging();

    // Red LED (active-low)
    digitalWrite(10, LOW);                 // on
    digitalWrite(10, HIGH);               // off
}
```

### Display dimensions

Always query at runtime so the sketch adapts to rotation:

```cpp
int W = M5.Display.width();   // 135 in rotation 0/2, 240 in 1/3
int H = M5.Display.height();  // 240 in rotation 0/2, 135 in 1/3
```

### Accelerometer axis orientation (M5StickC PLUS, flat on desk USB-down)

| Axis | ≈+1 g when… |
|------|-------------|
| ax | tilted right |
| ay | tilted forward (USB away from you) |
| az | face up (normal resting) ≈ +1 g |

Invert az to detect upside-down: if `az < -0.5` the device is face-down/inverted.
These signs are printed to serial at startup in `digital_pet` for field verification.

---

## Gotchas

1. **Sketch folder name = `.ino` name.** If the folder is `digital_pet/`, the file
   must be `digital_pet/digital_pet.ino`. arduino-cli will silently refuse to compile
   if they differ.

2. **FTDI upload speed.** This unit uses an FT232 instead of the usual CP2104.
   The default upload baud (1.5 Mbaud) may fail. First attempt at full speed; if you
   see `A fatal error occurred: Failed to connect`, retry with `:UploadSpeed=115200`.

3. **Manual bootloader entry.** If auto-reset doesn't work: hold the side button (BtnB),
   tap the power button, then release BtnB. The device enters download mode and the
   upload proceeds normally.

4. **RAM = volatile.** All variables are wiped on every power cycle. There is no
   persistent state unless you explicitly write to NVS (`Preferences`), SPIFFS, or the
   RTC domain. This is intentional for `digital_pet`.

5. **Serial monitor and upload share the port.** Close the monitor before uploading.

6. **AXP192 power-hold pin.** The board requires GPIO4 be held HIGH to stay on after
   USB power is removed (the AXP192 holds power when the button is released). M5Unified
   handles this automatically inside `M5.begin()`.

7. **Grove I2C vs internal I2C.** Internal peripherals (MPU6886, AXP192) are on G21/G22.
   The Grove port I2C is on G32/G33 — a separate bus. Don't mix them.
