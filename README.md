# M5StickC PLUS Playground

Arduino sketches for the **M5StickC PLUS** (v1.1) — a compact ESP32-based dev kit with a
1.14" colour TFT, 6-axis IMU, two buttons, buzzer, and LiPo battery.

For full hardware specs, pinout, build commands, and API notes see **[CLAUDE.md](CLAUDE.md)**.

---

## Projects

### `digital_pet/`

A Tamagotchi-style digital pet that lives on the screen.

The pet (an animated face) bounces around the 135×240 display driven by the real
accelerometer — tilt the device and gravity pulls it toward the lower edge. Its world
includes changing weather, rotating accessories, hunger, poop, and moods.

**Moods**

| Situation | Pet reaction |
|-----------|-------------|
| Idle / well-fed | Happy face, bounces freely |
| Hungry (>65%, ~40 min) | Sad face |
| Shaken hard | Angry red face + buzzer; stays mad for 30 s |
| Held upside-down | Dizzy `X_X` eyes + "Upside down!" label |

**Poop**

The pet poops at 50% and 100% hunger (5 beeps each time). Poops accumulate on screen up
to 12; clean them with BtnB before they pile up. Hunger takes 1 hour to reach 100% after
a full feed.

**Weather** — changes automatically every 2 minutes

| Weather | Effect |
|---------|--------|
| ☀️ Sunny | Yellow sun with rays (top-right corner) |
| ☁️ Cloudy | Dark sky strip + two grey clouds |
| 🌧️ Rainy | Dark sky + storm cloud + animated rain falling in front of the pet |

**Accessories** — rotates automatically every 3 minutes

| Accessory | Description |
|-----------|-------------|
| (none) | Just the face |
| 👓 Glasses | Gold oval frames with bridge and temple arms |
| 🎸 Mohawk | 5 hot-pink spikes, tallest in the centre |
| 🎩 Top hat | Dark navy crown + red band + wide brim |

**Controls**

| Input | Action |
|-------|--------|
| **BtnA** (front "M5") | Feed the pet |
| **BtnB** (side) | Clean all poop |
| No movement for 5 min | Screen sleeps; any button or shake wakes it |

A `HH:MM:SS` alive timer is shown in the status bar. **All state resets on power
cycle** — there is no persistence to NVS or RTC. This is intentional.

**Build & flash**

```bash
ACLI=~/Arduino/libraries/bin/arduino-cli
FQBN=esp32:esp32:m5stack_stickc_plus
PORT=/dev/ttyUSB0

$ACLI compile --fqbn $FQBN digital_pet
$ACLI upload  -p $PORT --fqbn $FQBN digital_pet

# FTDI fallback (if upload fails at default speed):
$ACLI upload  -p $PORT --fqbn $FQBN:UploadSpeed=115200 digital_pet

# Serial monitor — shows live accel values and pet state:
$ACLI monitor -p $PORT -c baudrate=115200
```

---

## Requirements

- arduino-cli with `esp32:esp32` core (see CLAUDE.md for installed versions)
- M5Unified 0.2.17+ and M5GFX 0.2.23+ (both pre-installed in `~/Arduino/libraries/`)
- Linux: user must be in `dialout` group (`sudo usermod -aG dialout $USER`)
