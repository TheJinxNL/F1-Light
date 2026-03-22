# F1 Light — ESP32 LED + TFT Display Controller

A live-connected ESP32 project that tracks the **official F1 Live Timing feed** and reacts in real time:
- An 18-LED WS2812B strip animates the current track status
- A 1.9″ ST7789 TFT display shows the upcoming race schedule in idle and live status during a session

---

## Features

- **Live track status** via the official F1 SignalR feed — no third-party API needed
- **LED effects** for every flag condition: Green, Yellow, Safety Car, VSC, Red Flag
- **TFT display** shows next race, upcoming sessions with GMT+1 times, and full-screen status during a session
- **WiFiManager** — configure WiFi from your phone on first boot; credentials saved to flash
- **WiFi reset button** — hold GPIO 0 (BOOT button) at power-on to wipe credentials and re-configure
- **Dimmable backlight** — idle at 50 %, live at 90 %
- **Auto-reconnect** with exponential back-off on SignalR disconnect

---

## Hardware

| Component | Detail |
|---|---|
| Microcontroller | ESP32 DevKit (any 30-pin or 38-pin variant) |
| LED strip | WS2812B / NeoPixel — 18 LEDs |
| Display | 1.9″ ST7789 TFT, 170×320 px |
| Power | 5 V / ≥ 2 A (LED strip draws up to 1 A at full brightness) |

---

## Wiring

### LED Strip
| ESP32 | LED Strip |
|---|---|
| GPIO 18 | DATA |
| GND | GND |
| 5 V rail | +5 V |

### TFT Display (ST7789)
| ESP32 | Display pin | Notes                   |
|---------|-----------|-------------------------|
| GPIO 23 | SDA / DIN | MOSI — data             |
| GPIO 14 | SCL / CLK | SPI clock               |
| GPIO 15 | CS        | Chip select             |
| GPIO 27 | DC / RS   | Data/command            |
| GPIO 4  | RES / RST | Reset — or tie to 3.3 V |
| GPIO 13 | BL        | Backlight PWM           |
| 3.3 V   | VCC       |                         |
| GND     | GND       |                         |

> All pin numbers can be changed in [config.h](config.h).  
> **Avoid GPIO 2** — reserved by the ESP32 WiFi PHY.  
> **Avoid GPIO 6–11** — connected to internal SPI flash.

### WiFi Reset Button
Hold **GPIO 0** (the built-in BOOT button on most DevKits) while powering on to erase saved WiFi credentials and open the configuration portal. No extra wiring needed.

---

## Project Structure

```
F1_Light/
├── F1_Light.ino   — Main sketch: setup(), loop(), state/display wiring
├── config.h       — All hardware pins, timing, and colour constants
├── f1_live.h      — Public API + shared types (TrackStatus, F1State, SessionInfo)
├── f1_live.cpp    — WiFi, NTP, Index.json polling, SignalR WebSocket FSM
├── effects.h      — LED effect declarations
├── effects.cpp    — LED effect implementations (non-blocking, millis-based)
├── display.h      — TFT display declarations
└── display.cpp    — ST7789 display driver (Adafruit GFX)
```

---

## Required Libraries

Install all via **Arduino Library Manager** (Sketch → Include Library → Manage Libraries):

| Library | Author |
|---|---|
| FastLED | FastLED |
| WebSockets | Markus Sattler (links2004) |
| ArduinoJson | Benoit Blanchon |
| WiFiManager | tzapu |
| Adafruit ST7735 and ST7789 Library | Adafruit |
| Adafruit GFX Library | Adafruit |

---

## Getting Started

### 1. Install Arduino IDE and ESP32 core
In **File → Preferences → Additional Boards Manager URLs** add:
```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```
Then install **esp32** via **Tools → Board → Boards Manager**. Tested on core **3.3.7**.

### 2. Install libraries
See the table above.

### 3. Configure [config.h](config.h)
Adjust GPIO pins if your wiring differs. Everything else (WiFi, timezone) is set at runtime.

### 4. Upload
Select board **ESP32 Dev Module**, choose your COM port, click **Upload**.

### 5. First-boot WiFi setup
On first boot the display shows a blue **SETUP** screen. Connect your phone/laptop to the Wi-Fi network named **`F1-Light-Setup`**, open a browser, go to **`192.168.4.1`**, and pick your home network. Credentials are saved permanently.

### 6. Monitor
Open **Serial Monitor** at **115200 baud** to see connection progress, parsed sessions and track status updates.

---

## LED Effects

| State | Effect | LEDs |
|---|---|---|
| WiFi / NTP connecting | Breathing white | All |
| Idle (no session) | Solid red at 50 % brightness | All |
| Connecting to SignalR | Gentle blue breathing | All |
| 🟢 Green flag | Solid green | All |
| 🟡 Yellow flag | Fast yellow blink | All |
| 🚗 Safety Car | Alternating blue/yellow chase | All |
| 🟠 VSC | Slow orange pulse | All |
| 🔴 Red flag | Rapid red flash | All |

---

## Display Screens

| State | Screen |
|---|---|
| WiFi / NTP | Grey header "WAIT" + connecting text |
| WiFiManager portal | Blue header "SETUP" + AP name + `192.168.4.1` |
| Idle | Red header "IDLE" + next race name + up to 3 sessions with day, date & GMT+1 time |
| Live | Full-screen coloured background + large status text (CLEAR / YELLOW / SC / VSC / RED FLAG) |

---

## Configuration Reference

All constants live in [config.h](config.h):

| Constant | Default | Description |
|---|---|---|
| `LED_PIN` | 18 | GPIO for LED strip data |
| `NUM_LEDS` | 18 | Number of LEDs |
| `MAX_BRIGHTNESS` | 200 | LED brightness cap (0–255) |
| `TFT_BL_DEFAULT` | 200 | Backlight brightness at boot (0–255) |
| `DISPLAY_TZ_OFFSET_HOURS` | 1 | UTC offset for session times on display |
| `WIFI_MANAGER_AP_NAME` | `"F1-Light-Setup"` | Config portal AP name |
| `WIFI_MANAGER_TIMEOUT` | 180 | Portal auto-close timeout (seconds) |
| `WIFI_RESET_PIN` | 0 | GPIO held LOW at boot to reset WiFi credentials |
| `F1_POLL_INTERVAL_MS` | 60000 | How often to poll the F1 schedule (ms) |
| `F1_PRE_WINDOW_MS` | 1 800 000 | Connect to feed this many ms before session starts |
| `F1_POST_WINDOW_MS` | 1 800 000 | Stay connected this many ms after session ends |

---

## F1 Live Timing API

This project uses the **unofficial but public** F1 Live Timing endpoints — no API key required:

- **Schedule:** `https://livetiming.formula1.com/static/{year}/Index.json`
- **SignalR negotiate:** `https://livetiming.formula1.com/signalr/negotiate`
- **WebSocket:** `wss://livetiming.formula1.com/signalr/connect`

Track status codes received over the feed:

| Code | Meaning |
|---|---|
| 1 | Green (All Clear) |
| 2 | Yellow flag |
| 4 | Safety Car |
| 5 | Red flag |
| 6 | Virtual Safety Car |
| 7 | VSC Ending |

---

## Used Links
F1 Font: https://www.onlinewebfonts.com/download/7a45cffcf1eee0797d566deb425ebaa9
Convert Fonts to C: https://rop.nl/truetype2gfx/
RGB565 Converter: https://marlinfw.org/tools/rgb565/converter.html

## License
MIT
