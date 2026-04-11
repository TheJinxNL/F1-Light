#include "effects.h"
#include "config.h"
#include "web_ui.h"

static bool g_autoShow = true;
static bool g_ledDirty = false;

static inline void commitLeds() {
  if (g_autoShow) {
    FastLED.show();
    g_ledDirty = false;
  } else {
    g_ledDirty = true;
  }
}

void effectSetAutoShow(bool enabled) {
  g_autoShow = enabled;
}

void effectFlush() {
  if (!g_autoShow && g_ledDirty) {
    FastLED.show();
    g_ledDirty = false;
  }
}

// ─── Utility ──────────────────────────────────────────────────────────────────

void fillSolid(CRGB colour) {
  fill_solid(leds, NUM_LEDS, colour);
  commitLeds();
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
  commitLeds();
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
  commitLeds();
}

/** Solid red at 50% brightness — no session active. */
static bool g_idleDrawn = false;
static uint8_t  g_idlePulse  = 32;
static int8_t   g_idleDir    = 2;
static uint32_t g_idleLastMs = 0;
static const uint8_t kIdlePulseMin = 20;

void effectIdleReset() {
  g_idleDrawn  = false;  // force redraw on next effectIdle() call
  g_idlePulse  = 32;
  g_idleDir    = 2;
  g_idleLastMs = 0;
}

void effectIdle() {
  const uint32_t now = millis();
  if (g_idleDrawn && (now - g_idleLastMs < 15)) return;

  g_idleDrawn = true;
  g_idleLastMs = now;

  // Idle base: all LEDs on dim red, last LED pulses brighter/dimmer.
  fill_solid(leds, NUM_LEDS, CRGB(IDLE_BASE_RED, 0, 0));

  uint8_t idleBars = webUiGetIdleBatteryBars();
  if (NUM_LEDS >= 4) {
    if (idleBars < 1) idleBars = 1;
    if (idleBars > 4) idleBars = 4;
    uint8_t base = NUM_LEDS - 4;
    for (uint8_t i = 0; i < 4; i++) leds[base + i] = CRGB::Black;
    for (uint8_t i = 0; i < idleBars; i++) {
      uint8_t red = (i == (uint8_t)(idleBars - 1)) ? g_idlePulse : IDLE_BASE_RED;
      leds[base + i] = CRGB(red, 0, 0);
    }
  } else if (NUM_LEDS > 0) {
    leds[NUM_LEDS - 1] = CRGB(g_idlePulse, 0, 0);
  }

  commitLeds();

  if (g_idlePulse >= IDLE_BASE_RED) g_idleDir = -2;
  if (g_idlePulse <= kIdlePulseMin) g_idleDir =  2;
  g_idlePulse = (uint8_t)(g_idlePulse + g_idleDir);
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
  commitLeds();
}

void effectRaceBatteryOverlay(bool raceSession, time_t raceStartUtc) {
  static uint8_t  pulse    = 170;
  static int8_t   pulseDir = 6;
  static uint32_t lastPulseMs = 0;

  if (!raceSession || NUM_LEDS < 4) {
    pulse = 170;
    pulseDir = 6;
    lastPulseMs = 0;
    return;
  }

  uint32_t nowMs = millis();
  if (nowMs - lastPulseMs >= 45) {
    lastPulseMs = nowMs;
    if (pulse >= 240) pulseDir = -6;
    if (pulse <= 90)  pulseDir =  6;
    pulse = (uint8_t)(pulse + pulseDir);
  }

  time_t nowUtc = time(nullptr);
  uint32_t elapsedMs = 0;
  if (raceStartUtc > 0 && nowUtc > raceStartUtc)
    elapsedMs = (uint32_t)(nowUtc - raceStartUtc) * 1000UL;

  uint8_t barsLit;
  if (elapsedMs >= RACE_BATTERY_DRAIN_MS) {
    barsLit = 0;
  } else {
    uint32_t remaining = RACE_BATTERY_DRAIN_MS - elapsedMs;
    uint8_t percent = (uint8_t)((remaining * 100UL) / RACE_BATTERY_DRAIN_MS);
    if (percent == 0) barsLit = 0;
    else if (percent <= 25) barsLit = 1;
    else if (percent <= 50) barsLit = 2;
    else if (percent <= 75) barsLit = 3;
    else barsLit = 4;
  }

  const uint8_t base = NUM_LEDS - 4;
  for (uint8_t i = 0; i < 4; i++) leds[base + i] = CRGB::Black;

  for (uint8_t i = 0; i < barsLit; i++) {
    uint8_t b = (i == (uint8_t)(barsLit - 1)) ? pulse : 200;
    leds[base + i] = CRGB(b, 0, 0);
  }

  commitLeds();
}

// ─── Live track status effects (non-blocking) ─────────────────────────────────

/** Solid green — all clear. */
void effectClear() {
  // Always paint the strip green when CLEAR is active.
  // Status-change throttling is already handled by effectTrackStatus().
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
  commitLeds();
  on = !on;
}

/** Alternating blue/yellow chase — Safety Car deployed. */
void effectSafetyCar() {
  static uint32_t lastMs = 0;
  static bool     phase  = false;

  uint32_t now = millis();
  if (now - lastMs < BLINK_INTERVAL) return;
  lastMs = now;

  const CRGB cA = phase ? COLOR_BLUE   : COLOR_YELLOW;
  const CRGB cB = phase ? COLOR_YELLOW : COLOR_BLUE;

  // Requested grouping: first 9, next 5, last 4.
  for (uint8_t i = 0; i < NUM_LEDS; i++) {
    if (i < 9) {
      leds[i] = cA;          // group 1
    } else if (i < 14) {
      leds[i] = cB;          // group 2
    } else {
      leds[i] = cA;          // group 3 (last 4)
    }
  }

  commitLeds();
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
  commitLeds();
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
  commitLeds();
  on = !on;
}

/** VSC_END: treat the same as CLEAR for now. */
static void effectVscEnd() {
  static uint32_t lastMs = 0;
  static uint8_t  bright = 40;
  static int8_t   dir    = 4;

  uint32_t now = millis();
  if (now - lastMs < 25) return;
  lastMs = now;

  if (bright >= MAX_BRIGHTNESS) dir = -4;
  if (bright <= 20)             dir =  4;
  bright += dir;

  fill_solid(leds, NUM_LEDS, CRGB(0, bright, 0));
  commitLeds();
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
    case TrackStatus::CLEAR:   effectClear();    break;
    case TrackStatus::VSC_END: effectVscEnd();   break;
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
