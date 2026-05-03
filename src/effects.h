#pragma once
#include <FastLED.h>
#include "f1_live.h"

// Forward declaration of the LED array (defined in F1_Light.ino)
extern CRGB leds[];

// ─── Utility ──────────────────────────────────────────────────────────────────

/** Fill all LEDs with a single colour then show. */
void fillSolid(CRGB colour);

/** Turn all LEDs off. */
void clearAll();

/**
 * Enable/disable automatic FastLED.show() inside effect functions.
 * When disabled, effects only update the LED buffer and mark it dirty.
 */
void effectSetAutoShow(bool enabled);

/**
 * Flush pending LED buffer updates when auto-show is disabled.
 * Safe to call each loop; does nothing when there are no pending changes.
 */
void effectFlush();

/** Connection / idle state indicators ──────────────────────────────────────

/** Slow breathing white — WiFi/NTP not yet ready. */
void effectConnecting();

/** Last 4 LEDs solid blue, all others off — WiFiManager portal is active. */
void effectPortal();

/**
 * Idle pattern: all LEDs are dim red, with the last LED pulsing red.
 * Call effectIdleReset() whenever the device re-enters IDLE so the
 * LEDs redraw immediately.
 */
void effectIdle();

/** Reset the idle effect — call once each time F1State transitions to IDLE. */
void effectIdleReset();

/** Gentle blue breathing — connecting / reconnecting to SignalR. */
void effectConnectingSignalR();

/**
 * Overlay a 4-bar battery on the last 4 LEDs during race sessions.
 * animated=true: bars drain over race duration (time-based).
 * animated=false: static full battery (4 bars, no pulse).
 * Pass raceSession=false to suppress the overlay entirely.
 */
void effectRaceBatteryOverlay(bool raceSession, time_t raceStartUtc, bool animated = true);

// ─── Live track status effects ────────────────────────────────────────────────

/**
 * Drive the LEDs for the given TrackStatus.
 * Call every loop() iteration while in F1State::LIVE.
 * All effects are non-blocking (return quickly, use millis() internally).
 */
void effectTrackStatus(TrackStatus status);

// ─── Individual status effects (called by effectTrackStatus) ─────────────────

/** Solid green — clear track. */
void effectClear();

/** Fast yellow blink — yellow flag. */
void effectYellow();

/** Alternating blue/yellow sweep — Safety Car. */
void effectSafetyCar();

/** Slow orange pulse — Virtual Safety Car. */
void effectVSC();

/** Urgent red flash — Red flag. */
void effectRedFlag();

// ─── Session-end celebration ─────────────────────────────────────────────────

/**
 * Blocking chequered-flag celebration — runs for ~3 s when a session ends.
 * Alternating white/off LEDs sweep across the strip, finish with a white
 * flash, then fade to black.  Safe to call because the WebSocket is already
 * closed before this runs.
 */
void effectSessionFinished();

// ─── Retained demo / start-light effects ─────────────────────────────────────

/** F1 race start sequence (blocking, demo only). */
void raceStartSequence();
