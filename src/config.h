#pragma once

// ─── WiFi ─────────────────────────────────────────────────────────────────────
// Credentials are managed by WiFiManager — no hardcoding needed.
// On first boot (or after reset), the ESP32 opens an access point named below.
// Connect to it with your phone/laptop, then pick your network and enter the password.
// The credentials are saved to flash and reused on every subsequent boot.
#define WIFI_MANAGER_AP_NAME  "F1-Light-Setup"   // AP name shown during config portal
#define WIFI_MANAGER_TIMEOUT  180                  // Portal auto-closes after N seconds
#define WIFI_RESET_PIN        0    // GPIO with built-in pull-up; hold LOW at boot to wipe saved credentials
                                   // GPIO 0 is the BOOT button on most ESP32 DevKits — no extra wiring needed

// ─── NTP (time sync) ──────────────────────────────────────────────────────────
#define NTP_SERVER1    "nl.pool.ntp.org"
#define NTP_SERVER2    "time.google.com"
#define NTP_SERVER3    "time.cloudflare.com"
#define NTP_GMT_OFFSET_SEC   0      // UTC; adjust if you want local time display
#define NTP_DAYLIGHT_OFFSET_SEC 0

// ─── TFT Display (ST7789, 170×320) ──────────────────────────────────────────
// Connect to any free GPIOs — avoid GPIO 18 which drives the LED strip.
#define TFT_MOSI   23   // SPI MOSI  → display SDA/DIN
#define TFT_SCLK   14   // SPI Clock → display SCL/CLK
#define TFT_CS     15   // Chip Select
#define TFT_DC     27   // Data / Command  (moved from GPIO 2 — reserved by WiFi PHY)
#define TFT_RST     4   // Reset (tie to 3.3 V and change to -1 if no reset pin)
#define TFT_BL     13   // Backlight PWM pin  (set to -1 if wired directly to 3.3 V)
#define TFT_BL_DEFAULT  200  // Default backlight brightness 0–255
#define TFT_WIDTH  170
#define TFT_HEIGHT 320

// POSIX timezone string for the display — determines local time and DST shown on screen.
// Central Europe (CET/CEST):  "CET-1CEST,M3.5.0,M10.5.0/3"
// UK (GMT/BST):               "GMT0BST,M3.5.0/1,M10.5.0"
// US Eastern (EST/EDT):       "EST5EDT,M3.2.0,M11.1.0"
// See https://github.com/nayarsystems/posix_tz_db for more zone strings.
#define DISPLAY_TZ_POSIX  "CET-1CEST,M3.5.0,M10.5.0/3"

// ─── F1 Live feed polling ─────────────────────────────────────────────────────
// How often (ms) to poll Index.json to check whether a session window is active
#define F1_POLL_INTERVAL_MS  60000UL  // 1 minute
// If no upcoming sessions are available, retry full schedule refresh at this interval.
#define F1_EMPTY_SCHEDULE_RETRY_MS (30UL * 60UL * 1000UL)  // 30 minutes
// How often (ms) to refresh championship standings while IDLE.
#define F1_CHAMP_REFRESH_MS (60UL * 60UL * 1000UL)  // 60 minutes

// ─── OTA update checks ───────────────────────────────────────────────────────
// Firmware version used for boot-time update checks.
#define FW_VERSION "1.0.13"
// Boot-time OTA is optional; set to 0 to disable remote update checks.
#define OTA_BOOT_CHECK_ENABLED 1
// Manifest endpoint checked once per boot after WiFi + NTP are ready.
#define OTA_MANIFEST_URL "https://www.jinx.nl/f1-light/update/manifest.json"
// HTTPS timeout for manifest request (ms).
#define OTA_MANIFEST_TIMEOUT_MS 7000
// HTTPS timeout for firmware download request (ms).
#define OTA_FIRMWARE_TIMEOUT_MS 20000
// For production, prefer certificate pinning. 1 = insecure TLS.
#define OTA_ALLOW_INSECURE_TLS 1

// Race battery overlay: time-based drain duration in LIVE race sessions.
// Adjust this to tune how quickly the last 4 LEDs drain from full to empty.
#define RACE_BATTERY_DRAIN_MS (90UL * 60UL * 1000UL)  // 90 minutes

// Pre/post buffer around the scheduled session window (ms)
#define F1_PRE_WINDOW_MS    (30UL * 60UL * 1000UL)   // 30 min before
#define F1_POST_WINDOW_MS   (30UL * 60UL * 1000UL)   // 30 min after

// ─── Hardware Configuration ───────────────────────────────────────────────────
#define LED_PIN        18       // GPIO pin connected to LED data line
#define NUM_LEDS       18      // Total number of LEDs in the strip
#define LED_TYPE       WS2812B // LED chipset type
#define COLOR_ORDER    GRB     // Color byte order for your strip

// ─── Brightness ───────────────────────────────────────────────────────────────
#define MAX_BRIGHTNESS 200     // 0–255 (keep below 255 to limit current draw)
#define DIM_BRIGHTNESS  50
#define IDLE_BASE_RED  180     // 0–255 red channel level for idle base LEDs

// ─── Timing (milliseconds) ────────────────────────────────────────────────────
#define START_LIGHT_INTERVAL  1000  // Delay between each red light turning on
#define LIGHTS_OUT_DELAY      1000  // Time all 5 reds stay on before "GO"
#define BLINK_INTERVAL         300  // Generic blink period
#define FLASH_DURATION         100  // Short flash duration

// ─── Colors ───────────────────────────────────────────────────────────────────
#define COLOR_RED     CRGB(255,   0,   0)
#define COLOR_GREEN   CRGB(  0, 255,   0)
#define COLOR_BLUE    CRGB(  0,   0, 255)
#define COLOR_YELLOW  CRGB(255, 200,   0)
#define COLOR_WHITE   CRGB(255, 255, 255)
#define COLOR_ORANGE  CRGB(255,  80,   0)
#define COLOR_OFF     CRGB(  0,   0,   0)
