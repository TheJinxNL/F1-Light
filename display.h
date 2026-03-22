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

/**
 * Idle screen: next race location + upcoming sessions with
 * dates and times converted to GMT+DISPLAY_TZ_OFFSET_HOURS.
 * Reads from g_upcomingSessions[] populated by f1LiveLoop().
 */
void displayShowIdle();

/**
 * Full-screen live track status screen.
 * Call whenever TrackStatus changes while in F1State::LIVE.
 */
void displayShowLive(TrackStatus status);

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
 * Live qualifying screen.
 * Shows track-status header, current Q stage + remaining time,
 * then top-10 drivers sorted by position with TLA and best lap.
 * Reads from g_qualStage, g_remainingTime, g_driverCount, g_drivers[].
 */
void displayShowQualifying(TrackStatus status);

/**
 * Live race / sprint screen.
 * Shows track-status header with current lap, then top-10 drivers
 * with TLA and gap to car ahead (leader shows last lap time).
 * Reads from g_currentLap, g_totalLaps, g_driverCount, g_drivers[].
 */
void displayShowRace(TrackStatus status);

/**
 * Championship standings screen (idle alternating view).
 * Shows top-6 drivers with position badge, driver code, and points.
 * Reads from g_champCount, g_champStandings[].
 */
void displayShowChampionship();
