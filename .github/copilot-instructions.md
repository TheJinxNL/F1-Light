# F1 Light — Project Guidelines

## Platform

- **MCU**: ESP32 (240 MHz, 4 MB flash, QIO), framework: Arduino via PlatformIO.
- **Environment**: `esp32dev` in `platformio.ini`; all sources live under `src/`.
- **Monitor**: 115200 baud, `esp32_exception_decoder` + `colorize` filters.
- **Upload speed**: 921600.
- **Arduino ESP32 core**: 3.3.7; **LED**: FastLED (WS2812B, GRB, 18 LEDs, GPIO 18, MAX_BRIGHTNESS 200); **Display**: Adafruit ST7789 + GFX — HSPI, 170×320 px, `setRotation(0)`, backlight PWM via LEDC.

## Libraries

- `WiFiClientSecure`, `WebSocketsClient` (links2004/arduinoWebSockets), `ArduinoJson`, `WiFiManager` (tzapu).
- LED strip driven by FastLED; display via ST7789 (170×320).

## Architecture

| File | Responsibility |
|------|---------------|
| `config.h` | All tuneable constants — never hardcode values elsewhere |
| `main.cpp` | State machine, LED routing, display routing, idle view alternation |
| `f1_live.cpp` / `f1_live.h` | WiFi, NTP, schedule fetch, SignalR state machine |
| `display.cpp` / `display.h` | ST7789 rendering — no HTTP calls |
| `effects.cpp` / `effects.h` | FastLED animations — pure LED logic |
| `web_ui.cpp` / `web_ui.h` | Async HTTP settings page (brightness, OTA) |
| `f1logo.cpp` / `f1logo.h` | F1 logo bitmap data |

## Hardware Pin Assignment (see config.h)
| Signal        | GPIO |
|---------------|------|
| LED data      |  18  |
| TFT MOSI/SDA  |  23  |
| TFT SCLK/SCL  |  14  |
| TFT CS        |  15  |
| TFT DC        |  27  |
| TFT RST       |   4  |
| TFT BL (PWM)  |  13  |
| WiFi reset btn|   0  |

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

## F1 Live Timing API
- **Always use https://github.com/Nicxe/f1_sensor as the reference implementation** for how to use the F1 live-timing API — it is the most complete and up-to-date source.
- Schedule / session index: `https://livetiming.formula1.com/static/{year}/Index.json`
  - **Only contains rounds F1 has already published** — early in the season (first 4–6 rounds) future races are missing. Use the event-tracker fallback below when `g_upcomingCount == 0`.
  - Response is ~600 KB uncompressed. **Never use `http.getString()`** — it exhausts heap after ~4 meetings. Always stream directly: `deserializeJson(doc, *http.getStreamPtr(), DeserializationOption::Filter(filter))`.
  - BOM handling: peek 3 bytes before streaming; if `0xEF 0xBB 0xBF` discard them, otherwise push them back via a small `PrependStream` wrapper.
  - `Sessions` field can be a JSON **array** OR a JSON **object (dict)**. Always handle both: check `sv.is<JsonArray>()` then `sv.is<JsonObject>()`.
  - Practice sessions have `Name` starting with `"Practice"` — skip them for the display list.
  - Stores all future non-practice sessions across all meetings up to `MAX_UPCOMING_SESSIONS`.
- **Event-tracker fallback** (when Index.json has no upcoming sessions): `https://api.formula1.com/v1/event-tracker`
  - Headers required: `apiKey: lfjBG5SiokAAND3ucpnE9BcPjO74SpUz`, `locale: en`
  - Timetables at `seasonContext.timetables` or `event.timetables`; meeting name at `race.meetingOfficialName` / `race.meetingName` / `event.meetingName`
  - Each timetable entry: `startTime`, `endTime`, `gmtOffset`, `description` (session name)
  - Must use its **own local `WiFiClientSecure`** — different domain from livetiming.formula1.com
  - Retry every hour; once Index.json has data it takes priority automatically
- SignalR negotiate: `https://livetiming.formula1.com/signalr/negotiate`
- SignalR WebSocket: `wss://livetiming.formula1.com/signalr/connect`
- Driver standings (Ergast mirror): `https://api.jolpi.ca/ergast/f1/current/driverstandings.json?limit=20`
  - Returns Ergast MRData format: `MRData.StandingsTable.StandingsLists[0].DriverStandings[].{position, points, Driver.code}`
  - **The domain is `api.jolpi.ca`, NOT `api.jolpica.com`** — the latter does not resolve
- SignalR subscriptions: **`TrackStatus`, `SessionStatus` only** — minimal subscription to keep the "R" seed object small and avoid TLS buffer overflow. Removing extra streams was the fix for the TCP drop at frame 5.
- Track status codes: 1=CLEAR, 2=YELLOW, 4=SC, 5=RED, 6=VSC, 7=VSC_END
- The SignalR negotiate step must call `http.collectHeaders({"Set-Cookie"}, 1)` **before** `GET()` to capture the ARRAffinity cookie — otherwise the WebSocket upgrade returns HTTP 400
- Always send `User-Agent: BestHTTP` and `Accept-Encoding: identity` on all requests to this API

## WebSocket / HTTP Critical Rules
- `static WiFiClientSecure g_tlsClient` is shared for `livetiming.formula1.com` calls only
- Any HTTPS request to a **different domain** (e.g. `api.jolpi.ca`, `api.formula1.com`) **must use its own local `WiFiClientSecure`** — reusing `g_tlsClient` across domains causes HTTP -1 (connection failure)
- Always call `g_tlsClient.stop()` after each blocking HTTPS request that uses the shared client so its ~40 KB SSL heap is freed before the next call
- Never fire two HTTPS requests in the same `loop()` iteration — separate them with a `return` so each runs in its own pass
- **Never use `http.getString()` for large responses** — ESP32 WROOM-32 has ~120 KB max contiguous heap after WiFi+TLS; Index.json is ~600 KB and will truncate silently after ~4 meetings. Always stream: pass `*http.getStreamPtr()` directly to `deserializeJson(doc, stream, DeserializationOption::Filter(filter))`
- **BOM stripping in streaming mode**: `WiFiClient` has no `unread()`. Use a `PrependStream` struct (extends `Stream`) with a 3-byte buffer: peek first 3 bytes, discard if UTF-8 BOM (`0xEF 0xBB 0xBF`), otherwise route them through the prepend buffer before the live stream
- Never set timing globals (`g_lastHeartbeatMs`, `g_connectedAtMs`) inside WStype_CONNECTED callbacks — this causes a 1 ms underflow when compared to `millis()` in the main loop. Set them via an edge-detector in the main loop instead
- Use absolute timestamps (`g_nextPollMs = millis() + interval`) not relative `now - g_lastMs` comparisons to avoid unsigned underflow on boot

## Display Screens (display.cpp)
- `displayShowIdle()` — session schedule: next meeting name + up to 3 upcoming sessions with countdown
- `displayShowCountdown()` / `displayCountdownReset()` — 5-minute pre-session countdown
- `displayShowChampionship()` — F1 STANDINGS header + 6 driver rows with position badge (gold/silver/bronze/gray), TLA, points
- `displayShowLive(TrackStatus)` — **sole live screen** — full-screen flag-colour + status text; used for all session types while LIVE (session-type branching removed)
- `displayShowFinished()` — chequered flag pattern full-screen + "FINISHED" in red; shown once on `SESSION_ENDED` state entry
- RGB565 palette: `COL_BG=0x0000`, `COL_RED=0xF800`, `COL_GREEN=0x07E0`, `COL_YELLOW=0xFFE0`, `COL_ORANGE=0xFD20`, `COL_DGRAY=0x2104`, `COL_LGRAY=0xC618`

## State Machine (main.cpp / f1_live.h)
- States: `WIFI_CONNECTING`, `NTP_SYNC`, `IDLE`, `CONNECTING`, `LIVE`, `RECONNECTING`, `SESSION_ENDED`
- `SESSION_ENDED` — fires when `SessionStatus` delivers "Finished"/"Finalised"/"Aborted". `f1LiveLoop()` advances to IDLE on the very next call (single-pass hold). The `stateChanged` block in `main.cpp` runs `displayShowFinished()` + `effectSessionFinished()` (blocking, ~3 s) before that transition completes.
- LIVE state is simplified: always calls `displayShowLive(status)`, only redraws on track-status change. No per-session-type branching.
- CONNECTING / RECONNECTING: only show the "connecting" screen if `status == TrackStatus::UNKNOWN`; otherwise hold the last known display and LED state (LEDs stay on last `effectTrackStatus()`).
- `g_upcomingCount` is cleared to 0 on all three session-end paths (WStype_TEXT session-end, seed-over, window-close) so `displayShowIdle()` never shows the finished race after returning to IDLE.

## Idle View Alternation (main.cpp)
- In IDLE state the display alternates every 10 s between the session schedule (`s_idleView=0`) and championship standings (`s_idleView=1`)
- Alternation is suppressed during the 5-minute countdown (always shows schedule)
- `f1ChampRefreshed()` always caches the new data; redraws immediately only if currently showing the standings view (`s_idleView == 1`) — otherwise the data is ready for the next view switch
- Championship standings are fetched once on first IDLE entry then every 30 minutes
- `g_scheduleRefreshed` is set unconditionally at the end of `fetchUpcomingSchedule()` (even when `g_upcomingCount == 0`) so the display redraws after a race ends and no future sessions are listed yet

## Development Guidelines
- Suggest Arduino-compatible C++ code only
- Prefer FastLED library patterns for LED effects
- Keep functions small and readable; add comments for timing-sensitive sections
- Define all hardware constants in `config.h`
- All HTTP/HTTPS calls must handle the shared-TLS-client rules above
- Target: ESP32 Arduino core 3.x (`FASTLED_ESP32` defines where needed)

