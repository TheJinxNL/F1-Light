# F1 Light — ESP32 LED + TFT Display Controller

A live-connected ESP32 project that tracks the **official F1 Live Timing feed** and reacts in real time:
- An 18-LED WS2812B strip animates the current track status (green / yellow / safety car / VSC / red flag)
- A 1.9″ ST7789 TFT display shows the upcoming race schedule, live track status, and championship standings

Based on the original 3D design of Julian F: https://makerworld.com/en/models/1624039-formula-1-lamp-stand#profileId-1714575
Inspiration on how to access the F1 Live Feeds: https://github.com/Nicxe/f1_sensor

---
![f1light](https://github.com/user-attachments/assets/a0cf1643-0b6c-4ca5-85f3-e863da392c33)

## Features

- **Live track status** via the official F1 SignalR feed — no third-party API key needed
- **LED effects** for every flag condition: Green, Yellow, Safety Car, VSC, VSC Ending, Red Flag
- **Race battery overlay** — during Race sessions, the last 4 LEDs drain over time like an EV battery indicator
- **Session-end celebration** — chequered flag LED sweep + 3× white flash + fade + "FINISHED" screen when a session ends
- **Reconnect resilience** — display and LEDs hold the last known track status during a SignalR reconnect
- **Upcoming screen** — next race name, up to 3 sessions with local times; alternates every 10 s with the standings screen
- **Championship standings screen** — top 6 drivers with position badge, code, and points; fetched from the Ergast mirror
- **5-minute countdown** before each session
- **WiFiManager** — configure WiFi from your phone on first boot; credentials saved to flash
- **WiFi reset button** — hold GPIO 0 (BOOT button) at power-on to wipe credentials and re-configure
- **Dimmable backlight** — reduced brightness at idle, full brightness during a live session
- **Auto-reconnect** with exponential back-off on SignalR disconnect
- **Web settings UI** — browser-accessible settings page (LED brightness, idle battery bar style) with persistent NVS storage; URL shown on display at boot
- **Track status test mode** — trigger any flag condition or the session-finished animation directly from the web UI without waiting for a live session

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
| ESP32   | Display pin | Notes |
|---------|-------------|-------|
| GPIO 23 | SDA / DIN   | MOSI — data |
| GPIO 14 | SCL / CLK   | SPI clock |
| GPIO 15 | CS          | Chip select |
| GPIO 27 | DC / RS     | Data/command |
| GPIO 4  | RES / RST   | Reset — or tie to 3.3 V and set `TFT_RST -1` |
| GPIO 13 | BL          | Backlight PWM |
| 3.3 V   | VCC         | |
| GND     | GND         | |

> All pin numbers can be changed in [config.h](config.h).  
> **Avoid GPIO 2** — can interfere with boot/flash on some boards.  
> **Avoid GPIO 6–11** — connected to internal SPI flash.

### WiFi Reset Button
Hold **GPIO 0** (the built-in BOOT button on most DevKits) while powering on to erase saved WiFi credentials and open the configuration portal. No extra wiring needed.

---

## Project Structure

```
F1_Light/
├── F1_Light.ino                        — Main sketch: setup(), loop(), state/display wiring
├── config.h                            — All hardware pins, timing, and colour constants
├── f1_live.h                           — Public API + shared types (TrackStatus, F1State, SessionInfo, ChampEntry)
├── f1_live.cpp                         — WiFi, NTP, Index.json polling, SignalR WebSocket FSM
├── effects.h                           — LED effect declarations
├── effects.cpp                         — LED effect implementations (non-blocking, millis-based)
├── display.h                           — TFT display declarations
├── display.cpp                         — ST7789 display driver (Adafruit GFX)
├── web_ui.h                            — Web settings server declarations
├── web_ui.cpp                          — HTTP settings server on port 80 (brightness, idle style, test mode)
├── f1logo.h / f1logo.cpp               — F1 logo bitmap (RGB565)
├── Formula1_Display_Regular11pt7b.h    — Custom F1 font (large — session names, lap info)
└── Formula1_Display_Regular7pt7b.h     — Custom F1 font (small — labels, times, standings)
```

---

## Required Libraries

Install all via **Arduino Library Manager** (Sketch → Include Library → Manage Libraries):

| Library | Author |
|---------|--------|
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
Adjust GPIO pins if your wiring differs. Set `DISPLAY_TZ_POSIX` to your local timezone string. Everything else (WiFi credentials) is configured at runtime.

### 4. Upload
Select board **ESP32 Dev Module**, choose your COM port, click **Upload**.

### 5. First-boot WiFi setup
On first boot the display shows a blue **SETUP** screen. Connect your phone/laptop to the Wi-Fi network named **`F1-Light-Setup`**, open a browser, go to **`192.168.4.1`**, and pick your home network. Credentials are saved permanently.

### 6. Web Settings
After connecting to WiFi, the display briefly shows the web settings URL (e.g. `http://192.168.1.42`). Open it in a browser to adjust LED brightness, idle style, or test track status effects.

### 7. Monitor
Open **Serial Monitor** at **115200 baud** to see connection progress, parsed sessions and track status updates.

---

## LED Effects

| State | Effect |
|-------|--------|
| WiFi / NTP connecting | Breathing white |
| Idle (no session) | All LEDs dim red, last LED slowly pulsating |
| Connecting to SignalR | Gentle blue breathing |
| 🟢 Green flag | Solid green |
| 🟡 Yellow flag | Fast yellow blink |
| 🚗 Safety Car | Alternating blue/yellow — grouped 9 / 5 / 4 block pattern |
| 🟠 VSC | Slow orange pulse |
| 🟢 VSC Ending | Pulsating green (ramps between dim and full brightness) |
| 🔴 Red flag | Rapid red flash |
| Race session overlay | Last 4 LEDs show a draining battery bar over race duration |
| 🏁 Session finished | Chequered sweep → 3× white flash → fade (~3 s) |

---

## Display Screens

| State | Screen |
|-------|--------|
| WiFi / NTP | Grey header + connecting text |
| WiFiManager portal | Blue header + AP name + `192.168.4.1` |
| Web settings URL | Red header + "WEB SETTINGS" + `http://<ip>` — shown briefly at boot |
| Idle — schedule | F1 logo header + next race name + up to 3 upcoming sessions with local times |
| Idle — standings | F1 logo header + top 6 driver championship standings |
| 5-min countdown | Session name + large countdown timer |
| Live — Green flag | Full-screen green background + "ALL CLEAR" |
| Live — VSC Ending | Full-screen green background + "VSC ENDING" |
| Live — other flags | Full-screen flag-colour background + large status text |
| Session finished | Chequered flag pattern + "FINISHED" text |

---

## Configuration Reference

All constants live in [config.h](config.h):

| Constant | Default | Description |
| ---------|---------|-------------|
| `LED_PIN` | 18 | GPIO for LED strip data |
| `NUM_LEDS` | 18 | Number of LEDs |
| `MAX_BRIGHTNESS` | 200 | LED brightness cap (0–255); overridden at runtime via web UI |
| `DIM_BRIGHTNESS` | 50 | Reduced LED brightness used for secondary/dim states |
| `IDLE_BASE_RED` | 180 | Red channel level (0–255) for all LEDs in idle state |
| `TFT_BL_DEFAULT` | 200 | Backlight brightness at idle (0–255) |
| `DISPLAY_TZ_POSIX` | `"CET-1CEST,M3.5.0,M10.5.0/3"` | POSIX timezone string used for local display time |
| `WIFI_MANAGER_AP_NAME` | `"F1-Light-Setup"` | Config portal AP name |
| `WIFI_MANAGER_TIMEOUT` | 180 | Portal auto-close timeout (seconds) |
| `WIFI_RESET_PIN` | 0 | GPIO held LOW at boot to reset WiFi credentials |
| `F1_POLL_INTERVAL_MS` | 60000 | How often to poll the F1 schedule (ms) |
| `F1_PRE_WINDOW_MS` | 1 800 000 | Connect to feed this many ms before session starts |
| `F1_POST_WINDOW_MS` | 1 800 000 | Stay connected this many ms after session ends |
| `RACE_BATTERY_DRAIN_MS` | 5 400 000 | Time for the Race battery overlay to drain from full to empty |

---

## Web Settings UI

When connected to WiFi, the device hosts an HTTP server on **port 80**. The URL is shown on the display briefly after boot.

Open `http://<device-ip>/` in a browser to access the settings page:

| Setting | Description |
|---------|-------------|
| **LED Brightness** | Slider (0–255). Applies immediately; persisted across reboots. |
| **Idle Battery Bars** | `0` = last LED pulsating (default); `1–4` = that many manual bars with the top bar pulsing. Persisted. |
| **Track Test Mode** | Toggle on to override live data with a chosen track status for previewing effects. Not persisted. |
| **Track Status** | Dropdown: Clear / Yellow / Safety Car / Red Flag / VSC / VSC End / Session Finished. Active when test mode is on. |

Settings are stored in ESP32 NVS (non-volatile storage) and survive power cycles.

---

## F1 Live Timing API

This project uses the **unofficial but public** F1 Live Timing endpoints:

- **Schedule:** `https://livetiming.formula1.com/static/{year}/Index.json`
- **Event tracker fallback:** `https://api.formula1.com/v1/event-tracker`
- **SignalR negotiate:** `https://livetiming.formula1.com/signalr/negotiate`
- **WebSocket:** `wss://livetiming.formula1.com/signalr/connect`
- **Championship standings:** `https://api.jolpi.ca/ergast/f1/current/driverstandings.json`

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
- Original F1 Light 3D files: https://makerworld.com/en/models/1624039-formula-1-lamp-stand
- F1 Font: https://www.onlinewebfonts.com/download/7a45cffcf1eee0797d566deb425ebaa9
- Convert Fonts to C: https://rop.nl/truetype2gfx/
- RGB565 Converter: https://marlinfw.org/tools/rgb565/converter.html
- Home Assistant F1 Sensor: https://github.com/Nicxe/f1_sensor

---

## License
MIT
