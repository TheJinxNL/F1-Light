#include "effects.h"
#include "config.h"

// ─── Utility ──────────────────────────────────────────────────────────────────

void fillSolid(CRGB colour) {
  fill_solid(leds, NUM_LEDS, colour);
  FastLED.show();
}

void clearAll() {
  fillSolid(COLOR_OFF);
}

// ─── Connection / idle state indicators ──────────────────────────────────────

/** Last 4 LEDs solid blue, all others off — WiFiManager portal active. */
void effectPortal() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  for (uint8_t i = NUM_LEDS - 4; i < NUM_LEDS; i++)
    leds[i] = CRGB::Blue;
  FastLED.show();
}

/** Slow breathing white — WiFi / NTP not ready. */
void effectConnecting() {
  static uint32_t lastMs = 0;
  static uint8_t  bright = 0;
  static int8_t   dir    = 3;

  uint32_t now = millis();
  if (now - lastMs < 20) return;
  lastMs = now;

  if (bright >= MAX_BRIGHTNESS) dir = -3;
  if (bright == 0)              dir =  3;
  bright += dir;

  fill_solid(leds, NUM_LEDS, CRGB(bright, bright, bright));
  FastLED.show();
}

/** Solid red at 50% brightness — no session active. */
static bool g_idleDrawn = false;

void effectIdleReset() {
  g_idleDrawn = false;  // force redraw on next effectIdle() call
}

void effectIdle() {
  if (g_idleDrawn) return;
  g_idleDrawn = true;

  FastLED.setBrightness(MAX_BRIGHTNESS / 2);
  fill_solid(leds, NUM_LEDS, CRGB(255, 0, 0));
  FastLED.show();
  FastLED.setBrightness(MAX_BRIGHTNESS);  // restore for live effects
}

/** Gentle blue breathing — connecting to SignalR. */
void effectConnectingSignalR() {
  static uint32_t lastMs = 0;
  static uint8_t  bright = 0;
  static int8_t   dir    = 2;

  uint32_t now = millis();
  if (now - lastMs < 15) return;
  lastMs = now;

  if (bright >= 120) dir = -2;
  if (bright == 0)   dir =  2;
  bright += dir;

  fill_solid(leds, NUM_LEDS, CRGB(0, 0, bright));
  FastLED.show();
}

// ─── Live track status effects (non-blocking) ─────────────────────────────────

/** Solid green — all clear. */
void effectClear() {
  // Only update when we first enter this state; afterwards do nothing.
  static TrackStatus lastCalled = TrackStatus::UNKNOWN;
  if (lastCalled == TrackStatus::CLEAR) return;
  lastCalled = TrackStatus::CLEAR;
  fillSolid(COLOR_GREEN);
}

/** Fast yellow blink at BLINK_INTERVAL ms. */
void effectYellow() {
  static uint32_t lastMs = 0;
  static bool     on     = true;

  uint32_t now = millis();
  if (now - lastMs < BLINK_INTERVAL) return;
  lastMs = now;

  fill_solid(leds, NUM_LEDS, on ? COLOR_YELLOW : COLOR_OFF);
  FastLED.show();
  on = !on;
}

/** Alternating blue/yellow chase — Safety Car deployed. */
void effectSafetyCar() {
  static uint32_t lastMs = 0;
  static bool     phase  = false;

  uint32_t now = millis();
  if (now - lastMs < BLINK_INTERVAL) return;
  lastMs = now;

  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    // Odd/even interleave flips each cycle
    bool even = (i % 2 == 0);
    leds[i] = ((even ^ phase)) ? COLOR_BLUE : COLOR_YELLOW;
  }
  FastLED.show();
  phase = !phase;
}

/** Slow orange pulse — Virtual Safety Car. */
void effectVSC() {
  static uint32_t lastMs = 0;
  static uint8_t  bright = 0;
  static int8_t   dir    = 4;

  uint32_t now = millis();
  if (now - lastMs < 25) return;
  lastMs = now;

  if (bright >= MAX_BRIGHTNESS) dir = -4;
  if (bright == 0)              dir =  4;
  bright += dir;

  // Orange: full R, ~31 % G, no B
  fill_solid(leds, NUM_LEDS, CRGB(bright, (uint8_t)(bright * 0.31f), 0));
  FastLED.show();
}

/** Urgent fast red flash — Red flag. */
void effectRedFlag() {
  static uint32_t lastMs = 0;
  static bool     on     = true;

  uint32_t now = millis();
  // Flash twice as fast as yellow
  if (now - lastMs < (BLINK_INTERVAL / 2)) return;
  lastMs = now;

  fill_solid(leds, NUM_LEDS, on ? COLOR_RED : COLOR_OFF);
  FastLED.show();
  on = !on;
}

/** VSC_END: treat the same as CLEAR for now. */
static void effectVscEnd() {
  fillSolid(COLOR_GREEN);
}

// ─── Dispatcher ───────────────────────────────────────────────────────────────

/**
 * Reset per-effect static state when we switch to a new status.
 * Calling this ensures effects don't carry stale timing from a previous state.
 */
static void resetEffectState() {
  // We achieve reset by clearing the leds and letting each effect
  // reinitialise its own statics on the next call naturally, EXCEPT
  // for effectClear which uses a guard. We clear that here:
  // (a bit of a trick: just clear the strip so the first frame shows correctly)
  clearAll();
}

void effectTrackStatus(TrackStatus status) {
  // Detect status change and reset LEDs
  static TrackStatus prev = TrackStatus::UNKNOWN;
  if (status != prev) {
    resetEffectState();
    prev = status;
  }

  switch (status) {
    case TrackStatus::CLEAR:
    case TrackStatus::VSC_END: effectClear();    break;
    case TrackStatus::YELLOW:  effectYellow();   break;
    case TrackStatus::SC:      effectSafetyCar();break;
    case TrackStatus::VSC:     effectVSC();      break;
    case TrackStatus::RED:     effectRedFlag();  break;
    default:                   effectIdle();     break;  // UNKNOWN
  }
}

// ─── Session-end chequered-flag celebration (blocking, ~3 s) ────────────────────

void effectSessionFinished() {
  // Phase 1: chequered sweep — alternating white/off pattern shifts right
  // 12 sweeps × 80 ms = ~960 ms
  for (uint8_t sweep = 0; sweep < 12; sweep++) {
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
      // XOR phase bit with sweep index so the pattern shifts each cycle
      leds[i] = ((i + sweep) % 2 == 0) ? CRGB::White : CRGB::Black;
    }
    FastLED.show();
    delay(80);
  }

  // Phase 2: three white flashes
  for (uint8_t f = 0; f < 3; f++) {
    fill_solid(leds, NUM_LEDS, CRGB::White);
    FastLED.show();
    delay(350);
    FastLED.clear(false);
    FastLED.show();
    delay(150);
  }

  // Phase 3: slow fade to black
  for (int16_t b = 255; b >= 0; b -= 5) {
    fill_solid(leds, NUM_LEDS, CRGB(b, b, b));
    FastLED.show();
    delay(10);
  }
  FastLED.clear(true);
}

// ─── F1 Race Start Sequence (blocking, demo / startup) ───────────────────────

void raceStartSequence() {
  const uint8_t lightPositions[5] = {0, 10, 5, 7, 17};

  clearAll();

  for (uint8_t i = 0; i < 5; i++) {
    leds[lightPositions[i]] = COLOR_RED;
    FastLED.show();
    delay(START_LIGHT_INTERVAL);
  }

  delay(LIGHTS_OUT_DELAY);

  clearAll();
  delay(100);

  fillSolid(COLOR_GREEN);
  delay(FLASH_DURATION * 15);
  clearAll();
}
