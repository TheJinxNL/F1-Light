/**
 * main.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 * ESP32-based F1-style LED light bar + TFT display controller.
 * Connects to the F1 live-timing SignalR feed and shows track status on
 * WS2812B LEDs and session data on a 170×320 ST7789 display.
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <FastLED.h>
#include <WiFi.h>
#include "config.h"
#include "effects.h"
#include "display.h"
#include "f1_live.h"
#include "web_ui.h"

// ─── LED array (shared with effects.cpp via extern) ───────────────────────────
CRGB leds[NUM_LEDS];

// ─── Idle view alternation ────────────────────────────────────────────────────
#define IDLE_VIEW_SWITCH_MS  10000UL   // switch between schedule / standings every 10 s
#define COUNTDOWN_WINDOW_SEC 300       // show countdown in the 5 min before a session

static F1State     s_prevState        = F1State::WIFI_CONNECTING;
static TrackStatus s_prevStatus       = TrackStatus::UNKNOWN;
static uint8_t     s_idleView         = 0;   // 0 = schedule, 1 = championship
static uint32_t    s_lastViewSwitchMs = 0;
static bool        s_inCountdown      = false;
static uint32_t    s_lastCountdownMs  = 0;
static bool        s_webIpShown       = false;
static uint32_t    s_webIpShownAtMs   = 0;
static bool        s_prevTestMode     = false;
static uint8_t     s_prevTestCode     = 0;

// ─── setup() ──────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("F1 Light — booting…");

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS)
         .setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(MAX_BRIGHTNESS);
  FastLED.clear(true);

  displayBegin();
  f1LiveBegin();
}

// ─── loop() ───────────────────────────────────────────────────────────────────
void loop() {
  FastLED.setBrightness(webUiGetLedBrightness());
  effectSetAutoShow(false);
  f1LiveLoop();

  // Keep the lightweight settings web server active while WiFi is up.
  // Do not start it until the boot-time OTA check has completed.
  if (WiFi.status() == WL_CONNECTED && f1BootOtaCheckComplete()) {
    if (!webUiIsRunning()) {
      webUiBegin();
      if (webUiIsRunning()) {
        displayShowWebIp(webUiGetUrl());
        s_webIpShown     = true;
        s_webIpShownAtMs = millis();
      }
    }
    webUiLoop();
  } else {
    webUiStop();
    s_webIpShown = false;
  }

  F1State     state  = f1GetState();
  TrackStatus status = f1GetTrackStatus();
  bool testMode      = webUiTrackTestEnabled();
  uint8_t testCode   = webUiTrackTestStatusCode();
  TrackStatus testStatus = TrackStatus::CLEAR;
  switch (testCode) {
    case 1: testStatus = TrackStatus::CLEAR;   break;
    case 2: testStatus = TrackStatus::YELLOW;  break;
    case 4: testStatus = TrackStatus::SC;      break;
    case 5: testStatus = TrackStatus::RED;     break;
    case 6: testStatus = TrackStatus::VSC;     break;
    case 7: testStatus = TrackStatus::VSC_END; break;
    default: testStatus = TrackStatus::CLEAR;  break;
  }
  bool stateChanged  = (state != s_prevState);
  s_prevState        = state;

  // Keep the web settings URL visible for a short hold window before any
  // other display screens are allowed to overwrite it.
  if (s_webIpShown && (millis() - s_webIpShownAtMs < 3000)) {
    switch (state) {
      case F1State::IDLE:
        effectIdle();
        break;

      case F1State::CONNECTING:
      case F1State::RECONNECTING:
        if (status != TrackStatus::UNKNOWN) {
          effectTrackStatus(status);
          effectRaceBatteryOverlay(f1IsRaceSessionActive(), f1GetActiveSessionStartUtc(),
                                   webUiGetRaceBatteryEnabled());
        } else
          effectConnectingSignalR();
        break;

      case F1State::SESSION_ENDED:
        break;
    }

    effectFlush();
    return;
  }

  // WebUI track-status test mode: preview LED animation + live screen override.
  if (testMode && state != F1State::WIFI_CONNECTING &&
      state != F1State::NTP_SYNC && state != F1State::SESSION_ENDED) {
    if (testCode == 99) {
      bool testChanged = (!s_prevTestMode) || (s_prevTestCode != 99) || stateChanged;
      if (testChanged) {
        displayShowFinished();
        effectSessionFinished();
      } else {
        displayShowFinished();
      }

      s_prevTestMode = true;
      s_prevTestCode = 99;
      effectFlush();
      return;
    }

    effectTrackStatus(testStatus);

    bool testChanged = (!s_prevTestMode) || (testCode != s_prevTestCode) || stateChanged;
    if (testChanged) displayShowLive(testStatus);

    s_prevTestMode = true;
    s_prevTestCode = testCode;
    effectFlush();
    return;
  }

  if (s_prevTestMode) {
    // Force fresh redraws when leaving test mode.
    s_prevStatus = TrackStatus::UNKNOWN;
  }
  s_prevTestMode = false;

  // ── State-transition actions ──────────────────────────────────────────────
  if (stateChanged) {
    switch (state) {
      case F1State::WIFI_CONNECTING:
      case F1State::NTP_SYNC:
        if (!s_webIpShown || (millis() - s_webIpShownAtMs > 3000))
          displayShowConnecting();
        break;

      case F1State::IDLE:
        effectIdleReset();
        displaySetBrightness(TFT_BL_DEFAULT);
        s_idleView         = 0;
        s_lastViewSwitchMs = millis();
        s_inCountdown      = false;
        displayShowIdle();  // always draw — function shows "No upcoming sessions" if empty
        break;

      case F1State::CONNECTING:
      case F1State::RECONNECTING:
        // Only show the connecting screen on first-ever connect (no track status yet).
        // On a reconnect from LIVE state, keep the last track status visible instead.
        if (status == TrackStatus::UNKNOWN)
          displayShowConnecting();
        break;

      case F1State::LIVE:
        displaySetBrightness(230);
        s_inCountdown = false;
        displayShowLive(status);
        break;

      case F1State::SESSION_ENDED:
        // Blocking celebration: chequered LED sweep + finished screen.
        // f1_live.cpp advances to IDLE on the very next f1LiveLoop() call,
        // so this runs for exactly one stateChanged pass.
        displayShowFinished();
        effectSessionFinished();
        break;
    }
  }

  // ── Continuous per-state actions ─────────────────────────────────────────
  switch (state) {
    // ── IDLE ────────────────────────────────────────────────────────────────
    case F1State::IDLE: {
      effectIdle();

      // Countdown: if the first upcoming session starts within 5 minutes, show
      // the countdown screen and suppress the view alternation timer.
      if (g_upcomingCount > 0) {
        uint32_t nowTs  = (uint32_t)time(nullptr);  // uint32_t strips int64_t time_t garbage upper bits
        int32_t  secsTo = (int32_t)(g_upcomingSessions[0].startUtc - nowTs);
        bool     wantCd = (secsTo >= 0 && secsTo <= COUNTDOWN_WINDOW_SEC);

        if (wantCd && !s_inCountdown) {
          // Entering countdown
          s_inCountdown     = true;
          s_lastCountdownMs = 0;  // force immediate draw
        } else if (!wantCd && s_inCountdown) {
          // Leaving countdown — fall back to schedule
          s_inCountdown = false;
          s_idleView    = 0;
          displayShowIdle();
        }

        if (s_inCountdown) {
          // Redraw countdown every second
          uint32_t nowMs = millis();
          if (nowMs - s_lastCountdownMs >= 1000) {
            s_lastCountdownMs = nowMs;
            uint32_t nowTs2 = (uint32_t)time(nullptr);
            int32_t  secs   = (int32_t)(g_upcomingSessions[0].startUtc - nowTs2);
            if (secs < 0) secs = 0;
            displayShowCountdown(secs, g_upcomingSessions[0].sessionName);
          }
          break;  // skip view alternation while in countdown
        }
      }

      // View alternation: schedule ↔ championship every IDLE_VIEW_SWITCH_MS
      if (millis() - s_lastViewSwitchMs >= IDLE_VIEW_SWITCH_MS) {
        s_lastViewSwitchMs = millis();
        s_idleView = (s_idleView == 0) ? 1 : 0;
        if (s_idleView == 0) {
          displayShowIdle();  // handles empty state internally ("No upcoming sessions")
        } else {
          if (g_champCount > 0) displayShowChampionship();
          else                  displayShowIdle();  // no standings yet — stay on schedule
        }
      }

      // Callbacks from f1_live: schedule refresh
      if (f1ScheduleRefreshed()) {
        s_idleView         = 0;
        s_lastViewSwitchMs = millis();
        displayShowIdle();  // always redraw — handles empty state internally
      }

      // Championship standings updated — always redraw next time the champ
      // view is shown; if already on it, update immediately.
      if (f1ChampRefreshed()) {
        if (s_idleView == 1 && !s_inCountdown)
          displayShowChampionship();
        // else: standings cached in g_champStandings, drawn on next view switch
      }
      break;
    }

    // ── CONNECTING / RECONNECTING ────────────────────────────────────────────
    case F1State::CONNECTING:
    case F1State::RECONNECTING:
      // If we have a prior track status, keep its LED effect running so the
      // strip stays live-looking during a reconnect.  Only fall back to the
      // blue breathing animation on the very first connect (no status yet).
      if (status != TrackStatus::UNKNOWN) {
        effectTrackStatus(status);
        effectRaceBatteryOverlay(f1IsRaceSessionActive(), f1GetActiveSessionStartUtc(),
                                 webUiGetRaceBatteryEnabled());
      } else
        effectConnectingSignalR();
      break;

    // ── LIVE ───────────────────────────────────────────────────────────────
    case F1State::LIVE: {
      effectTrackStatus(status);
      effectRaceBatteryOverlay(f1IsRaceSessionActive(), f1GetActiveSessionStartUtc(),
                               webUiGetRaceBatteryEnabled());

      // Only redraw when track status changes
      bool statusChanged = (status != s_prevStatus);
      s_prevStatus       = status;
      if (statusChanged) displayShowLive(status);
      break;
    }
    // ── SESSION_ENDED ────────────────────────────────────────────────────────────
    case F1State::SESSION_ENDED:
      // Animation played in stateChanged block; continuous case is a no-op.
      // f1LiveLoop() immediately advances to IDLE on the next call.
      break;
    // ── WIFI / NTP ───────────────────────────────────────────────────────────
    case F1State::WIFI_CONNECTING:
    case F1State::NTP_SYNC:
      effectConnecting();
      break;
  }

  effectFlush();
}
