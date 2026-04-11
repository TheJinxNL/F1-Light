/**
 * f1_live.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 * Connects the ESP32 to the official F1 Live Timing SignalR feed and extracts
 * the TrackStatus stream.
 *
 * Flow
 * ────
 * 1. Connect to WiFi.
 * 2. Sync time via NTP (required to compare against session window timestamps).
 * 3. Every F1_POLL_INTERVAL_MS: fetch
 *      https://livetiming.formula1.com/static/<YEAR>/Index.json
 *    and check whether *now* falls inside any session window
 *    (StartDate - F1_PRE_WINDOW_MS … EndDate + F1_POST_WINDOW_MS).
 *    Schedule/standings data comes from the event-tracker API (sole source);
 *    Index.json is used only to determine if a session window is active.
 * 4. If a window is active:
 *      a. Negotiate a SignalR token via HTTPS GET /signalr/negotiate.
 *      b. Open a WSS connection to wss://livetiming.formula1.com/signalr/connect.
 *      c. Subscribe to "TrackStatus" (and "SessionStatus" for session-end detection).
 *      d. Parse incoming frames and update g_trackStatus.
 * 5. If no window is active (or the session ends), close the WebSocket and
 *    return to IDLE — poll again after F1_POLL_INTERVAL_MS.
 *
 * Libraries required (install via Arduino Library Manager)
 * ──────────────────────────────────────────────────────────
 *   • WiFiClientSecure  (bundled with ESP32 Arduino core)
 *   • WebSockets        by Markus Sattler  (links2004/arduinoWebSockets)
 *   • ArduinoJson       by Benoit Blanchon
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "f1_live.h"
#include "config.h"
#include "display.h"
#include "effects.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>   // tzapu/WiFiManager
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <time.h>
#include <ctype.h>

// ─── Internal constants ───────────────────────────────────────────────────────

static const char* SIGNALR_NEGOTIATE_HOST = "livetiming.formula1.com";
static const char* SIGNALR_NEGOTIATE_PATH = "/signalr/negotiate?clientProtocol=1.5&connectionData=%5B%7B%22name%22%3A%22Streaming%22%7D%5D";
static const char* SIGNALR_CONNECT_HOST   = "livetiming.formula1.com";
static const int   SIGNALR_PORT           = 443;
// WebSocket path is built at runtime once we have the token.

// Subscribe message — only the 2 streams we actually need.
// TrackStatus: drives the LEDs and display.
// SessionStatus: tells us when the session ends so we can return to IDLE.
// Minimal subscription = minimal "R" seed object = no TLS buffer overflow.
static const char* SUBSCRIBE_MSG =
  "{\"H\":\"Streaming\",\"M\":\"Subscribe\","
  "\"A\":[[\"TrackStatus\",\"SessionStatus\"]],\"I\":1}";

// Heartbeat: re-send subscribe every 5 minutes to keep the SignalR group alive.
// Matches the reference (signalr.py) which uses asyncio.sleep(300).
static const uint32_t HEARTBEAT_INTERVAL_MS = 5UL * 60UL * 1000UL;

// Inactivity guard: if no WebSocket frame arrives for 45 s after the seed data
// is received, the server has gone silent — force a reconnect.
// Matches the reference _monitor_heartbeat logic (heartbeat_timeout = 45.0 s).
static const uint32_t INACTIVITY_TIMEOUT_MS = 45000UL;

// Reconnect back-off: 5 s → 10 s → 20 s … cap at 60 s
static const uint32_t BACKOFF_MIN_MS  = 5000UL;
static const uint32_t BACKOFF_MAX_MS  = 60000UL;

// Keep blocking HTTP sections short so loop() latency stays bounded.
static const uint16_t HTTP_TIMEOUT_INDEX_MS     = 7000;
static const uint16_t HTTP_TIMEOUT_NEGOTIATE_MS = 6000;
static const uint16_t HTTP_TIMEOUT_CHAMP_MS     = 7000;
static const uint16_t HTTP_TIMEOUT_EVENT_MS     = 7000;
static const uint16_t HTTP_TIMEOUT_START_MS     = 4000;

// Session-window checks in LIVE require an HTTPS fetch; run less often than IDLE polls.
static const uint32_t LIVE_WINDOW_CHECK_INTERVAL_MS = 5UL * 60UL * 1000UL;


// ─── Module state ─────────────────────────────────────────────────────────────

static F1State      g_state       = F1State::WIFI_CONNECTING;
static TrackStatus  g_trackStatus = TrackStatus::UNKNOWN;

static WebSocketsClient g_ws;
static WiFiClientSecure g_tlsClient;

// Timing
static uint32_t g_lastHeartbeatMs = 0;  // last subscribe heartbeat
static uint32_t g_backoffMs       = BACKOFF_MIN_MS;
static uint32_t g_reconnectAfter  = 0;  // millis() target for next connect attempt

static bool g_wsConnected = false;

// ─── Session schedule (exposed to display module) ────────────────────────────
uint8_t     g_upcomingCount = 0;
SessionInfo g_upcomingSessions[MAX_UPCOMING_SESSIONS];
static bool g_scheduleRefreshed = false;
static bool     g_seedReceived          = false;  // true once the "R" seed frame is parsed
static uint8_t  g_wsFrameCount          = 0;      // frames logged since last connect (debug)
static uint32_t g_connectedAtMs         = 0;      // millis() when WStype_CONNECTED fired
static uint32_t g_lastStreamActivityMs  = 0;      // millis() of last WStype_TEXT frame received
static uint32_t g_nextPollMs            = 0;      // absolute ms for next IDLE/LIVE poll
static uint32_t g_nextLiveWindowPollMs  = 0;      // absolute ms for next LIVE window check
static bool     g_intentionalDisconnect = false;  // set before deliberate g_ws.disconnect()
static time_t   g_windowEndUtc          = 0;      // UTC epoch when active session window expires
static uint8_t  g_emptyConnectCount     = 0;      // consecutive connects with no seed data received
uint8_t         g_champCount            = 0;
ChampEntry      g_champStandings[MAX_CHAMP_ENTRIES];
static bool     g_champRefreshed        = false;
static bool     g_needChampFetch        = true;   // fetch standings on next IDLE pass (startup + session end)
static bool     g_needScheduleFetch     = true;   // fetch event-tracker on next IDLE pass (startup + session end)
static uint32_t g_wifiRetryMs           = 0;      // millis() when to next attempt WiFiManager portal
static String   g_signalrEncodedToken   = "";     // URL-encoded token saved for /start call
static String   g_signalrCookie         = "";     // raw Set-Cookie from negotiate, forwarded to /start
static bool     g_activeSessionIsRace   = false;  // true if current active session name contains "Race"
static time_t   g_activeSessionStartUtc = 0;      // UTC start of currently active session
static bool     g_bootOtaChecked        = false;  // one-shot guard for boot-time OTA check
// ─── Helpers ──────────────────────────────────────────────────────────────────

static int compareVersionText(const char* current, const char* remote) {
  // Compare dotted numeric versions like 1.2.3 (supports leading "v").
  auto readPart = [](const char*& s) -> int {
    while (*s && !isdigit((unsigned char)*s)) s++;
    int v = 0;
    while (isdigit((unsigned char)*s)) {
      v = v * 10 + (*s - '0');
      s++;
    }
    if (*s == '.') s++;
    return v;
  };

  const char* a = current ? current : "0";
  const char* b = remote  ? remote  : "0";
  for (uint8_t i = 0; i < 4; i++) {
    int av = readPart(a);
    int bv = readPart(b);
    if (av < bv) return -1;
    if (av > bv) return 1;
  }
  return 0;
}

static bool otaInstallFirmware(const char* firmwareUrl, const char* md5) {
  if (!firmwareUrl || !firmwareUrl[0]) return false;

  displayShowOtaStatus("New Firmware", "Downloading...");
  Serial.printf("[OTA] Downloading firmware: %s\n", firmwareUrl);
  WiFiClientSecure tls;
#if OTA_ALLOW_INSECURE_TLS
  tls.setInsecure();
#endif

  HTTPClient http;
  if (!http.begin(tls, firmwareUrl)) {
    Serial.println("[OTA] HTTP begin failed");
    return false;
  }

  http.setTimeout(OTA_FIRMWARE_TIMEOUT_MS);
  http.addHeader("Accept-Encoding", "identity");

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[OTA] Firmware HTTP %d\n", code);
    http.end();
    return false;
  }

  int contentLen = http.getSize();
  if (contentLen <= 0) {
    Serial.println("[OTA] Invalid content length");
    http.end();
    return false;
  }

  displayShowOtaStatus("Installing", "Writing flash...");
  if (!Update.begin((size_t)contentLen)) {
    Serial.printf("[OTA] Update begin failed (err=%u)\n", (unsigned)Update.getError());
    http.end();
    return false;
  }

  if (md5 && md5[0]) {
    if (!Update.setMD5(md5)) {
      Serial.println("[OTA] Invalid MD5 format in manifest");
      Update.abort();
      http.end();
      return false;
    }
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  http.end();

  if (written != (size_t)contentLen) {
    Serial.printf("[OTA] Incomplete write (%u/%u bytes)\n",
                  (unsigned)written, (unsigned)contentLen);
    Update.abort();
    return false;
  }

  if (!Update.end()) {
    Serial.printf("[OTA] Update end failed (err=%u)\n", (unsigned)Update.getError());
    return false;
  }

  if (!Update.isFinished()) {
    Serial.println("[OTA] Update not finished");
    return false;
  }

  Serial.println("[OTA] Firmware install complete");
  return true;
}

static void otaCheckAtBoot() {
#if !OTA_BOOT_CHECK_ENABLED
  return;
#else
  if (g_bootOtaChecked) return;
  g_bootOtaChecked = true;

  displayShowOtaStatus("Checking", "Looking for updates...");
  Serial.printf("[OTA] Checking manifest: %s\n", OTA_MANIFEST_URL);
  WiFiClientSecure tls;
#if OTA_ALLOW_INSECURE_TLS
  tls.setInsecure();
#endif

  HTTPClient http;
  if (!http.begin(tls, OTA_MANIFEST_URL)) {
    displayShowOtaStatus("OTA Check", "Manifest request failed");
    Serial.println("[OTA] Manifest HTTP begin failed");
    delay(600);
    return;
  }

  http.setTimeout(OTA_MANIFEST_TIMEOUT_MS);
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Accept", "application/json");

  int code = http.GET();
  if (code != 200) {
    displayShowOtaStatus("OTA Check", "Manifest unavailable");
    Serial.printf("[OTA] Manifest HTTP %d\n", code);
    http.end();
    delay(600);
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    displayShowOtaStatus("OTA Check", "Manifest parse error");
    Serial.printf("[OTA] Manifest JSON error: %s\n", err.c_str());
    delay(600);
    return;
  }

  const char* version = doc["version"] | "";
  const char* url     = doc["firmwareUrl"] | doc["url"] | "";
  const char* md5     = doc["md5"] | "";

  if (!version[0] || !url[0]) {
    displayShowOtaStatus("OTA Check", "Invalid manifest");
    Serial.println("[OTA] Manifest missing version or firmwareUrl");
    delay(600);
    return;
  }

  int cmp = compareVersionText(FW_VERSION, version);
  if (cmp >= 0) {
    displayShowOtaStatus("Up to date", FW_VERSION);
    Serial.printf("[OTA] Up-to-date (local=%s, remote=%s)\n", FW_VERSION, version);
    delay(700);
    return;
  }

  displayShowOtaStatus("Update Found", version);
  Serial.printf("[OTA] New firmware available: %s -> %s\n", FW_VERSION, version);
  if (otaInstallFirmware(url, md5)) {
    displayShowOtaStatus("Update Complete", "Rebooting...");
    Serial.println("[OTA] Rebooting into updated firmware...");
    delay(900);
    ESP.restart();
  } else {
    displayShowOtaStatus("Update Failed", "Continuing boot");
    Serial.println("[OTA] Update failed, continuing normal boot");
    delay(900);
  }
#endif
}

static bool containsRaceWord(const char* s) {
  if (!s) return false;
  for (const char* p = s; *p; ++p) {
    if (strncasecmp(p, "Race", 4) == 0) return true;
  }
  return false;
}

/** Return current UTC epoch seconds (requires NTP sync). */
static time_t utcNow() {
  time_t now = 0;
  time(&now);
  return now;
}

/** Parse an ISO-8601 date string like "2026-03-15T14:00:00Z" or
 *  "2026-03-15T14:00:00" plus a GmtOffset "+02:00" into a UTC epoch. */
static time_t parseIso8601(const char* dateStr, const char* gmtOffset) {
  if (!dateStr || strlen(dateStr) < 19) return 0;

  struct tm t = {};
  // Parse YYYY-MM-DDTHH:MM:SS
  sscanf(dateStr, "%4d-%2d-%2dT%2d:%2d:%2d",
         &t.tm_year, &t.tm_mon, &t.tm_mday,
         &t.tm_hour, &t.tm_min, &t.tm_sec);
  t.tm_year -= 1900;
  t.tm_mon  -= 1;

  // TZ is permanently set to UTC0 by f1LiveBegin(); mktime() already behaves
  // as timegm() — no per-call setenv/tzset needed.
  time_t epoch = mktime(&t);

  // Apply GMT offset if present and string does NOT end with 'Z'
  bool hasZ = (dateStr[strlen(dateStr) - 1] == 'Z');
  if (!hasZ && gmtOffset && strlen(gmtOffset) >= 6) {
    int sign = (gmtOffset[0] == '-') ? -1 : 1;
    int hOff = 0, mOff = 0;
    sscanf(gmtOffset + 1, "%2d:%2d", &hOff, &mOff);
    epoch -= sign * (hOff * 3600 + mOff * 60);  // subtract offset to get UTC
  }
  return epoch;
}

/**
 * Fetch Index.json for the current year and return true if any session
 * window (StartDate - pre … EndDate + post) contains 'now'.
 */
static bool isSessionWindowActive() {
  char url[80];
  struct tm t = {};
  time_t now = utcNow();
  gmtime_r(&now, &t);
  snprintf(url, sizeof(url),
           "https://livetiming.formula1.com/static/%04d/Index.json",
           t.tm_year + 1900);

  Serial.printf("[F1] Polling: %s\n", url);

  HTTPClient http;
  http.begin(g_tlsClient, url);
  http.setTimeout(HTTP_TIMEOUT_INDEX_MS);
  // Explicitly request plain text — prevents the CDN returning gzip/deflate
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Accept", "application/json");
  http.addHeader("User-Agent", "BestHTTP");

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[F1] Index.json HTTP %d\n", code);
    http.end();
    return false;
  }

  // ── Stream directly into ArduinoJson — never buffer the raw response ────────
  // Index.json is ~600 KB uncompressed; getString() exhausts the heap after
  // ~4 meetings. Passing the WiFiClient stream directly lets ArduinoJson read
  // one byte at a time, discarding non-whitelisted fields on-the-fly so peak
  // RAM stays flat at ~4–8 KB regardless of response size.
  WiFiClient* stream = http.getStreamPtr();

  // Strip UTF-8 BOM (0xEF 0xBB 0xBF) emitted by some F1 CDN edge nodes.
  // WiFiClient has no unread(), so we implement a tiny prepend-buffer wrapper
  // that the ArduinoJson stream interface can consume directly.
  struct PrependStream : public Stream {
    uint8_t     buf[3];
    uint8_t     len  = 0;
    uint8_t     idx  = 0;
    WiFiClient* src  = nullptr;
    int  available() override { return (len - idx) + src->available(); }
    int  read()      override { return idx < len ? buf[idx++] : src->read(); }
    int  peek()      override { return idx < len ? buf[idx]   : src->peek(); }
    size_t write(uint8_t) override { return 0; }
  } ps;
  ps.src = stream;

  // Wait briefly for the first bytes to land (TLS handshake already done)
  uint32_t t0 = millis();
  while (stream->available() < 3 && millis() - t0 < 2000) delay(1);

  if (stream->available() >= 3) {
    uint8_t bom[3];
    stream->readBytes(bom, 3);
    if (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
      Serial.println("[F1] BOM stripped");
      // BOM consumed — stream is now positioned at the first '{'; no prepend needed
    } else if (bom[0] == '{') {
      // Normal JSON start — put the 3 peeked bytes back via the prepend buffer
      ps.buf[0] = bom[0]; ps.buf[1] = bom[1]; ps.buf[2] = bom[2];
      ps.len    = 3;
    } else {
      Serial.printf("[F1] Unexpected response start: 0x%02X 0x%02X 0x%02X\n",
                    bom[0], bom[1], bom[2]);
      http.end();
      return false;
    }
  }

  // ── Filter: only keep fields we actually need ───────────────────────────────
  // ArduinoJson discards every other field as bytes arrive —
  // peak heap stays ~4–8 KB even for a 600 KB response.
  JsonDocument filter;
  filter["Meetings"][0]["Name"]                     = true;
  filter["Meetings"][0]["Sessions"][0]["Name"]      = true;
  filter["Meetings"][0]["Sessions"][0]["StartDate"] = true;
  filter["Meetings"][0]["Sessions"][0]["EndDate"]   = true;
  filter["Meetings"][0]["Sessions"][0]["GmtOffset"] = true;

  JsonDocument doc;
  Stream& src = (ps.len > 0) ? static_cast<Stream&>(ps) : static_cast<Stream&>(*stream);
  DeserializationError err = deserializeJson(
      doc, src, DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("[F1] JSON parse error: %s\n", err.c_str());
    return false;
  }

  JsonArray meetings = doc["Meetings"].as<JsonArray>();
  Serial.printf("[F1] Index.json OK — %d meeting(s) parsed\n", meetings.size());

  bool windowActive = false;
  bool selectedMeta = false;

  // Reset active-session metadata and repopulate if a matching window is found.
  g_activeSessionIsRace   = false;
  g_activeSessionStartUtc = 0;

  for (JsonObject meeting : meetings) {
    // Sessions can be a JSON array OR a JSON object (dict keyed by session index).
    // The F1 API has used both formats depending on the season/round, so handle both.
    JsonVariant sv = meeting["Sessions"];
    Serial.printf("[F1]   Meeting: %s  Sessions type: %s\n",
                  meeting["Name"] | "?",
                  sv.is<JsonArray>()  ? "array" :
                  sv.is<JsonObject>() ? "object" : "NONE");

    auto handleSession = [&](JsonObject session) {
      const char* startStr  = session["StartDate"];
      const char* endStr    = session["EndDate"];
      const char* gmtOffset = session["GmtOffset"];

      Serial.printf("[F1]     Session: %-14s  start=%.19s  gmt=%s\n",
                    session["Name"] | "?",
                    startStr  ? startStr  : "(null)",
                    gmtOffset ? gmtOffset : "(null)");

      // Skip practice sessions — only show Qualifying, Sprint, Race on the display
      const char* sName = session["Name"] | "";
      if (strncasecmp(sName, "Practice", 8) == 0) return;

      time_t sessionStart = parseIso8601(startStr,  gmtOffset);
      time_t sessionEnd   = parseIso8601(endStr,    gmtOffset);

      if (sessionStart == 0) {
        Serial.println("[F1]       -> parse FAILED (epoch=0), skipping");
        return;
      }
      if (sessionEnd <= sessionStart) sessionEnd = sessionStart + 7200;

      // Check if current time falls inside this session's connection window
      time_t windowStart = sessionStart - (time_t)(F1_PRE_WINDOW_MS  / 1000UL);
      time_t windowEnd   = sessionEnd   + (time_t)(F1_POST_WINDOW_MS / 1000UL);

      if (now >= windowStart && now <= windowEnd) {
        Serial.printf("[F1] Active window: %s \u2014 %s (ends in %ld min)\n",
                      session["Name"] | "Session",
                      meeting["Name"] | "Meeting",
                      (long)((windowEnd - now) / 60));
        windowActive = true;
        if (windowEnd > g_windowEndUtc) g_windowEndUtc = windowEnd;
        if (!selectedMeta) {
          selectedMeta = true;
          g_activeSessionStartUtc = sessionStart;
          g_activeSessionIsRace   = containsRaceWord(sName);
        }
      } else {
        long secsUntil = (long)(windowStart - now);
        if (secsUntil > 0) {
          Serial.printf("[F1]   %s/%s: window opens in %ld h %02ld m\n",
                        meeting["Name"] | "?",
                        session["Name"] | "?",
                        secsUntil / 3600, (secsUntil % 3600) / 60);
        }
      }
    };
    if (sv.is<JsonArray>()) {
      for (JsonObject s : sv.as<JsonArray>()) handleSession(s);
    } else if (sv.is<JsonObject>()) {
      for (JsonPair kv : sv.as<JsonObject>()) {
        if (kv.value().is<JsonObject>()) handleSession(kv.value().as<JsonObject>());
      }
    } else {
      Serial.printf("[F1] WARNING: Sessions field is neither array nor object for %s\n",
                    meeting["Name"] | "?");
    }
  }

  if (!windowActive) Serial.println("[F1] No active session window — idle");

  // Release the TLS session so its ~40 KB heap is available for the next fetch
  g_tlsClient.stop();
  return windowActive;
}

/**
 * Negotiate a SignalR connection token.
 * Returns token string on success, empty string on failure.
 */
static String negotiateToken(String& outCookie) {
  g_tlsClient.setInsecure();
  HTTPClient http;
  String url = String("https://") + SIGNALR_NEGOTIATE_HOST + SIGNALR_NEGOTIATE_PATH;
  http.begin(g_tlsClient, url);
  http.setTimeout(HTTP_TIMEOUT_NEGOTIATE_MS);
  // No extra request headers on negotiate — matches the reference (signalr.py),
  // which calls session.get() with no explicit headers.  User-Agent / Accept-Encoding
  // are only set on the WebSocket connect step.

  // Collect the first Set-Cookie response header — exactly as the reference does:
  //   cookie = resp.headers.get("Set-Cookie")
  // The raw value (including "; Path=/; MaxAge=900; SameSite=None; Secure") is
  // forwarded verbatim as the Cookie header on the WebSocket upgrade.
  const char* wantedHeaders[] = { "Set-Cookie" };
  http.collectHeaders(wantedHeaders, 1);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[F1] Negotiate HTTP %d\n", code);
    http.end();
    return "";
  }

  // Take the raw Set-Cookie value exactly as received — no attribute stripping.
  outCookie = http.header("Set-Cookie");
  Serial.printf("[F1] Negotiate Set-Cookie: %.100s%s\n",
                outCookie.c_str(), outCookie.length() > 100 ? "..." : "(end)");

  String payload = http.getString();
  http.end();
  g_tlsClient.stop();  // free ~40 KB SSL heap before the WebSocket TLS session opens

  // Strip UTF-8 BOM if present
  if (payload.length() >= 3 &&
      (uint8_t)payload[0] == 0xEF &&
      (uint8_t)payload[1] == 0xBB &&
      (uint8_t)payload[2] == 0xBF) {
    payload.remove(0, 3);
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("[F1] Negotiate JSON error: %s\n", err.c_str());
    Serial.printf("[F1] Negotiate raw (first 120): %.120s\n", payload.c_str());
    return "";
  }

  String token = doc["ConnectionToken"] | "";
  if (token.isEmpty()) {
    Serial.printf("[F1] Negotiate: no token — raw (first 120): %.120s\n", payload.c_str());
  }
  return token;
}

// ─── Championship standings fetch ──────────────────────────────────────────────────────

static void fetchChampStandings() {
  // api.jolpi.ca is the correct Ergast mirror — separate domain needs its own TLS client
  Serial.println("[F1] Fetching championship standings...");
  WiFiClientSecure tlsChamp;
  tlsChamp.setInsecure();
  HTTPClient http;
  http.begin(tlsChamp,
             "https://api.jolpi.ca/ergast/f1/current/driverstandings.json?limit=20");
  http.setTimeout(HTTP_TIMEOUT_CHAMP_MS);
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Accept", "application/json");

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[F1] Standings HTTP %d\n", code);
    http.end();
    return;
  }

  // Filter: keep only the fields we need — cuts RAM use significantly
  JsonDocument filter;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["position"]      = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["points"]         = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["code"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(),
                                             DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    Serial.printf("[F1] Standings JSON error: %s\n", err.c_str());
    return;
  }

  JsonArray list = doc["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"]
                       .as<JsonArray>();
  if (list.isNull() || list.size() == 0) {
    Serial.println("[F1] Standings: empty list");
    return;
  }

  g_champCount = 0;
  for (JsonObject entry : list) {
    if (g_champCount >= MAX_CHAMP_ENTRIES) break;
    ChampEntry& ce      = g_champStandings[g_champCount++];
    ce.position         = (uint8_t)atoi(entry["position"] | "0");
    ce.points           = (uint16_t)(atof(entry["points"]  | "0") + 0.5f);
    const char* drCode  = entry["Driver"]["code"] | "???";
    strncpy(ce.code, drCode, sizeof(ce.code) - 1);
    ce.code[sizeof(ce.code) - 1] = '\0';
  }
  Serial.printf("[F1] Standings loaded: %u drivers\n", g_champCount);
  g_champRefreshed = true;
}
/**
 * Fallback schedule source: api.formula1.com/v1/event-tracker.
 * Called when Index.json yields 0 upcoming sessions (e.g. early in the season
 * before F1 has published future rounds to the static Index.json).
 * Populates g_upcomingSessions with the sessions of the current/next event.
 */
static void fetchEventTracker() {
  Serial.println("[F1] Trying event-tracker fallback...");
  // api.formula1.com is a different domain — must use its own TLS client
  WiFiClientSecure tlsET;
  tlsET.setInsecure();
  HTTPClient http;
  http.begin(tlsET, "https://api.formula1.com/v1/event-tracker");
  http.setTimeout(HTTP_TIMEOUT_EVENT_MS);
  http.addHeader("apiKey",          "lfjBG5SiokAAND3ucpnE9BcPjO74SpUz");
  http.addHeader("locale",          "en");
  http.addHeader("Accept",          "application/json");
  http.addHeader("Accept-Encoding", "identity");

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[F1] Event-tracker HTTP %d\n", code);
    http.end();
    return;
  }

  // Filter to the fields we need across both possible timetable locations
  JsonDocument filter;
  filter["race"]["meetingOfficialName"]                              = true;
  filter["race"]["meetingName"]                                      = true;
  filter["event"]["meetingOfficialName"]                             = true;
  filter["event"]["meetingName"]                                     = true;
  filter["seasonContext"]["timetables"][0]["startTime"]              = true;
  filter["seasonContext"]["timetables"][0]["endTime"]                = true;
  filter["seasonContext"]["timetables"][0]["gmtOffset"]              = true;
  filter["seasonContext"]["timetables"][0]["description"]            = true;
  filter["seasonContext"]["timetables"][0]["shortName"]              = true;
  filter["event"]["timetables"][0]["startTime"]                      = true;
  filter["event"]["timetables"][0]["endTime"]                        = true;
  filter["event"]["timetables"][0]["gmtOffset"]                      = true;
  filter["event"]["timetables"][0]["description"]                    = true;
  filter["event"]["timetables"][0]["shortName"]                      = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    Serial.printf("[F1] Event-tracker JSON error: %s\n", err.c_str());
    return;
  }

  // Extract meeting name — try several known locations
  const char* meetingName = nullptr;
  for (auto candidate : {
        doc["race"]["meetingOfficialName"].as<const char*>(),
        doc["race"]["meetingName"].as<const char*>(),
        doc["event"]["meetingOfficialName"].as<const char*>(),
        doc["event"]["meetingName"].as<const char*>(),
      }) {
    if (candidate && strlen(candidate) > 0) { meetingName = candidate; break; }
  }
  if (!meetingName) meetingName = "F1 Grand Prix";

  // Try timetables from seasonContext first, then event
  JsonVariant ttv = doc["seasonContext"]["timetables"];
  if (!ttv.is<JsonArray>() || ttv.as<JsonArray>().size() == 0)
    ttv = doc["event"]["timetables"];

  if (!ttv.is<JsonArray>()) {
    Serial.println("[F1] Event-tracker: no timetables found");
    return;
  }

  time_t now = utcNow();
  // Reset and repopulate upcoming sessions from the timetable
  g_upcomingCount = 0;
  for (JsonObject item : ttv.as<JsonArray>()) {
    // Session name: try description → shortName
    const char* sName = item["description"] | item["shortName"] | "Session";

    // Skip practice sessions
    if (strncasecmp(sName, "Practice", 8) == 0 ||
        strncasecmp(sName, "Free",     4) == 0) continue;

    const char* startStr  = item["startTime"];
    const char* endStr    = item["endTime"];
    const char* gmtOffset = item["gmtOffset"];

    time_t sessionStart = parseIso8601(startStr, gmtOffset);
    time_t sessionEnd   = parseIso8601(endStr,   gmtOffset);

    if (sessionStart == 0) continue;
    if (sessionEnd <= sessionStart) sessionEnd = sessionStart + 7200;
    if (sessionEnd <= now)          continue;  // already finished
    if (g_upcomingCount >= MAX_UPCOMING_SESSIONS) break;

    SessionInfo& si = g_upcomingSessions[g_upcomingCount++];
    strncpy(si.meetingName, meetingName, sizeof(si.meetingName) - 1);
    si.meetingName[sizeof(si.meetingName) - 1] = '\0';
    strncpy(si.sessionName, sName, sizeof(si.sessionName) - 1);
    si.sessionName[sizeof(si.sessionName) - 1] = '\0';
    si.startUtc = sessionStart;

    Serial.printf("[F1]   ET session: %-20s  start=%ld\n", sName, (long)sessionStart);
  }

  Serial.printf("[F1] Event-tracker: %u session(s) stored\n", g_upcomingCount);
  g_scheduleRefreshed = true;  // always redraw — covers the "0 sessions" (race just finished) case too
}

// ─── WebSocket event handler ──────────────────────────────────────────────────

static inline bool sessionIsOver(const char* ss) {
  return strcmp(ss, "Finished")  == 0 ||
         strcmp(ss, "Finalised") == 0 ||
         strcmp(ss, "Ends")      == 0 ||
         strcmp(ss, "Aborted")   == 0 ||
         strcmp(ss, "Inactive")  == 0;
}

static void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      g_wsConnected           = true;
      g_backoffMs             = BACKOFF_MIN_MS;  // reset back-off on success
      g_intentionalDisconnect = false;
      // g_connectedAtMs and g_lastHeartbeatMs are intentionally NOT set here.
      // They are set to 'now' (captured before g_ws.loop()) in the edge-detector
      // inside f1LiveLoop(), which avoids a uint32_t underflow when millis()
      // inside this callback is even 1 ms ahead of 'now'.
      Serial.println("[F1] WebSocket connected");
      g_wsFrameCount = 0;  // reset per-connection frame log
      g_seedReceived = false;
      // Subscribe is sent from f1LiveLoop()'s edge-detector immediately after
      // g_ws.loop() returns rather than here.  Calling sendTXT() inside
      // g_ws.loop() (which is what firing from a callback means) risks hitting
      // internal state that isn't yet ready for writes, causing a silent failure
      // — the server never receives Subscribe, waits briefly, then closes with
      // "Connection lost" (code 17263).  The edge-detector send is equivalent
      // in timing (same loop() call) but guaranteed to run outside the library.
      break;

    case WStype_DISCONNECTED: {
      g_wsConnected = false;
      // A proper WS close frame has a 2-byte big-endian close code followed by
      // an optional UTF-8 reason string.  If length < 2 it's a TCP-level drop.
      if (payload && length >= 2) {
        uint16_t code = (uint16_t)((payload[0] << 8) | payload[1]);
        if (length > 2)
          Serial.printf("[F1] WebSocket closed (code %u: %.*s) intentional=%d\n", code,
                        (int)min(length - 2, (size_t)60), (const char*)payload + 2,
                        (int)g_intentionalDisconnect);
        else
          Serial.printf("[F1] WebSocket closed (code %u) intentional=%d\n",
                        code, (int)g_intentionalDisconnect);
      } else {
        Serial.printf("[F1] WebSocket disconnected (TCP drop, len=%u) intentional=%d\n",
                      (unsigned)length, (int)g_intentionalDisconnect);
      }

      if (g_intentionalDisconnect) {
        // We initiated this close — state was already set by the caller; don't override.
        g_intentionalDisconnect = false;
      } else {
        // Unexpected drop — schedule a reconnect with exponential back-off.
        if (!g_seedReceived) {
          g_emptyConnectCount++;
          Serial.printf("[F1] Dropped before seed data (%u consecutive)\n",
                        g_emptyConnectCount);
        }
        g_state          = F1State::RECONNECTING;
        g_reconnectAfter = millis() + g_backoffMs;
        g_backoffMs      = min(g_backoffMs * 2u, BACKOFF_MAX_MS);
      }
      break;
    }

    case WStype_TEXT: {
      // Track last activity — used by the 45-second inactivity guard.
      g_lastStreamActivityMs = millis();

      // Log first few frames verbatim (truncated) to diagnose server responses.
      if (g_wsFrameCount < 5) {
        g_wsFrameCount++;
        Serial.printf("[F1] WS frame #%u (%u bytes): %.120s\n",
                      g_wsFrameCount, (unsigned)length,
                      payload ? (const char*)payload : "(null)");
      }

      // ── Parse SignalR hub envelope ─────────────────────────────────────────
      // Frames arrive as: {"M":[{"M":"feed","A":["TrackStatus",{...}]}],...}
      static JsonDocument doc;
      doc.clear();
      DeserializationError err = deserializeJson(doc, payload, length);
      if (err) break;

      // Hub messages under "M" array
      JsonArray msgs = doc["M"].as<JsonArray>();
      for (JsonObject msg : msgs) {
        if (strcmp(msg["M"] | "", "feed") != 0) continue;

        JsonArray args = msg["A"].as<JsonArray>();
        if (args.size() < 2) continue;

        const char* stream = args[0];
        JsonObject  data   = args[1].as<JsonObject>();

        if (strcmp(stream, "TrackStatus") == 0) {
          // {"Status":"1","Message":"AllClear"}  (Status is a numeric string)
          int statusCode = atoi(data["Status"] | "0");
          TrackStatus prev = g_trackStatus;

          switch (statusCode) {
            case 1: g_trackStatus = TrackStatus::CLEAR;   break;
            case 2: g_trackStatus = TrackStatus::YELLOW;  break;
            case 4: g_trackStatus = TrackStatus::SC;      break;
            case 5: g_trackStatus = TrackStatus::RED;     break;
            case 6: g_trackStatus = TrackStatus::VSC;     break;
            case 7: g_trackStatus = TrackStatus::VSC_END; break;
            default: break;
          }

          if (g_trackStatus != prev) {
            Serial.printf("[F1] Track status → %s (code %d)\n",
                          trackStatusName(g_trackStatus), statusCode);
          }
        }
        else if (strcmp(stream, "SessionStatus") == 0) {
          // {"Status":"Finished"} / "Aborted" / "Ends" → leave the feed
          const char* ss = data["Status"] | "";
          if (sessionIsOver(ss)) {
            Serial.printf("[F1] Session ended (%s) — disconnecting\n", ss);
            g_intentionalDisconnect = true;  // prevent WStype_DISCONNECTED overriding state
            g_ws.disconnect();
            g_wsConnected   = false;
            g_trackStatus   = TrackStatus::UNKNOWN;
            g_upcomingCount = 0;  // clear stale schedule immediately so IDLE shows "No upcoming sessions"
            g_state         = F1State::SESSION_ENDED;
            g_nextPollMs    = millis() + F1_POST_WINDOW_MS;
            g_needScheduleFetch = true;
            g_needChampFetch    = true;
            g_activeSessionIsRace   = false;
            g_activeSessionStartUtc = 0;
          }
        }
      }

      // Initial subscription result arrives under "R" key
      JsonObject rResult = doc["R"].as<JsonObject>();
      if (!rResult.isNull()) {
        // Check seed SessionStatus first — if the session is already over,
        // the feed returns stale track status (e.g. last yellow of qualifying).
        // Disconnect immediately and stay in IDLE rather than showing stale data.
        const char* seedSS = rResult["SessionStatus"]["Status"] | "";
        Serial.printf("[F1] Seed session status: \"%s\"\n", seedSS);
        if (sessionIsOver(seedSS)) {
          Serial.println("[F1] Seed shows session already over — disconnecting");
          g_intentionalDisconnect = true;
          g_ws.disconnect();
          g_wsConnected   = false;
          g_trackStatus   = TrackStatus::UNKNOWN;
          g_upcomingCount = 0;  // clear stale schedule immediately
          g_state         = F1State::IDLE;
          g_nextPollMs    = millis() + F1_POST_WINDOW_MS;
          g_needScheduleFetch = true;
          g_needChampFetch    = true;
          g_activeSessionIsRace   = false;
          g_activeSessionStartUtc = 0;
          break;
        }

        // Session is live — apply seed TrackStatus
        JsonObject ts = rResult["TrackStatus"].as<JsonObject>();
        if (!ts.isNull()) {
          int statusCode = atoi(ts["Status"] | "0");
          switch (statusCode) {
            case 1: g_trackStatus = TrackStatus::CLEAR;   break;
            case 2: g_trackStatus = TrackStatus::YELLOW;  break;
            case 4: g_trackStatus = TrackStatus::SC;      break;
            case 5: g_trackStatus = TrackStatus::RED;     break;
            case 6: g_trackStatus = TrackStatus::VSC;     break;
            case 7: g_trackStatus = TrackStatus::VSC_END; break;
            default: break;
          }
          Serial.printf("[F1] Seed track status: %s\n", trackStatusName(g_trackStatus));
        }

        g_seedReceived      = true;
        g_emptyConnectCount = 0;  // connection was good — reset consecutive-failure counter
        Serial.println("[F1] Seed data applied");
      }
      break;
    }

    case WStype_ERROR:
      Serial.printf("[F1] WebSocket error: %.*s\n",
                    (int)min(length, (size_t)120),
                    payload ? (const char*)payload : "(null)");
      break;

    case WStype_PING:
      Serial.println("[F1] WS ping received");
      break;

    case WStype_PONG:
      Serial.println("[F1] WS pong received");
      break;

    default:
      break;
  }
}

// ─── Connect to SignalR ───────────────────────────────────────────────────────

/**
 * Call GET /signalr/start to complete the SignalR 1.x handshake.
 * SignalR 1.x spec: after the WebSocket transport is established the client
 * must call /start so the server transitions from "connecting" to "connected".
 * Without this call the server sends {"S":1} and then closes the socket with
 * "Connection lost" (code 17263) after a brief timeout.
 * g_tlsClient is idle during the LIVE state (WebSocket uses its own internal
 * TLS connection), so we reuse it here to avoid a second 40 KB TLS heap.
 */
static void callSignalRStart() {
  if (g_signalrEncodedToken.isEmpty()) return;

  String url = String("https://") + SIGNALR_CONNECT_HOST + "/signalr/start"
               + "?transport=webSockets&clientProtocol=1.5"
               + "&connectionToken=" + g_signalrEncodedToken
               + "&connectionData=%5B%7B%22name%22%3A%22Streaming%22%7D%5D";

  g_tlsClient.setInsecure();
  HTTPClient http;
  http.begin(g_tlsClient, url);
  http.setTimeout(HTTP_TIMEOUT_START_MS);
  http.addHeader("User-Agent", "BestHTTP");
  if (g_signalrCookie.length() > 0)
    http.addHeader("Cookie", g_signalrCookie);

  int code = http.GET();
  if (code == 200) {
    String resp = http.getString();
    Serial.printf("[F1] /start OK: %s\n", resp.c_str());
  } else {
    Serial.printf("[F1] /start HTTP %d\n", code);
  }
  http.end();
  g_tlsClient.stop();  // free ~40 KB SSL heap before next use
}

static bool connectSignalR() {
  String cookie;
  String token = negotiateToken(cookie);
  if (token.isEmpty()) return false;

  // URL-encode the token (spaces → %20, + → %2B, etc.)
  String encodedToken = "";
  encodedToken.reserve(token.length() * 3);
  for (char c : token) {
    if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encodedToken += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
      encodedToken += buf;
    }
  }

  String wsPath = String("/signalr/connect?transport=webSockets&clientProtocol=1.5")
                  + "&connectionToken=" + encodedToken
                  + "&connectionData=%5B%7B%22name%22%3A%22Streaming%22%7D%5D";

  Serial.printf("[F1] Token: raw=%u chars  encoded=%u chars\n",
                token.length(), encodedToken.length());

  // Always send the headers the F1 server expects — matches the reference
  // (signalr.py) which sets exactly these two headers on the WS connect.
  // Cookie is the raw Set-Cookie value from negotiate, forwarded verbatim
  // (including attributes) exactly as the reference does.
  //
  // IMPORTANT: do NOT put \r\n at the end of the last header here.
  // WebSocketsClient::sendHeader() does:
  //   handshake += extraHeaders + "\r\n";
  //   handshake += "User-Agent: arduino-WebSocket-Client\r\n";
  //   handshake += "\r\n";   // ← final blank line
  // If we trail our last header with \r\n, the library adds another \r\n
  // immediately after, inserting a blank line in the *middle* of the HTTP
  // headers.  The server sees that blank line as end-of-headers and then
  // receives "User-Agent: arduino-WebSocket-Client\r\n\r\n" as the first
  // WebSocket frame — which is garbage, causing an immediate close.
  String extraHeaders = "User-Agent: BestHTTP\r\nAccept-Encoding: gzip,identity";
  if (cookie.length() > 0) {
    extraHeaders += "\r\nCookie: " + cookie;  // no trailing \r\n — library adds it
    Serial.printf("[F1] WS cookie header: %.100s%s\n",
                  cookie.c_str(), cookie.length() > 100 ? "..." : "");
  } else {
    Serial.println("[F1] WS cookie header: NONE");
  }
  Serial.printf("[F1] WS path (%u chars, first 120): %.120s\n",
                wsPath.length(), wsPath.c_str());

  // Purge any lingering socket/TLS state from the previous connection attempt.
  // beginSSL() on a dirty g_ws object replays stale connect+disconnect events,
  // causing an immediate spurious "Connection lost" on the very next loop().
  // The intentional flag ensures the resulting WStype_DISCONNECTED is ignored.
  g_intentionalDisconnect = true;
  g_ws.disconnect();

  g_ws.setExtraHeaders(extraHeaders.c_str());
  // Pass "" for protocol to suppress the default "Sec-WebSocket-Protocol: arduino"
  // header — the F1 SignalR server does not recognise that sub-protocol and
  // closes the connection immediately after sending {"S":1}.  Python's
  // websocket-client library sends no sub-protocol header, and it works.
  g_ws.beginSSL(SIGNALR_CONNECT_HOST, SIGNALR_PORT, wsPath.c_str(), "", "");
  g_ws.onEvent(onWsEvent);
  g_ws.setReconnectInterval(0);  // our state machine handles retries

  // Save token and cookie so callSignalRStart() can use them from f1LiveLoop()
  g_signalrEncodedToken = encodedToken;
  g_signalrCookie       = cookie;

  Serial.printf("[F1] Opening WebSocket (token %.8s...)\n", token.c_str());
  return true;  // actual connection result arrives via WStype_CONNECTED callback
}

// ─── Public API ───────────────────────────────────────────────────────────────

void f1LiveBegin() {
  // Keep UTC permanently for parseIso8601 / NTP
  setenv("TZ", "UTC0", 1);
  tzset();

  // Don't validate F1's TLS cert (they use a standard chain but we skip to
  // save ~8 KB of certificate data in flash).
  g_tlsClient.setInsecure();

  g_state = F1State::WIFI_CONNECTING;
  WiFi.mode(WIFI_STA);

  // ── WiFi reset button check ──────────────────────────────────────────────
  // Hold the reset button (GPIO WIFI_RESET_PIN) during power-on to wipe
  // saved credentials and force the configuration portal to open.
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
  delay(100);  // settle time
  if (digitalRead(WIFI_RESET_PIN) == LOW) {
    Serial.println("[F1] WiFi reset button held — erasing saved credentials");
    displayShowPortal("-- RESETTING --");
    WiFiManager wm;
    wm.resetSettings();
    delay(500);
  }

  // Try saved credentials immediately.  If the network is not available,
  // f1LiveLoop()'s WIFI_CONNECTING handler will open the WiFiManager portal
  // after a short grace period.  Doing it this way avoids the double-portal
  // problem: previously autoConnect() was called here AND immediately again
  // in the loop (g_wifiRetryMs == 0), so a second portal opened the instant
  // the first one timed out — confusing if the user submitted credentials
  // near the end of the first portal's window.
  Serial.println("[F1] WiFi: attempting saved credentials…");
  WiFi.begin();
  // Give saved credentials 15 s to connect before the loop opens the portal.
  g_wifiRetryMs = millis() + 15000UL;
}

void f1LiveLoop() {
  uint32_t now = millis();

  // ── 1. WiFi ─────────────────────────────────────────────────────────────────
  if (g_state == F1State::WIFI_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[F1] WiFi OK  IP: %s\n", WiFi.localIP().toString().c_str());
      configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET_SEC, NTP_SERVER);
      g_state = F1State::NTP_SYNC;
    } else if (now >= g_wifiRetryMs) {
      // Network not found or portal previously timed out — re-open the
      // WiFiManager portal so the user can update credentials.
      // Disconnect first to clear any lingering "sta is connecting" state
      // that would cause autoConnect() to fail immediately.
      WiFi.disconnect(false);
      // static: WiFiManager registers WiFi event handlers internally via
      // WiFi.onEvent(). If the instance is destroyed while those handlers are
      // still in the NetworkEvents queue, the next WiFi event calls through a
      // dangling pointer and crashes (InstrFetchProhibited, PC ≈ 0x0).
      // Making it static ensures its lifetime matches the program.
      static WiFiManager wm;
      wm.setConfigPortalTimeout(WIFI_MANAGER_TIMEOUT);
      wm.setAPCallback([](WiFiManager*) {
        Serial.printf("[F1] WiFiManager portal open — SSID: %s\n", WIFI_MANAGER_AP_NAME);
        displayShowPortal(WIFI_MANAGER_AP_NAME);
        effectPortal();
      });
      Serial.println("[F1] WiFiManager: (re)trying...");
      if (wm.autoConnect(WIFI_MANAGER_AP_NAME)) {
        Serial.printf("[F1] WiFi OK  IP: %s\n", WiFi.localIP().toString().c_str());
        configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET_SEC, NTP_SERVER);
        g_state = F1State::NTP_SYNC;
      } else {
        // Portal timed out again — short pause then retry so we don't hammer.
        Serial.println("[F1] WiFiManager portal timed out — will retry");
        g_wifiRetryMs = millis() + 10000UL;
      }
    }
    return;
  }

  // ── 2. NTP sync ─────────────────────────────────────────────────────────────
  if (g_state == F1State::NTP_SYNC) {
    time_t epoch = utcNow();
    if (epoch > 1700000000UL) {  // sanity: after Nov 2023
      struct tm t = {};
      gmtime_r(&epoch, &t);
      Serial.printf("[F1] NTP synced: %04d-%02d-%02dT%02d:%02d:%02dZ\n",
                    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                    t.tm_hour, t.tm_min, t.tm_sec);
      otaCheckAtBoot();
      g_state      = F1State::IDLE;
      g_nextPollMs = 0;  // force immediate poll (now >= 0 is always true)
    }
    return;
  }

  // ── WiFi reconnect guard (for all later states) ──────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[F1] WiFi lost — reconnecting...");
    if (g_wsConnected) {
      g_ws.disconnect();
      g_wsConnected = false;
    }
    g_state = F1State::WIFI_CONNECTING;
    // Don't call WiFi.begin() — the ESP32's own auto-reconnect is already
    // running after a drop, and a redundant begin() causes
    // "sta is connecting, return error".  Just wait 30 s for auto-reconnect
    // before opening the portal.
    g_wifiRetryMs = millis() + 30000UL;
    return;
  }

  // ── 3. IDLE: poll Index.json every minute ────────────────────────────────
  if (g_state == F1State::IDLE) {
    // Run one-shot fetches first — they must complete before we decide whether
    // a session window is active.  Without this order, a boot inside the 30-min
    // pre-window jumps straight to CONNECTING before fetchEventTracker() runs,
    // leaving g_upcomingCount = 0 the whole time the connecting screen is shown.
    if (g_needChampFetch) {
      g_needChampFetch = false;
      fetchChampStandings();
      return;
    }
    if (g_needScheduleFetch) {
      g_needScheduleFetch = false;
      fetchEventTracker();
      return;
    }
    if (now >= g_nextPollMs) {
      g_nextPollMs = now + F1_POLL_INTERVAL_MS;  // schedule next poll
      if (isSessionWindowActive()) {
        g_state = F1State::CONNECTING;
      }
      // Always return after the schedule fetch — subsequent fetches run on
      // later iterations so TLS calls are never back-to-back.
      return;
    }
    return;
  }

  // ── 4. CONNECTING: negotiate + open WebSocket ────────────────────────────
  if (g_state == F1State::CONNECTING) {
    if (connectSignalR()) {
      g_state = F1State::LIVE;
      // Defer the next LIVE-state session-window poll by a full interval.
      // Without this, g_nextPollMs is stale from the IDLE poll (which
      // happened minutes ago during backoff retries), so the first LIVE
      // loop iteration immediately calls isSessionWindowActive() — a
      // blocking HTTP call that takes 2-3 s.  During that time g_ws.loop()
      // is never called, the server's TCP send buffer fills with seed data
      // frames, and the server closes the connection.
      g_nextPollMs = now + F1_POLL_INTERVAL_MS;
      g_nextLiveWindowPollMs = now + LIVE_WINDOW_CHECK_INTERVAL_MS;
    } else {
      // Count HTTP/TLS-level failures the same way WStype_DISCONNECTED counts
      // pre-seed drops — so the g_emptyConnectCount >= 3 escape in RECONNECTING
      // also fires when the server is simply not accepting connections yet.
      g_emptyConnectCount++;
      Serial.printf("[F1] Connection failed — back-off retry (%u consecutive)\n",
                    g_emptyConnectCount);
      g_state          = F1State::RECONNECTING;
      g_reconnectAfter = now + g_backoffMs;
      g_backoffMs      = min(g_backoffMs * 2u, BACKOFF_MAX_MS);
    }
    return;
  }

  // ── 5. LIVE: pump the WebSocket loop ────────────────────────────────────
  if (g_state == F1State::LIVE) {
    static bool s_prevConnected = false;
    g_ws.loop();

    // ── Heartbeat timer reset ─────────────────────────────────────────────────
    // Reset using 'now' (captured before g_ws.loop()) rather than calling
    // millis() inside the callback.  If the callback millis() > now by even
    // 1 ms the uint32_t subtraction below would underflow to ~4 billion,
    // making the heartbeat fire immediately and sending a duplicate subscribe
    // that causes the server to close the socket.
    if (g_wsConnected && !s_prevConnected) {
      // Set timers from 'now' (captured before g_ws.loop()) — avoids uint32_t
      // underflow if millis() inside WStype_CONNECTED was 1 ms ahead of 'now'.
      g_lastHeartbeatMs      = now;
      g_connectedAtMs        = now;
      g_lastStreamActivityMs = now;
      // Send Subscribe here, outside g_ws.loop(), where the SSL write path is
      // unconditionally ready.  Matches the reference: send_json() is called
      // immediately after ws_connect() returns, not inside a library callback.
      bool sent = g_ws.sendTXT(SUBSCRIBE_MSG);
      Serial.printf("[F1] Subscribe %s (%u bytes)\n",
                    sent ? "sent" : "FAILED — will retry on heartbeat",
                    (unsigned)strlen(SUBSCRIBE_MSG));
    }
    s_prevConnected = g_wsConnected;

    // ── Seed data timeout ──────────────────────────────────────────────────
    // If the server accepted the WS upgrade but never delivered the "R" seed
    // frame within 12 seconds, treat it as a failed connection and retry.
    static const uint32_t SEED_TIMEOUT_MS = 12000UL;
    if (g_wsConnected && !g_seedReceived &&
        (now - g_connectedAtMs >= SEED_TIMEOUT_MS)) {
      Serial.println("[F1] No seed data in 12 s — disconnecting");
      g_intentionalDisconnect = true;
      g_ws.disconnect();
      g_wsConnected    = false;
      s_prevConnected  = false;
      g_state          = F1State::RECONNECTING;
      g_reconnectAfter = now + g_backoffMs;
      g_backoffMs      = min(g_backoffMs * 2u, BACKOFF_MAX_MS);
      return;
    }

    // ── Inactivity guard ─────────────────────────────────────────────────────
    // If the seed data arrived but then the server goes silent for 45 s,
    // force a reconnect — matches the reference _monitor_heartbeat logic.
    if (g_wsConnected && g_seedReceived &&
        (now - g_lastStreamActivityMs >= INACTIVITY_TIMEOUT_MS)) {
      Serial.println("[F1] 45 s inactivity — forcing reconnect");
      g_intentionalDisconnect = true;
      g_ws.disconnect();
      g_wsConnected    = false;
      s_prevConnected  = false;
      g_state          = F1State::RECONNECTING;
      g_reconnectAfter = now + BACKOFF_MIN_MS;
      g_backoffMs      = BACKOFF_MIN_MS;  // reset back-off: this is a clean reconnect
      return;
    }

    // ── Heartbeat ─────────────────────────────────────────────────────────────
    if (g_wsConnected && (now - g_lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS)) {
      bool sent = g_ws.sendTXT(SUBSCRIBE_MSG);
      g_lastHeartbeatMs = now;
      Serial.printf("[F1] Heartbeat %s\n", sent ? "sent" : "FAILED");
    }

    // Periodic session window check: if the window closed, disconnect gracefully.
    // Run less often in LIVE to avoid frequent blocking HTTPS fetches.
    if (now >= g_nextLiveWindowPollMs) {
      g_nextLiveWindowPollMs = now + LIVE_WINDOW_CHECK_INTERVAL_MS;
      if (!isSessionWindowActive()) {
        Serial.println("[F1] Session window closed \u2014 disconnecting");
        g_intentionalDisconnect = true;
        g_ws.disconnect();
        g_wsConnected   = false;
        s_prevConnected = false;
        g_trackStatus   = TrackStatus::UNKNOWN;
        g_upcomingCount = 0;  // clear stale schedule immediately
        g_state         = F1State::IDLE;
        g_nextPollMs    = now + F1_POST_WINDOW_MS;  // override: delay re-poll
        g_nextLiveWindowPollMs = 0;
        g_needScheduleFetch = true;
        g_needChampFetch    = true;
        g_activeSessionIsRace   = false;
        g_activeSessionStartUtc = 0;
      }
    }
    return;
  }

  // \u2500\u2500 5b. SESSION_ENDED: main loop plays the animation, then we move to IDLE \u2500\u2500\u2500\u2500\u2500
  // The actual effect (effectSessionFinished) and display (displayShowFinished) are
  // called from F1_Light.ino on the stateChanged transition.  f1_live.cpp simply
  // moves to IDLE on the very next iteration so the main loop has one pass to draw.
  if (g_state == F1State::SESSION_ENDED) {
    g_state = F1State::IDLE;
    return;
  }

  // ── 6. RECONNECTING: wait for back-off, then retry ──────────────────────
  if (g_state == F1State::RECONNECTING) {
    // Do NOT call g_ws.loop() here — pumping the library during back-off can
    // trigger its internal reconnect logic and bypass our timer entirely.
    if (now >= g_reconnectAfter) {
      // ── Empty-connection escape hatch ─────────────────────────────────────
      // If the server accepted the WS upgrade but immediately closed the TCP
      // connection 3 times in a row (no seed data ever delivered), the session
      // is definitively over.  Stop hammering it and return to IDLE.
      if (g_emptyConnectCount >= 3) {
        Serial.printf("[F1] %u consecutive no-seed connections \u2014 session ended, "
                      "returning to IDLE\n", g_emptyConnectCount);
        g_emptyConnectCount = 0;
        g_windowEndUtc      = 0;
        g_backoffMs         = BACKOFF_MIN_MS;  // reset back-off for next session
        g_needScheduleFetch = true;
        g_needChampFetch    = true;
        g_activeSessionIsRace   = false;
        g_activeSessionStartUtc = 0;
        g_state             = F1State::IDLE;
        g_nextPollMs        = now + F1_POLL_INTERVAL_MS;
        return;
      }

      // ── Window-expiry check ───────────────────────────────────────────────
      // Before retrying, check whether the session window is still open.
      // If it has expired (e.g. the session ended while we were in back-off),
      // return to IDLE instead of hammering the server indefinitely.
      time_t utcEpoch = utcNow();
      if (g_windowEndUtc > 0 && utcEpoch > g_windowEndUtc) {
        Serial.printf("[F1] Session window expired %ld s ago \u2014 returning to IDLE\n",
                      (long)(utcEpoch - g_windowEndUtc));
        g_windowEndUtc = 0;  // reset so a future window can be detected fresh
        g_needScheduleFetch = true;
        g_needChampFetch    = true;
        g_activeSessionIsRace   = false;
        g_activeSessionStartUtc = 0;
        g_state        = F1State::IDLE;
        g_nextPollMs   = now + F1_POLL_INTERVAL_MS;
      } else {
        Serial.println("[F1] Retrying connection...");
        g_state = F1State::CONNECTING;
      }
    }
    return;
  }
}

// ─── Accessors ────────────────────────────────────────────────────────────────

bool f1ScheduleRefreshed() {
  if (g_scheduleRefreshed) {
    g_scheduleRefreshed = false;
    return true;
  }
  return false;
}

bool f1ChampRefreshed() {
  if (g_champRefreshed) {
    g_champRefreshed = false;
    return true;
  }
  return false;
}

F1State f1GetState() {
  return g_state;
}

TrackStatus f1GetTrackStatus() {
  return g_trackStatus;
}

bool f1IsRaceSessionActive() {
  return g_activeSessionIsRace;
}

time_t f1GetActiveSessionStartUtc() {
  return g_activeSessionStartUtc;
}

const char* trackStatusName(TrackStatus s) {
  switch (s) {
    case TrackStatus::CLEAR:   return "CLEAR";
    case TrackStatus::YELLOW:  return "YELLOW";
    case TrackStatus::SC:      return "SC";
    case TrackStatus::RED:     return "RED";
    case TrackStatus::VSC:     return "VSC";
    case TrackStatus::VSC_END: return "VSC_END";
    default:                   return "UNKNOWN";
  }
}
