#pragma once
#include <Arduino.h>
#include "f1_live.h"

/**
 * Initialise the SPI bus and ST7789 display.
 * Call once in setup() before any other display function.
 * Requires Adafruit ST7735 and ST7789 Library + Adafruit GFX Library.
 */
void displayBegin();

/**
 * Set backlight brightness (0 = off, 255 = full).
 * Has no effect if TFT_BL is set to -1 in config.h.
 */
void displaySetBrightness(uint8_t brightness);

/** "Please wait" screen shown during WiFi / NTP startup. */
void displayShowConnecting();

/**
 * WiFiManager captive-portal screen.
 * Shows the AP name the user must connect to for WiFi setup.
 */
void displayShowPortal(const char* apName);

/** Boot-time info screen showing the local IP of the web settings page. */
void displayShowWebIp(const char* ipOrUrl);

/** Boot-time OTA status screen with one required and one optional detail line. */
void displayShowOtaStatus(const char* title, const char* detail = nullptr);

/**
 * Idle screen: next race location + upcoming sessions with
 * dates and times converted to the local timezone defined by DISPLAY_TZ_POSIX (DST-aware).
 * Reads from g_upcomingSessions[] populated by f1LiveLoop().
 */
void displayShowIdle();

/**
 * Full-screen live track status screen.
 * Call whenever TrackStatus changes while in F1State::LIVE.
 */
void displayShowLive(TrackStatus status);

/** Chequered-flag "FINISHED" screen — shown when a session ends. */
void displayShowFinished();

/**
 * Countdown overlay — replaces the bottom of the idle screen with a live
 * MM:SS counter. Redraws only the counter area each second so there's no flicker.
 * Call every second while the next session is within COUNTDOWN_WINDOW_SEC.
 * @param secsRemaining seconds until the session starts (0 = starting now)
 * @param sessionName   e.g. "Race"
 */
void displayShowCountdown(int32_t secsRemaining, const char* sessionName);

/** Clear the countdown banner (call when countdown ends or state changes). */
void displayCountdownReset();

/**
 * Championship standings screen (idle alternating view).
 * Shows top-6 drivers with position badge, driver code, and points.
 * Reads from g_champCount, g_champStandings[].
 */
void displayShowChampionship();
