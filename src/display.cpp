/**
 * display.cpp
 * ─────────────────────────────────────────────────────────────────────────────
 * ST7789 170×320 TFT display driver for the F1 Light project.
 *
 * Libraries (install via Arduino Library Manager):
 *   • Adafruit ST7735 and ST7789 Library  by Adafruit
 *   • Adafruit GFX Library                by Adafruit
 *
 * Wiring (see config.h for GPIO numbers):
 *   Display VCC       → 3.3 V
 *   Display GND       → GND
 *   Display SDA/DIN   → TFT_MOSI  (GPIO 23)   ← SDA = MOSI = DIN, same pin
 *   Display SCL/CLK   → TFT_SCLK  (GPIO 14)
 *   Display CS        → TFT_CS    (GPIO 15)
 *   Display DC        → TFT_DC    (GPIO  2)
 *   Display RST       → TFT_RST   (GPIO  4)  — or 3.3 V if no reset pin
 *   Display BL        → 3.3 V (always on) or a PWM GPIO 13 for brightness control
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "display.h"
#include "config.h"
#include "f1logo.h"

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "Formula1_Display_Regular11pt7b.h"
#include "Formula1_Display_Regular7pt7b.h"
#include <time.h>

// ─── RGB565 colour palette ────────────────────────────────────────────────────
#define COL_BG      0x0000  // black
#define COL_WHITE   0xFFFF
#define COL_RED     0xF800
#define COL_GREEN   0x07E0
#define COL_YELLOW  0xFFE0
#define COL_ORANGE  0xFD20  // warm orange
#define COL_BLUE    0x001F
#define COL_GRAY    0x8410  // medium gray
#define COL_DGRAY   0x2104  // dark gray — divider lines
#define COL_LGRAY   0xC618  // light gray — secondary text

// ─── Display instance (HSPI hardware SPI for speed) ──────────────────────────
static SPIClass        g_spi(HSPI);
static Adafruit_ST7789 g_tft(&g_spi, TFT_CS, TFT_DC, TFT_RST);

#if TFT_BL >= 0
static const uint8_t TFT_BL_PWM_CHANNEL = 0;
#endif

// ─── Layout constants (pixels) ────────────────────────────────────────────────
static const int16_t HDR_H       = 48;   // header bar height
static const int16_t LABEL_Y     = 52;   // "NEXT RACE:" label top Y (FreeSans9pt)
static const int16_t MTG_LINE1_Y = 69;   // meeting name first line top Y
static const int16_t MTG_LINE2_Y = 86;   // meeting name second line top Y (if needed)
static const int16_t SEP1_Y_1LN  = 87;   // separator after 1-line meeting name
static const int16_t SEP1_Y_2LN  = 103;  // separator after 2-line meeting name
static const int16_t SESS_FIRST_Y_1LN = 92;
static const int16_t SESS_FIRST_Y_2LN = 108;

// Per-session block heights
static const int16_t SESS_NAME_H = 22;  // session name row (FreeSans12pt 19px + 3px gap)
static const int16_t SESS_DATE_H = 17;  // date row         (FreeSans9pt  14px + 3px gap)
static const int16_t SESS_TIME_H = 22;  // time row         (FreeSans12pt 19px + 3px gap)
static const int16_t SESS_SEP_H  =  8;  // divider + gap between sessions

// ─── Helpers ─────────────────────────────────────────────────────────────────

/** Convert UTC epoch to "HH:MM" string in the configured POSIX timezone (DST-aware). */
static void fmtTime(time_t utc, char* buf, size_t len) {
  setenv("TZ", DISPLAY_TZ_POSIX, 1); tzset();
  struct tm t = {};
  localtime_r(&utc, &t);
  snprintf(buf, len, "%02d:%02d", t.tm_hour, t.tm_min);
  setenv("TZ", "UTC0", 1); tzset();
}

/** Convert UTC epoch to "Mon DD Mmm" string in the configured POSIX timezone (DST-aware). */
static void fmtDate(time_t utc, char* buf, size_t len) {
  static const char* DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
  static const char* MON[] = {"Jan","Feb","Mar","Apr","May","Jun",
                               "Jul","Aug","Sep","Oct","Nov","Dec"};
  setenv("TZ", DISPLAY_TZ_POSIX, 1); tzset();
  struct tm t = {};
  localtime_r(&utc, &t);
  setenv("TZ", "UTC0", 1); tzset();
  snprintf(buf, len, "%s %d %s", DOW[t.tm_wday], t.tm_mday, MON[t.tm_mon]);
}

/** Strip a leading "Formula 1 " or "Formula 1" prefix from a meeting name. */
static const char* stripF1Prefix(const char* name) {
  if (strncmp(name, "FORMULA 1 ", 10) == 0) return name + 10;
  if (strncmp(name, "FORMULA 1",  9) == 0) return name + 9;
  return name;
}

/** Word-wrap a string into two lines of at most maxChars each.
 * Splits at the last space before maxChars; truncates if still too long.
 */
static void wordWrap(const char* src, uint8_t maxChars,
                     char* line1, char* line2, size_t bufLen) {
  size_t len = strlen(src);
  if (len <= maxChars) {
    strncpy(line1, src, bufLen - 1);
    line1[bufLen - 1] = '\0';
    line2[0] = '\0';
    return;
  }
  // Find last space before maxChars
  int split = maxChars;
  for (int i = (int)maxChars - 1; i > 0; i--) {
    if (src[i] == ' ') { split = i; break; }
  }
  strncpy(line1, src, split);
  line1[split] = '\0';

  const char* rest = src + split + (src[split] == ' ' ? 1 : 0);
  strncpy(line2, rest, bufLen - 1);
  line2[bufLen - 1] = '\0';
  if (strlen(line2) > maxChars) line2[maxChars] = '\0';
}

/** Pixel-width-aware word wrap into two lines.
 * Must be called with the target font already active (uses getTextBounds).
 * Walks spaces right-to-left to find the rightmost split where line1 fits.
 * line2 is hard-truncated from the right if it is still too wide.
 */
static void wordWrapPixels(const char* src, int16_t maxPx,
                           char* line1, char* line2, size_t bufLen) {
  line1[0] = '\0';  line2[0] = '\0';
  int16_t x1, y1;  uint16_t tw, th;

  g_tft.getTextBounds(src, 0, 0, &x1, &y1, &tw, &th);
  if ((int16_t)tw <= maxPx) {
    strncpy(line1, src, bufLen - 1);
    line1[bufLen - 1] = '\0';
    return;
  }

  int split = -1;
  for (int i = (int)strlen(src) - 1; i > 0; i--) {
    if (src[i] == ' ') {
      char tmp[40];
      int n = min(i, (int)(sizeof(tmp) - 1));
      strncpy(tmp, src, n);
      tmp[n] = '\0';
      g_tft.getTextBounds(tmp, 0, 0, &x1, &y1, &tw, &th);
      if ((int16_t)tw <= maxPx) { split = i; break; }
    }
  }

  if (split < 0) {
    strncpy(line1, src, bufLen - 1);
    line1[bufLen - 1] = '\0';
    return;
  }

  int n1 = min(split, (int)bufLen - 1);
  strncpy(line1, src, n1);  line1[n1] = '\0';

  const char* rest2 = src + split + 1;
  strncpy(line2, rest2, bufLen - 1);
  line2[bufLen - 1] = '\0';

  g_tft.getTextBounds(line2, 0, 0, &x1, &y1, &tw, &th);
  size_t l2 = strlen(line2);
  while (l2 > 0 && (int16_t)tw > maxPx) {
    line2[--l2] = '\0';
    g_tft.getTextBounds(line2, 0, 0, &x1, &y1, &tw, &th);
  }
}

// Formula1_Display_Regular11pt7b metrics (size 1):
//   cap height = 16 px, yOffset = -16 (baseline offset), descender = 4 px below baseline
static const int8_t FONT2_BASELINE = 16;  // add to top-Y to get cursor Y
static const int8_t FONT2_HEIGHT   = 20;  // total cell height (cap 16 + descender 4)

// Formula1_Display_Regular7pt7b metrics (size 1):
//   cap height = 10 px, yOffset = -10 (baseline offset), descender = 3 px below baseline
static const int8_t FONT1_BASELINE = 10;  // add to top-Y to get cursor Y
static const int8_t FONT1_HEIGHT   = 13;  // total cell height (cap 10 + descender 3)

/** Print text centred horizontally using the current font. */
static void printCentred(const char* str, int16_t y) {
  int16_t x1, y1;
  uint16_t w, h;
  g_tft.getTextBounds(str, 0, y, &x1, &y1, &w, &h);
  g_tft.setCursor((TFT_WIDTH - (int16_t)w) / 2, y);
  g_tft.print(str);
}

/** Draw a full-width horizontal rule. */
static void hRule(int16_t y, uint16_t col = COL_DGRAY) {
  g_tft.drawFastHLine(0, y, TFT_WIDTH, col);
}

// ─── Header bar ───────────────────────────────────────────────────────────────

static void drawHeader(const char* subtitle, uint16_t bgCol, uint16_t fgCol = COL_WHITE, bool useLogo = false) {
  g_tft.fillRect(0, 0, TFT_WIDTH, HDR_H, bgCol);

  if (useLogo) {
    // F1 logo bitmap — centred horizontally, top-aligned with small margin
    int16_t lx = (TFT_WIDTH - F1LOGO_W) / 2;
    g_tft.drawRGBBitmap(lx, 3, f1logo_array, F1LOGO_W, F1LOGO_H);
    // Subtitle — FreeSans12pt, centred below the logo
    g_tft.setFont(&Formula1_Display_Regular11pt7b);
    g_tft.setTextSize(1);
    g_tft.setTextColor(fgCol);
    printCentred(subtitle, 27 + FONT2_BASELINE);
    g_tft.setFont(nullptr);
  } else {
    // "F1" — size 3, left aligned
    g_tft.setTextColor(fgCol);
    g_tft.setTextSize(3);
    g_tft.setCursor(8, 12);
    g_tft.print("F1");
    // Subtitle — FreeSans12pt, right of "F1"
    g_tft.setFont(&Formula1_Display_Regular11pt7b);
    g_tft.setTextSize(1);
    g_tft.setCursor(54, 16 + FONT2_BASELINE);
    g_tft.print(subtitle);
    g_tft.setFont(nullptr);
  }

  // Bottom border
  hRule(HDR_H, fgCol);
}

// ─── Screen implementations ───────────────────────────────────────────────────

void displayShowPortal(const char* apName) {
  g_tft.fillScreen(COL_BG);
  drawHeader("Setup", COL_BLUE);

  g_tft.setFont(&Formula1_Display_Regular7pt7b);
  g_tft.setTextSize(1);
  g_tft.setTextColor(COL_WHITE);
  g_tft.setCursor(8, 60 + FONT1_BASELINE);
  g_tft.print("WiFi not configured.");
  g_tft.setCursor(8, 74 + FONT1_BASELINE);
  g_tft.print("Connect to this network:");

  // AP name — highlighted box
  g_tft.fillRoundRect(4, 84, TFT_WIDTH - 8, 24, 4, COL_BLUE);
  g_tft.setTextColor(COL_WHITE);
  int16_t ax1, ay1; uint16_t aw, ah;
  g_tft.getTextBounds(apName, 0, 0, &ax1, &ay1, &aw, &ah);
  int16_t apX = max((int16_t)8, (int16_t)((TFT_WIDTH - (int16_t)aw) / 2));
  g_tft.setCursor(apX, 84 + (24 + FONT1_BASELINE) / 2);
  g_tft.print(apName);

  g_tft.setTextColor(COL_LGRAY);
  g_tft.setCursor(8, 118 + FONT1_BASELINE);
  g_tft.print("Then open your browser");
  g_tft.setCursor(8, 132 + FONT1_BASELINE);
  g_tft.print("and go to:");

  g_tft.setTextColor(COL_YELLOW);
  g_tft.setCursor(8, 148 + FONT1_BASELINE);
  g_tft.print("192.168.4.1");

  g_tft.setTextColor(COL_LGRAY);
  g_tft.setCursor(8, 164 + FONT1_BASELINE);
  g_tft.print("to select your WiFi");
  g_tft.setCursor(8, 178 + FONT1_BASELINE);
  g_tft.print("network.");
  g_tft.setFont(nullptr);
}

void displayShowConnecting() {
  g_tft.fillScreen(COL_BG);
  drawHeader("WAIT", COL_DGRAY);

  g_tft.setFont(&Formula1_Display_Regular7pt7b);
  g_tft.setTextSize(1);
  g_tft.setTextColor(COL_GRAY);
  g_tft.setCursor(8, 62 + FONT1_BASELINE);
  g_tft.print("Connecting to WiFi...");
  g_tft.setCursor(8, 78 + FONT1_BASELINE);
  g_tft.print("Syncing time (NTP)...");
  g_tft.setFont(nullptr);
}

void displayShowWebIp(const char* ipOrUrl) {
  g_tft.fillScreen(COL_BG);
  drawHeader("WEB", COL_BLUE);

  g_tft.setFont(&Formula1_Display_Regular7pt7b);
  g_tft.setTextSize(1);
  g_tft.setTextColor(COL_WHITE);
  g_tft.setCursor(8, 62 + FONT1_BASELINE);
  g_tft.print("Settings page:");

  g_tft.setTextColor(COL_YELLOW);
  g_tft.setCursor(8, 82 + FONT1_BASELINE);
  g_tft.print((ipOrUrl && ipOrUrl[0]) ? ipOrUrl : "IP unavailable");

  g_tft.setTextColor(COL_GRAY);
  g_tft.setCursor(8, 108 + FONT1_BASELINE);
  g_tft.print("Open in browser to set");
  g_tft.setCursor(8, 122 + FONT1_BASELINE);
  g_tft.print("LED brightness.");
  g_tft.setFont(nullptr);
}

void displayShowOtaStatus(const char* title, const char* detail) {
  g_tft.fillScreen(COL_BG);
  drawHeader("OTA", COL_BLUE);

  g_tft.setFont(&Formula1_Display_Regular11pt7b);
  g_tft.setTextSize(1);
  g_tft.setTextColor(COL_WHITE);
  printCentred((title && title[0]) ? title : "Checking update", 94 + FONT2_BASELINE);
  g_tft.setFont(nullptr);

  g_tft.setFont(&Formula1_Display_Regular7pt7b);
  g_tft.setTextSize(1);
  g_tft.setTextColor(COL_GRAY);
  printCentred((detail && detail[0]) ? detail : "Please wait...", 122 + FONT1_BASELINE);
  g_tft.setFont(nullptr);
}

void displayShowIdle() {
  g_tft.fillScreen(COL_BG);
  drawHeader("Upcoming", COL_RED, COL_WHITE, true);

  // Skip sessions that have clearly ended already (started > 4 h ago).
  // This prevents stale data showing after a race during the 30-min post-window
  // before the next Index.json poll fires.
  // Use uint32_t for 'now' — time() on ESP-IDF 5.x returns int64_t time_t whose
  // upper 32 bits can contain garbage. uint32_t covers all valid F1 timestamps.
  uint32_t nowTs = (uint32_t)time(nullptr);
  uint8_t first = 0;
  while (first < g_upcomingCount &&
         g_upcomingSessions[first].startUtc + 14400u < nowTs) {
    first++;
  }

  // No (fresh) session data available
  if (first >= g_upcomingCount) {
    g_tft.setFont(&Formula1_Display_Regular7pt7b);
    g_tft.setTextSize(1);
    g_tft.setTextColor(COL_GRAY);
    g_tft.setCursor(0, 62 + FONT1_BASELINE);
    g_tft.print("No upcoming sessions");
    g_tft.setFont(nullptr);
    return;
  }

  // ── "Next Race:" label ────────────────────────────────────────────────────
  g_tft.setFont(&Formula1_Display_Regular7pt7b);
  g_tft.setTextColor(COL_GRAY);
  g_tft.setTextSize(1);
  g_tft.setCursor(8, LABEL_Y + FONT1_BASELINE);
  g_tft.print("Next Race:");

  // ── Meeting name (pixel-aware wrap, 2 lines max) ────────────────────────────
  char line1[32] = {}, line2[32] = {};
  wordWrapPixels(stripF1Prefix(g_upcomingSessions[first].meetingName),
                 TFT_WIDTH, line1, line2, sizeof(line1));

  g_tft.setTextColor(COL_WHITE);
  g_tft.setCursor(0, MTG_LINE1_Y + FONT1_BASELINE);
  g_tft.print(line1);

  bool twoLines = (line2[0] != '\0');
  if (twoLines) {
    g_tft.setCursor(0, MTG_LINE2_Y + FONT1_BASELINE);
    g_tft.print(line2);
  }
  g_tft.setFont(nullptr);

  // Separator below meeting name
  int16_t sepY = twoLines ? SEP1_Y_2LN : SEP1_Y_1LN;
  hRule(sepY);

  // ── Sessions ─────────────────────────────────────────────────────────────
  // Show up to 3 upcoming sessions. After exhausting meeting[0] sessions,
  // overflow into the next meeting so e.g. Chinese GP Race + two Japan FP
  // sessions are all visible on the same screen.
  int16_t y     = twoLines ? SESS_FIRST_Y_2LN : SESS_FIRST_Y_1LN;
  uint8_t shown = 0;
  const char* curMeeting = nullptr;

  // Build the TZ label each call so it updates when DST changes.
  // tm_gmtoff is a GNU extension not available on ESP32 newlib, so derive the
  // offset by comparing local and UTC broken-down time for the same epoch.
  char tzLabel[10] = {};
  {
    setenv("TZ", DISPLAY_TZ_POSIX, 1); tzset();
    time_t nowTs = time(nullptr);
    struct tm lt = {}, ut = {};
    localtime_r(&nowTs, &lt);
    gmtime_r(&nowTs, &ut);
    setenv("TZ", "UTC0", 1); tzset();
    int offH = lt.tm_hour - ut.tm_hour;
    if (offH >  12) offH -= 24;   // handle midnight boundary wrap
    if (offH < -12) offH += 24;
    if (offH >= 0)
      snprintf(tzLabel, sizeof(tzLabel), "GMT+%d", offH);
    else
      snprintf(tzLabel, sizeof(tzLabel), "GMT%d",  offH);
  }

  for (uint8_t i = first; i < g_upcomingCount && shown < 3 && y < 298; i++) {
    // When the meeting changes mid-list, insert a divider + next meeting name
    if (curMeeting != nullptr &&
        strcmp(g_upcomingSessions[i].meetingName, curMeeting) != 0) {
      hRule(y + 2);
      y += SESS_SEP_H;
      g_tft.setFont(&Formula1_Display_Regular7pt7b);
      g_tft.setTextSize(1);
      g_tft.setTextColor(COL_LGRAY);
      char nm[28] = {};
      strncpy(nm, stripF1Prefix(g_upcomingSessions[i].meetingName), 27);
      g_tft.setCursor(8, y + FONT1_BASELINE);
      g_tft.print(nm);
      g_tft.setFont(nullptr);
      y += FONT1_HEIGHT + 2;
    }
    curMeeting = g_upcomingSessions[i].meetingName;

    char timeBuf[8], dateBuf[16];
    fmtTime((time_t)g_upcomingSessions[i].startUtc, timeBuf, sizeof(timeBuf));
    fmtDate((time_t)g_upcomingSessions[i].startUtc, dateBuf, sizeof(dateBuf));

    // Session name — yellow, FreeSans12pt
    g_tft.setFont(&Formula1_Display_Regular11pt7b);
    g_tft.setTextSize(1);
    g_tft.setTextColor(COL_YELLOW);
    g_tft.setCursor(8, y + FONT2_BASELINE);
    g_tft.print(g_upcomingSessions[i].sessionName);
    g_tft.setFont(nullptr);
    y += SESS_NAME_H + 1;

    // Date — light gray, FreeSans9pt
    g_tft.setFont(&Formula1_Display_Regular7pt7b);
    g_tft.setTextSize(1);
    g_tft.setTextColor(COL_LGRAY);
    g_tft.setCursor(8, y + FONT1_BASELINE);
    g_tft.print(dateBuf);
    g_tft.setFont(nullptr);
    y += SESS_DATE_H - 1;

    // Time (FreeSans12pt, white) + TZ label (FreeSans9pt, gray) on the same baseline
    g_tft.setFont(&Formula1_Display_Regular11pt7b);
    g_tft.setTextSize(1);
    g_tft.setTextColor(COL_WHITE);
    g_tft.setCursor(8, y + FONT2_BASELINE);
    g_tft.print(timeBuf);
    // measure rendered width for TZ label placement
    int16_t tx1, ty1; uint16_t tw, th;
    g_tft.getTextBounds(timeBuf, 8, y + FONT2_BASELINE, &tx1, &ty1, &tw, &th);
    g_tft.setFont(&Formula1_Display_Regular7pt7b);
    g_tft.setTextColor(COL_GRAY);
    g_tft.setCursor(8 + (int16_t)tw + 4, y + FONT2_BASELINE);
    g_tft.print(tzLabel);
    g_tft.setFont(nullptr);
    y += SESS_TIME_H;

    shown++;

    // Divider between consecutive sessions of the same meeting
    if (shown < 3 && i + 1 < g_upcomingCount &&
        strcmp(g_upcomingSessions[i + 1].meetingName, curMeeting) == 0) {
      hRule(y + 2);
      y += SESS_SEP_H;
    }
  }
}

void displayShowLive(TrackStatus status) {
  uint16_t    bg, fg;
  const char* mainLabel;
  const char* subLabel  = nullptr;
  const char* subLabel2 = nullptr;

  switch (status) {
    case TrackStatus::CLEAR:
      bg = COL_GREEN;  fg = COL_BG;    mainLabel = "CLEAR";    break;
    case TrackStatus::VSC_END:
      bg = COL_GREEN;  fg = COL_BG;    mainLabel = "VSC ENDING";
      break;
    case TrackStatus::YELLOW:
      bg = COL_YELLOW; fg = COL_BG;    mainLabel = "YELLOW";   break;
    case TrackStatus::SC:
      bg = COL_YELLOW; fg = COL_BG;    mainLabel = "SC";       subLabel = "SAFETY CAR"; break;
    case TrackStatus::VSC:
      bg = COL_ORANGE; fg = COL_BG;    mainLabel = "VSC";      subLabel = "VIRTUAL SC"; break;
    case TrackStatus::RED:
      bg = COL_RED;    fg = COL_WHITE; mainLabel = "RED";      subLabel = "FLAG";       break;
    default:
      bg = COL_BG;     fg = COL_WHITE; mainLabel = "LIVE";     break;
  }

  g_tft.fillScreen(bg);

  // "F1 LIVE" small tag in top-left corner
  g_tft.setFont(&Formula1_Display_Regular7pt7b);
  g_tft.setTextSize(1);
  g_tft.setTextColor(fg);
  g_tft.setCursor(6, 6 + FONT1_BASELINE);
  g_tft.print("F1 LIVE");
  g_tft.setFont(nullptr);

  // Main label — size 3 (18px per char), vertically centred
  int16_t mainW = (int16_t)strlen(mainLabel) * 18;
  int16_t mainX = max((int16_t)4, (int16_t)((TFT_WIDTH - mainW) / 2));
  int16_t mainY = (subLabel || subLabel2)
                  ? (TFT_HEIGHT / 2 - 28)
                  : (TFT_HEIGHT / 2 - 12);

  g_tft.setTextColor(fg);
  g_tft.setTextSize(3);
  g_tft.setCursor(mainX, mainY);
  g_tft.print(mainLabel);

  // Sub-label(s) — FreeSans12pt, centred
  if (subLabel) {
    g_tft.setFont(&Formula1_Display_Regular11pt7b);
    g_tft.setTextSize(1);
    int16_t subY = TFT_HEIGHT / 2 + 10 + FONT2_BASELINE;
    printCentred(subLabel, subY);
    if (subLabel2)
      printCentred(subLabel2, subY + 18);
    g_tft.setFont(nullptr);
  }
}

void displayShowFinished() {
  // Chequered flag style: alternating black/white 20-px columns
  const int16_t SQ = 20;  // square size
  for (int16_t x = 0; x < TFT_WIDTH; x += SQ) {
    for (int16_t y = 0; y < TFT_HEIGHT; y += SQ) {
      bool white = (((x / SQ) + (y / SQ)) % 2 == 0);
      g_tft.fillRect(x, y, SQ, SQ, white ? COL_WHITE : COL_BG);
    }
  }

  // "FINISHED" centred, large, in red
  g_tft.setFont(&Formula1_Display_Regular11pt7b);
  g_tft.setTextSize(2);
  g_tft.setTextColor(COL_RED);
  printCentred("FINISHED", TFT_HEIGHT / 2 - 8);
  g_tft.setFont(nullptr);
  g_tft.setTextSize(1);
}

// File-scope flag: true while a countdown is in progress.
// Cleared by displayCountdownReset() so the banner redraws on re-entry.
static bool s_countdownActive = false;

void displayShowCountdown(int32_t secsRemaining, const char* sessionName) {
  if (secsRemaining < 0) secsRemaining = 0;
  int32_t mins = secsRemaining / 60;
  int32_t secs = secsRemaining % 60;

  // ── On first call draw the full banner ───────────────────────────────────
  bool firstCall = !s_countdownActive;
  if (firstCall) {
    s_countdownActive = true;
    // Draw a red banner that sits below the schedule rows
    g_tft.fillRect(0, TFT_HEIGHT - 68, TFT_WIDTH, 68, COL_RED);
    g_tft.drawFastHLine(0, TFT_HEIGHT - 68, TFT_WIDTH, COL_WHITE);

    // Session label
    g_tft.setFont(&Formula1_Display_Regular7pt7b);
    g_tft.setTextSize(1);
    g_tft.setTextColor(COL_WHITE);
    char label[36];
    snprintf(label, sizeof(label), "%.22s Starting in", sessionName);
    int16_t ll1, ly1; uint16_t lw, lh;
    g_tft.getTextBounds(label, 0, 0, &ll1, &ly1, &lw, &lh);
    int16_t lx = max((int16_t)4, (int16_t)((TFT_WIDTH - (int16_t)lw) / 2));
    g_tft.setCursor(lx, TFT_HEIGHT - 60 + FONT1_BASELINE);
    g_tft.print(label);
    g_tft.setFont(nullptr);
  }

  // ── Redraw only the MM:SS digits each second (black-fill then reprint) ───
  char buf[8];
  snprintf(buf, sizeof(buf), "%02ld:%02ld", (long)mins, (long)secs);

  // Size-4 font: each char is 24 px wide, 32 px tall
  int16_t tw = (int16_t)(strlen(buf)) * 24;
  int16_t tx = (TFT_WIDTH - tw) / 2;
  int16_t ty = TFT_HEIGHT - 48;

  g_tft.fillRect(0, ty, TFT_WIDTH, 40, COL_RED);  // clear previous digits
  g_tft.setTextColor(COL_WHITE);
  g_tft.setTextSize(4);
  g_tft.setCursor(tx, ty);
  g_tft.print(buf);
}

// ─── Reset countdown state (call when leaving countdown mode) ────────────────
void displayCountdownReset() {
  s_countdownActive = false;  // forces banner redraw on next countdown entry
  g_tft.fillRect(0, TFT_HEIGHT - 68, TFT_WIDTH, 68, COL_BG);
}

// ─── Championship standings screen ─────────────────────────────────────────────────────────

void displayShowChampionship() {
  g_tft.fillScreen(COL_BG);
  drawHeader("Standings", COL_RED, COL_WHITE, true);

  // ── Column header row ──────────────────────────────────────────────────────
  g_tft.setFont(&Formula1_Display_Regular7pt7b);
  g_tft.setTextSize(1);
  g_tft.setTextColor(COL_GRAY);
  g_tft.setCursor(28, 52 + FONT1_BASELINE);
  g_tft.print("Driver");
  int16_t chx1, chy1; uint16_t chw, chh;
  g_tft.getTextBounds("Pts", 0, 0, &chx1, &chy1, &chw, &chh);
  g_tft.setCursor(TFT_WIDTH - (int16_t)chw - 4, 52 + FONT1_BASELINE);
  g_tft.print("Pts");
  g_tft.setFont(nullptr);
  hRule(67);  // 3px below bottom of FreeSans9pt (top=50, height=14 → bottom=64)

  if (g_champCount == 0) {
    g_tft.setFont(&Formula1_Display_Regular7pt7b);
    g_tft.setTextSize(1);
    g_tft.setTextColor(COL_GRAY);
    g_tft.setCursor(8, 79 + FONT1_BASELINE);
    g_tft.print("Loading standings...");
    g_tft.setFont(nullptr);
    return;
  }

  // 6 rows in 320-68 = 252 px → 42 px each
  static const int16_t ROW_H  = 42;
  static const int16_t ROWS_Y = 68;

  // Position badge: gold / silver / bronze / dark-gray for P4-6
  static const uint16_t BADGE_BG[6] = {
    0xFEA0,      // P1 gold
    COL_LGRAY,   // P2 silver
    COL_ORANGE,  // P3 bronze
    COL_DGRAY,   // P4
    COL_DGRAY,   // P5
    COL_DGRAY,   // P6
  };
  static const uint16_t BADGE_FG[6] = {
    COL_BG,      // P1 dark text on gold
    COL_BG,      // P2 dark text on silver
    COL_BG,      // P3 dark text on orange
    COL_WHITE,   // P4
    COL_WHITE,   // P5
    COL_WHITE,   // P6
  };

  for (uint8_t i = 0; i < g_champCount; i++) {
    const ChampEntry& e  = g_champStandings[i];
    int16_t ry           = ROWS_Y + i * ROW_H;
    uint8_t bi           = (i < 6) ? i : 5;  // badge index clamped

    // Alternate row background
    g_tft.fillRect(0, ry, TFT_WIDTH, ROW_H - 1, (i & 1) ? 0x1082 : COL_BG);

    // ── Position badge: filled circle with centred number ────────────────────
    g_tft.fillCircle(11, ry + 21, 12, BADGE_BG[bi]);
    g_tft.setFont(&Formula1_Display_Regular11pt7b);
    g_tft.setTextSize(1);
    g_tft.setTextColor(BADGE_FG[bi]);
    char posBuf[3];
    snprintf(posBuf, sizeof(posBuf), "%u", e.position);
    // Centre text inside circle using getTextBounds
    int16_t bx1, by1; uint16_t bw, bh;
    g_tft.getTextBounds(posBuf, 0, ry + 21 + FONT2_BASELINE / 2, &bx1, &by1, &bw, &bh);
    g_tft.setCursor(11 - (int16_t)bw / 2, ry + 21 + FONT2_BASELINE / 2);
    g_tft.print(posBuf);
    g_tft.setFont(nullptr);

    // ── Driver code ──────────────────────────────────────────────────────────
    g_tft.setFont(&Formula1_Display_Regular11pt7b);
    g_tft.setTextSize(1);
    g_tft.setTextColor(COL_WHITE);
    g_tft.setCursor(28, ry + 1 + FONT2_BASELINE + (ROW_H - FONT2_HEIGHT) / 2);
    g_tft.print(e.code[0] ? e.code : "???");

    // ── Points (right-aligned, yellow) ────────────────────────────────────
    char ptsBuf[6];
    snprintf(ptsBuf, sizeof(ptsBuf), "%u", e.points);
    int16_t ptx1, pty1; uint16_t ptw, pth;
    g_tft.getTextBounds(ptsBuf, 0, 0, &ptx1, &pty1, &ptw, &pth);
    g_tft.setTextColor(COL_YELLOW);
    g_tft.setCursor(TFT_WIDTH - (int16_t)ptw - 4, ry + FONT2_BASELINE + (ROW_H - FONT2_HEIGHT) / 2);
    g_tft.print(ptsBuf);
    g_tft.setFont(nullptr);

    // Row divider (skip after last row)
    if (i < (uint8_t)(g_champCount - 1))
      hRule(ry + ROW_H - 1, COL_DGRAY);
  }
}

void displaySetBrightness(uint8_t brightness) {
#if TFT_BL >= 0
  ledcWrite(TFT_BL_PWM_CHANNEL, brightness);
#endif
}

void displayBegin() {
  // HSPI: SCLK, MISO (unused/-1), MOSI, CS
  g_spi.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  g_tft.init(TFT_WIDTH, TFT_HEIGHT);
  g_tft.setRotation(0);      // portrait, upright orientation
  g_tft.fillScreen(COL_BG);
  g_tft.setTextWrap(false);

  // Backlight PWM
#if TFT_BL >= 0
  ledcSetup(TFT_BL_PWM_CHANNEL, 5000, 8);   // 5 kHz, 8-bit (0-255)
  ledcAttachPin(TFT_BL, TFT_BL_PWM_CHANNEL);
  ledcWrite(TFT_BL_PWM_CHANNEL, TFT_BL_DEFAULT);
  Serial.printf("[Display] Backlight on GPIO %d, brightness %d\n",
                TFT_BL, TFT_BL_DEFAULT);
#else
  Serial.println("[Display] Backlight PWM disabled (TFT_BL = -1)");
#endif

  Serial.println("[Display] ST7789 170x320 ready");
}
