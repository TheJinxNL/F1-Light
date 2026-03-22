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
// ─── Session type ──────────────────────────────────────────────────────────────────
enum class SessionType : uint8_t {
  UNKNOWN           = 0,
  PRACTICE          = 1,
  QUALIFYING        = 2,
  SPRINT_QUALIFYING = 3,
  SPRINT            = 4,
  RACE              = 5,
};

// ─── Live timing data (updated from TimingData + DriverList feeds) ───────────────
#define MAX_DRIVERS 20

struct DriverTiming {
  char    racingNumber[4];   // "1", "44"
  char    tla[4];            // "VER", "HAM" (from DriverList feed)
  uint8_t position;          // 1-based; 99 = not yet known
  char    bestLap[12];       // best lap time (qualifying)
  char    lastLap[12];       // last completed lap (race leader display)
  char    interval[12];      // gap to car ahead; leader = ""
  bool    inPit;
  bool    knockedOut;        // qualifying: eliminated in Q1/Q2
};

extern SessionType  g_sessionType;
extern char         g_qualStage[4];       // "Q1", "Q2", "Q3"
extern char         g_remainingTime[12];  // "01:23:45" from ExtrapolatedClock
extern uint8_t      g_currentLap;
extern uint8_t      g_totalLaps;
extern uint8_t      g_driverCount;
extern DriverTiming g_drivers[MAX_DRIVERS];

// ─── Championship standings (fetched from Jolpica API while idle) ─────────────
#define MAX_CHAMP_ENTRIES 6

struct ChampEntry {
  uint8_t  position;
  char     code[4];     // "VER", "HAM"
  uint16_t points;      // rounded championship points
};

extern uint8_t    g_champCount;
extern ChampEntry g_champStandings[MAX_CHAMP_ENTRIES];

/** Returns true once after timing data has been updated. Clears on read. */
bool f1TimingRefreshed();
/** Zero all timing globals — call when leaving LIVE state. */
void resetDriverData();
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

/**
 * Returns true once after each Index.json poll has refreshed the session list.
 * Clears the flag on read — call at most once per loop() iteration.
 */
bool f1ScheduleRefreshed();

/** Human-readable track status string for Serial debug. */
const char* trackStatusName(TrackStatus s);
