# F1 Light — Project Guidelines

## Platform

- **MCU**: ESP32 (240 MHz, 4 MB flash, QIO), framework: Arduino via PlatformIO.
- **Environment**: `esp32dev` in `platformio.ini`; all sources live under `src/`.
- **Monitor**: 115200 baud, `esp32_exception_decoder` + `colorize` filters.
- **Upload speed**: 921600.

## Libraries

- `WiFiClientSecure`, `WebSocketsClient` (links2004/arduinoWebSockets), `ArduinoJson`, `WiFiManager` (tzapu).
- LED strip driven by FastLED; display via ST7789 (170×320).

## Architecture

| File | Responsibility |
|------|---------------|
| `config.h` | All tuneable constants — never hardcode values elsewhere |
| `f1_live.cpp` / `f1_live.h` | WiFi, NTP, schedule fetch, SignalR state machine |
| `display.cpp` / `display.h` | ST7789 rendering — no HTTP calls |
| `effects.cpp` / `effects.h` | FastLED animations — pure LED logic |
| `f1logo.cpp` / `f1logo.h` | F1 logo bitmap data |

## Data Sources

- **Session windows**: `https://livetiming.formula1.com/static/{year}/Index.json` — parsed via streaming ArduinoJson filter to avoid OOM on ~600 KB payload.
- **Upcoming schedule fallback**: `https://api.formula1.com/v1/event-tracker`
- **Championship standings**: `https://api.jolpi.ca/ergast/f1/current/driverstandings.json`

## Code Conventions

- C++11; no RTTI, no exceptions.
- All module-private state and helpers are `static` at file scope.
- Prefer `Serial.printf` over chained `Serial.print` calls.
- TLS certificates are not validated (`setInsecure()`) — noted in comments.
- Time is always UTC internally (`setenv("TZ","UTC0",1)`); display TZ applied separately via `DISPLAY_TZ_POSIX`.
- `g_tlsClient.stop()` after every HTTPS request to reclaim ~40 KB TLS heap.
- Event-tracker and standings fetches use their own local `WiFiClientSecure` (different domain).

## Build & Flash

```
pio run -t upload          # build + upload
pio device monitor         # serial monitor (115200)
pio run -t upload && pio device monitor   # combined
```

## OTA

Manifest URL and version are in `config.h`. Boot-time OTA check is controlled by `OTA_BOOT_CHECK_ENABLED`.
