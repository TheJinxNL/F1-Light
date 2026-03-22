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

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WiFiManager.h>   // tzapu/WiFiManager
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ─── Internal constants ───────────────────────────────────────────────────────

static const char* SIGNALR_NEGOTIATE_HOST = "livetiming.formula1.com";
static const char* SIGNALR_NEGOTIATE_PATH = "/signalr/negotiate?clientProtocol=1.5&connectionData=%5B%7B%22name%22%3A%22Streaming%22%7D%5D";
static const char* SIGNALR_CONNECT_HOST   = "livetiming.formula1.com";
static const int   SIGNALR_PORT           = 443;
// WebSocket path is built at runtime once we have the token.

// Subscribe to all streams needed for the timing display.
static const char* SUBSCRIBE_MSG =
  "{\"H\":\"Streaming\",\"M\":\"Subscribe\","
  "\"A\":[[\"TrackStatus\",\"SessionStatus\",\"TimingData\","
  "\"DriverList\",\"SessionInfo\",\"LapCount\","
  "\"ExtrapolatedClock\"]],\"I\":1}";

// Heartbeat: re-send subscribe every 4 minutes to keep the SignalR group alive.
static const uint32_t HEARTBEAT_INTERVAL_MS = 4UL * 60UL * 1000UL;

// Reconnect back-off: 5 s → 10 s → 20 s … cap at 60 s
static const uint32_t BACKOFF_MIN_MS  = 5000UL;
static const uint32_t BACKOFF_MAX_MS  = 60000UL;


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
// ─── Live timing data (exposed to display module) ───────────────────────────
SessionType  g_sessionType       = SessionType::UNKNOWN;
char         g_qualStage[4]      = "";
char         g_remainingTime[12] = "--:--";
uint8_t      g_currentLap        = 0;
uint8_t      g_totalLaps         = 0;
uint8_t      g_driverCount       = 0;
DriverTiming g_drivers[MAX_DRIVERS];
static bool     g_timingRefreshed       = false;
static bool     g_seedReceived          = false;  // true once the "R" seed frame is parsed
static uint32_t g_connectedAtMs         = 0;      // millis() when WStype_CONNECTED fired
static uint32_t g_nextPollMs            = 0;      // absolute ms for next IDLE/LIVE poll
static bool     g_intentionalDisconnect = false;  // set before deliberate g_ws.disconnect()
static time_t   g_windowEndUtc          = 0;      // UTC epoch when active session window expires
static uint8_t  g_emptyConnectCount     = 0;      // consecutive connects with no seed data received
uint8_t         g_champCount            = 0;
ChampEntry      g_champStandings[MAX_CHAMP_ENTRIES];
static bool     g_champRefreshed        = false;
static bool     g_needChampFetch        = true;   // fetch standings on next IDLE pass (startup + session end)
static bool     g_needScheduleFetch     = true;   // fetch event-tracker on next IDLE pass (startup + session end)
// ─── Helpers ──────────────────────────────────────────────────────────────────

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
  http.setTimeout(15000);
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
  http.setTimeout(10000);
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("User-Agent", "BestHTTP");

  // HTTPClient only retains response headers you explicitly ask for.
  // Without this call, http.header("Set-Cookie") always returns "".
  const char* wantedHeaders[] = { "Set-Cookie" };
  http.collectHeaders(wantedHeaders, 1);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("[F1] Negotiate HTTP %d\n", code);
    http.end();
    return "";
  }

  // Grab Set-Cookie header for the WS handshake (ARRAffinity load-balancer cookie)
  outCookie = http.header("Set-Cookie");
  Serial.printf("[F1] Negotiate cookie: %s\n",
                outCookie.length() > 0 ? "received" : "NONE (server may reject WS)");

  String payload = http.getString();
  http.end();

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

// ─── Live timing helpers ──────────────────────────────────────────────────────

void resetDriverData() {
  g_driverCount         = 0;
  g_sessionType         = SessionType::UNKNOWN;
  g_qualStage[0]        = '\0';
  strncpy(g_remainingTime, "--:--", sizeof(g_remainingTime) - 1);
  g_currentLap          = 0;
  g_totalLaps           = 0;
  g_seedReceived        = false;
  g_emptyConnectCount   = 0;
  memset(g_drivers, 0, sizeof(g_drivers));
}

static DriverTiming* findOrCreateDriver(const char* racingNumber) {
  for (uint8_t i = 0; i < g_driverCount; i++) {
    if (strcmp(g_drivers[i].racingNumber, racingNumber) == 0)
      return &g_drivers[i];
  }
  if (g_driverCount < MAX_DRIVERS) {
    DriverTiming& d = g_drivers[g_driverCount++];
    memset(&d, 0, sizeof(d));
    strncpy(d.racingNumber, racingNumber, sizeof(d.racingNumber) - 1);
    d.position = 99;
    return &d;
  }
  return nullptr;
}

static void parseTimingData(JsonObject data) {
  if (data.containsKey("SessionPart")) {
    int part = data["SessionPart"].as<int>();
    if (part >= 1 && part <= 3)
      snprintf(g_qualStage, sizeof(g_qualStage), "Q%d", part);
  }
  JsonObject lines = data["Lines"].as<JsonObject>();
  if (lines.isNull()) return;
  for (JsonPair kv : lines) {
    const char* num = kv.key().c_str();
    if (!isDigit((unsigned char)num[0])) continue;
    JsonObject dv = kv.value().as<JsonObject>();
    DriverTiming* d = findOrCreateDriver(num);
    if (!d) continue;
    const char* pos = dv["Position"] | "";
    if (pos[0]) d->position = (uint8_t)atoi(pos);
    const char* bl = dv["BestLapTime"]["Value"] | "";
    if (bl[0]) strncpy(d->bestLap, bl, sizeof(d->bestLap) - 1);
    const char* ll = dv["LastLapTime"]["Value"] | "";
    if (ll[0]) strncpy(d->lastLap, ll, sizeof(d->lastLap) - 1);
    const char* iv = dv["IntervalToPositionAhead"]["Value"] | "";
    if (iv[0]) strncpy(d->interval, iv, sizeof(d->interval) - 1);
    { JsonVariant v = dv["InPit"];      if (!v.isNull()) d->inPit      = v.as<bool>(); }
    { JsonVariant v = dv["KnockedOut"]; if (!v.isNull()) d->knockedOut = v.as<bool>(); }
  }
  g_timingRefreshed = true;
}

static void parseDriverList(JsonObject data) {
  for (JsonPair kv : data) {
    const char* num = kv.key().c_str();
    if (!isDigit((unsigned char)num[0])) continue;
    JsonObject di = kv.value().as<JsonObject>();
    const char* tla = di["Tla"] | "";
    if (strlen(tla) >= 2) {
      DriverTiming* d = findOrCreateDriver(num);
      if (d) strncpy(d->tla, tla, sizeof(d->tla) - 1);
    }
  }
}

static void parseSessionInfo(JsonObject data) {
  const char* name = data["Name"] | "";
  if (!name[0]) name = data["Type"] | "";
  Serial.printf("[F1] Session type: %s\n", name);
  if      (strstr(name, "Sprint Qualifying") || strstr(name, "Shootout"))
    g_sessionType = SessionType::SPRINT_QUALIFYING;
  else if (strstr(name, "Sprint"))
    g_sessionType = SessionType::SPRINT;
  else if (strstr(name, "Qualifying"))
    g_sessionType = SessionType::QUALIFYING;
  else if (strstr(name, "Race"))
    g_sessionType = SessionType::RACE;
  else if (strstr(name, "Practice"))
    g_sessionType = SessionType::PRACTICE;
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
  http.setTimeout(15000);
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
  http.setTimeout(15000);
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
  if (g_upcomingCount > 0) g_scheduleRefreshed = true;
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
      resetDriverData();
      g_ws.sendTXT(SUBSCRIBE_MSG);
      break;

    case WStype_DISCONNECTED: {
      g_wsConnected = false;
      // A proper WS close frame has a 2-byte big-endian close code followed by
      // an optional UTF-8 reason string.  If length < 2 it's a TCP-level drop.
      if (payload && length >= 2) {
        uint16_t code = (uint16_t)((payload[0] << 8) | payload[1]);
        if (length > 2)
          Serial.printf("[F1] WebSocket closed (code %u: %.*s)\n", code,
                        (int)min(length - 2, (size_t)60), (const char*)payload + 2);
        else
          Serial.printf("[F1] WebSocket closed (code %u)\n", code);
      } else {
        Serial.println("[F1] WebSocket disconnected");
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
            g_state         = F1State::IDLE;
            g_nextPollMs    = millis() + F1_POST_WINDOW_MS;
            g_needScheduleFetch = true;
            g_needChampFetch    = true;
            resetDriverData();
          }
        }
        else if (strcmp(stream, "TimingData") == 0) {
          parseTimingData(data);
        }
        else if (strcmp(stream, "DriverList") == 0) {
          parseDriverList(data);
          g_timingRefreshed = true;
        }
        else if (strcmp(stream, "SessionInfo") == 0) {
          parseSessionInfo(data);
          g_timingRefreshed = true;
        }
        else if (strcmp(stream, "LapCount") == 0) {
          if (data.containsKey("CurrentLap")) g_currentLap = (uint8_t)data["CurrentLap"].as<int>();
          if (data.containsKey("TotalLaps"))  g_totalLaps  = (uint8_t)data["TotalLaps"].as<int>();
          g_timingRefreshed = true;
        }
        else if (strcmp(stream, "ExtrapolatedClock") == 0) {
          const char* rem = data["Remaining"] | "";
          if (rem[0]) strncpy(g_remainingTime, rem, sizeof(g_remainingTime) - 1);
          g_timingRefreshed = true;
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
          g_wsConnected = false;
          g_trackStatus = TrackStatus::UNKNOWN;
          g_state       = F1State::IDLE;
          g_nextPollMs  = millis() + F1_POST_WINDOW_MS;
          g_needScheduleFetch = true;
          g_needChampFetch    = true;
          resetDriverData();
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

        // Seed timing streams
        JsonObject seedTd = rResult["TimingData"].as<JsonObject>();
        if (!seedTd.isNull()) parseTimingData(seedTd);
        JsonObject seedDl = rResult["DriverList"].as<JsonObject>();
        if (!seedDl.isNull()) parseDriverList(seedDl);
        JsonObject seedSi = rResult["SessionInfo"].as<JsonObject>();
        if (!seedSi.isNull()) parseSessionInfo(seedSi);
        JsonObject seedLc = rResult["LapCount"].as<JsonObject>();
        if (!seedLc.isNull()) {
          if (seedLc.containsKey("CurrentLap")) g_currentLap = (uint8_t)seedLc["CurrentLap"].as<int>();
          if (seedLc.containsKey("TotalLaps"))  g_totalLaps  = (uint8_t)seedLc["TotalLaps"].as<int>();
        }
        JsonObject seedEc = rResult["ExtrapolatedClock"].as<JsonObject>();
        if (!seedEc.isNull()) {
          const char* rem = seedEc["Remaining"] | "";
          if (rem[0]) strncpy(g_remainingTime, rem, sizeof(g_remainingTime) - 1);
        }
        g_seedReceived = true;
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

  // Always send the headers the F1 server expects.
  // Cookie is only present when negotiate returned one (Azure ARRAffinity);
  // without it the connect may be routed to a different backend (HTTP 400).
  String extraHeaders = "User-Agent: BestHTTP\r\n";
  extraHeaders += "Accept-Encoding: gzip, identity\r\n";
  if (cookie.length() > 0) {
    extraHeaders += "Cookie: " + cookie + "\r\n";
    Serial.printf("[F1] WS cookie: %.50s%s\n",
                  cookie.c_str(), cookie.length() > 50 ? "..." : "");
  }

  // Purge any lingering socket/TLS state from the previous connection attempt.
  // beginSSL() on a dirty g_ws object replays stale connect+disconnect events,
  // causing an immediate spurious "Connection lost" on the very next loop().
  // The intentional flag ensures the resulting WStype_DISCONNECTED is ignored.
  g_intentionalDisconnect = true;
  g_ws.disconnect();

  g_ws.setExtraHeaders(extraHeaders.c_str());
  g_ws.beginSSL(SIGNALR_CONNECT_HOST, SIGNALR_PORT, wsPath.c_str());
  g_ws.onEvent(onWsEvent);
  g_ws.setReconnectInterval(0);  // our state machine handles retries

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

  WiFiManager wm;
  wm.setConfigPortalTimeout(WIFI_MANAGER_TIMEOUT);

  // Called while the config portal is open — keep the display updated
  wm.setAPCallback([](WiFiManager*) {
    Serial.printf("[F1] WiFiManager portal open — SSID: %s\n", WIFI_MANAGER_AP_NAME);
    displayShowPortal(WIFI_MANAGER_AP_NAME);
  });

  Serial.println("[F1] WiFiManager: connecting...");
  if (wm.autoConnect(WIFI_MANAGER_AP_NAME)) {
    // Credentials were found (or just entered) — WiFi is now connected
    Serial.printf("[F1] WiFi OK  IP: %s\n", WiFi.localIP().toString().c_str());
    configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    g_state = F1State::NTP_SYNC;
  } else {
    // Portal timed out without credentials — stay in WIFI_CONNECTING so
    // f1LiveLoop() keeps retrying
    Serial.println("[F1] WiFiManager portal timed out — will retry");
  }
}

void f1LiveLoop() {
  uint32_t now = millis();

  // ── 1. WiFi ─────────────────────────────────────────────────────────────────
  if (g_state == F1State::WIFI_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[F1] WiFi OK  IP: %s\n", WiFi.localIP().toString().c_str());
      // Start NTP
      configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET_SEC, NTP_SERVER);
      g_state = F1State::NTP_SYNC;
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
    // WiFiManager has saved credentials — a simple reconnect is enough here
    WiFi.begin();
    return;
  }

  // ── 3. IDLE: poll Index.json every minute ────────────────────────────────
  if (g_state == F1State::IDLE) {
    if (now >= g_nextPollMs) {
      g_nextPollMs = now + F1_POLL_INTERVAL_MS;  // schedule next poll
      if (isSessionWindowActive()) {
        g_state = F1State::CONNECTING;
      }
      // Always return after the schedule fetch — subsequent fetches run on
      // later iterations so TLS calls are never back-to-back.
      return;
    }
    // Championship standings: fetched at startup and after each session ends.
    if (g_needChampFetch) {
      g_needChampFetch = false;
      fetchChampStandings();
      return;
    }
    // Event-tracker: sole source for the upcoming session schedule.
    // Fetched at startup and after each session ends.
    if (g_needScheduleFetch) {
      g_needScheduleFetch = false;
      fetchEventTracker();
      return;
    }
    return;
  }

  // ── 4. CONNECTING: negotiate + open WebSocket ────────────────────────────
  if (g_state == F1State::CONNECTING) {
    if (connectSignalR()) {
      g_state = F1State::LIVE;
    } else {
      Serial.println("[F1] Connection failed — back-off retry");
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
      // Set both timers from 'now' (safe: now <= actual current time).
      // This avoids uint32_t underflow if millis() inside WStype_CONNECTED
      // fired 1 ms ahead of 'now' captured at the start of this function.
      g_lastHeartbeatMs = now;
      g_connectedAtMs   = now;
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

    // ── Heartbeat ─────────────────────────────────────────────────────────────
    if (g_wsConnected && (now - g_lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS)) {
      g_ws.sendTXT(SUBSCRIBE_MSG);
      g_lastHeartbeatMs = now;
      Serial.println("[F1] Heartbeat sent");
    }

    // Periodic session window check: if the window closed, disconnect gracefully
    if (now >= g_nextPollMs) {
      g_nextPollMs = now + F1_POLL_INTERVAL_MS;
      if (!isSessionWindowActive()) {
        Serial.println("[F1] Session window closed \u2014 disconnecting");
        g_intentionalDisconnect = true;
        g_ws.disconnect();
        g_wsConnected   = false;
        s_prevConnected = false;
        g_trackStatus   = TrackStatus::UNKNOWN;
        g_state         = F1State::IDLE;
        g_nextPollMs    = now + F1_POST_WINDOW_MS;  // override: delay re-poll
        g_needScheduleFetch = true;
        g_needChampFetch    = true;
      }
    }
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

bool f1TimingRefreshed() {
  if (g_timingRefreshed) {
    g_timingRefreshed = false;
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
