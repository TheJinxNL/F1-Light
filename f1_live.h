#pragma once
#include <Arduino.h>
#include <time.h>

// ─── Track status enum ────────────────────────────────────────────────────────
// Mirrors the F1 Live Timing "TrackStatus" stream values.
enum class TrackStatus : uint8_t {
  UNKNOWN = 0,
  CLEAR   = 1,  // Green flag / normal racing
  YELLOW  = 2,  // Yellow flag (sector or full)
  SC      = 4,  // Safety Car
  RED     = 5,  // Red flag
  VSC     = 6,  // Virtual Safety Car
  VSC_END = 7,  // VSC ending (used sometimes; treat same as CLEAR)
};

// ─── Connection / session state ───────────────────────────────────────────────
enum class F1State : uint8_t {
  WIFI_CONNECTING,   // Establishing WiFi
  NTP_SYNC,          // Waiting for NTP time
  IDLE,              // No active session window right now
  CONNECTING,        // Session window open; connecting to SignalR
  LIVE,              // Connected and receiving track status
  RECONNECTING,      // Lost connection; back-off retry in progress
  SESSION_ENDED,     // Session just finished — brief celebration before IDLE
};

// ─── Session schedule (populated after every Index.json poll) ─────────────────
#define MAX_UPCOMING_SESSIONS 12

struct SessionInfo {
  char   meetingName[50];  // e.g. "Chinese Grand Prix"
  char   sessionName[24];  // e.g. "Sprint", "Qualifying", "Race"
  time_t startUtc;         // UTC epoch seconds
};

/** Number of entries valid in g_upcomingSessions[] after the latest poll. */
extern uint8_t     g_upcomingCount;

/** Upcoming sessions sorted by start time (future sessions from nearest meeting). */
extern SessionInfo g_upcomingSessions[MAX_UPCOMING_SESSIONS];
// ─── Championship standings (fetched from Jolpica API while idle) ─────────────
#define MAX_CHAMP_ENTRIES 6

struct ChampEntry {
  uint8_t  position;
  char     code[4];     // "VER", "HAM"
  uint16_t points;      // rounded championship points
};

extern uint8_t    g_champCount;
extern ChampEntry g_champStandings[MAX_CHAMP_ENTRIES];

/** Returns true once after championship standings have been refreshed. Clears on read. */
bool f1ChampRefreshed();
// ─── Public API ───────────────────────────────────────────────────────────────

/** Call once in setup() after FastLED is initialised. */
void f1LiveBegin();

/**
 * Call in loop(). Drives WiFi, NTP, schedule polling and the WebSocket.
 * Non-blocking: returns quickly when there is nothing urgent to do.
 */
void f1LiveLoop();

/** Current FSM state — read by the main sketch to drive LED effects. */
F1State   f1GetState();

/** Latest track status received from the live feed. */
TrackStatus f1GetTrackStatus();

/** True when the currently active session window is a Race session. */
bool f1IsRaceSessionActive();

/** UTC start time of the currently active session window (0 if unknown). */
time_t f1GetActiveSessionStartUtc();

/**
 * Returns true once after each Index.json poll has refreshed the session list.
 * Clears the flag on read — call at most once per loop() iteration.
 */
bool f1ScheduleRefreshed();

/** Human-readable track status string for Serial debug. */
const char* trackStatusName(TrackStatus s);
