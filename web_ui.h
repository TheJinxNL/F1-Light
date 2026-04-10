#pragma once

#include <Arduino.h>

// Start/stop and service the lightweight HTTP UI for runtime settings.
void webUiBegin();
void webUiStop();
void webUiLoop();

// Returns true when the HTTP server is currently running.
bool webUiIsRunning();

// Current LED brightness (0-255) controlled via web UI.
uint8_t webUiGetLedBrightness();

// Idle battery bars for manual override: 0 = disabled, 1-4 = bars on.
uint8_t webUiGetIdleBatteryBars();

// Track-status animation test mode (manual override from WebUI).
bool webUiTrackTestEnabled();
uint8_t webUiTrackTestStatusCode();

// Human-readable URL, e.g. "http://192.168.1.42" (empty if not running).
const char* webUiGetUrl();
