/*
  Main Board  v7.0  —  Panel 1  (192 × 96 px)
  ─────────────────────────────────────────────────────────────────────────
  Hardware : 6 panels wide × 6 tall  (192 × 96 px)

  Row layout (y=0 is bottom):
    y=80..95  Row 1 : Event name  (SystemFont5x7, scroll or static)
    y=64..79  Row 2 : [Team A 66px] ["GAME CLOCK" 60px] [Team B 66px]
                       Team names: height×2 scaled (14 px tall), no box
                       "GAME CLOCK": unscaled SystemFont5x7 (7 px), single line
    y=32..63  Rows 3+4 (32 px zone) : Score A | Clock digits | Score B
    y=0..31   Rows 5+6 (32 px zone) : TOL-A | Possession Arrows | TOL-B

  Changes vs v6.0:
    • Team names row — new zone widths: 88 / 16 / 88 px (was 64 / 64 / 64)
    • Team name letters height-scaled ×2 (srcH=7 → dstH=14) via new
      drawStringScaledDirect — uses dmd.writePixel directly (Y_TEAMS is a
      single 16-px panel so NO cross-row remapping needed)
    • Each team name fits in 88 px at original 5-px width + 1-px gap,
      so up to 11 chars render at full 2× height without any truncation
    • "GAME CLOCK" → tiny two-char "GC" label (unscaled = 7 px, half the
      height of team names) centred in the 16-px centre zone
    • Box border (dmd.drawBox) drawn around each team name zone
    • Score / clock zone and all other rows unchanged from v6.0
*/

#include <DMD32.h>
#include "fonts/SystemFont5x7.h"
#include "fonts/Comic24.h"
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define DISPLAYS_ACROSS 6
#define DISPLAYS_DOWN   6
#define DISPLAY_W       (32 * DISPLAYS_ACROSS)   // 192 px

// Virtual Y-starts (y=0 = bottom-most physical row)
#define Y_EVENT  80
#define Y_TEAMS  64
#define Y_ROW3   48
#define Y_ROW4   32
#define Y_ROW5   16
#define Y_ROW6    0

// Score zone — three 64-px columns
#define ZONE_W   64
#define ZONE_CX  64
#define ZONE_BX 128

// Team names row — 66 / 60 / 66 px
// Team names  : max 11 rendered chars × 6 px = 66 px → exactly fills zone
// Centre zone : clock digits zone, 60 px
#define TN_A_X    0
#define TN_A_W   66
#define TN_C_X   66
#define TN_C_W   60
#define TN_B_X  126
#define TN_B_W   66

// TOL zone layout
#define TOL_A_X   64
#define TOL_A_W   32
#define ARR_A_X   96
#define ARR_B_X  128
#define ARR_W     32
#define TOL_B_X  160
#define TOL_B_W   32

// Timeout dots — 7×7 px, 2 px gap, max 3
#define DOT_W      7
#define DOT_H      7
#define DOT_GAP    2
#define DOT_MAX    3
#define DOT_ROW_W  (DOT_MAX * DOT_W + (DOT_MAX - 1) * DOT_GAP)   // 25 px
#define DOT_VY    20

// Event scroll
#define SCROLL_SPEED_MS 40

DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);

unsigned long lastScan = 0;
void scanIfNeeded() {
    if ((long)(micros() - lastScan) >= 300) {
        dmd.scanDisplayBySPI();
        lastScan = micros();
    }
}
void waitMs(long ms) {
    long end = millis() + ms;
    while ((long)(millis() - end) < 0) scanIfNeeded();
}

typedef struct __attribute__((packed)) {
    char eventName[64];
    char teamA[16];
    char teamB[16];
    int  scoreA,    scoreB;
    int  clockSecs, clockTenths;
    int  quarter;
    char possession;
    int  foulsA,    foulsB;
    int  timeoutsA, timeoutsB;
    int  screenMask;
    int  clockRunning;
    int  shotSecs,  shotTenths;
    int  shotRunning;
    int  eventScroll;
    char marketingText[32];
} BoardData;

BoardData     rxBuf;
volatile bool newData = false;

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == 1 && data[0] == 0xAA) { ESP.restart(); return; }
    if (len == sizeof(BoardData)) { memcpy(&rxBuf, data, sizeof(rxBuf)); newData = true; }
}

// ── String pixel-width (uses currently selected font) ────────────────────────
int strPixelWidth(const char* s, int len) {
    int w = 0;
    for (int i = 0; i < len; i++) {
        int cw = dmd.charWidth((unsigned char)s[i]);
        if (cw > 0) w += cw + 1;
    }
    return w;
}

// ── Cross-row pixel helper (for 32 px score/TOL zones) ───────────────────────
static void writePixelRemapped(int x, int VY, int baseY, uint8_t val) {
    int dmd_y = (VY < 16) ? (baseY + VY + 16) : (baseY + VY - 16);
    int dmd_x = x;
    if (baseY == Y_ROW6 && VY >= 16) dmd_x = x - 64;
    if (dmd_y < 0 || dmd_y >= DISPLAYS_DOWN * 16) return;
    if (dmd_x < 0 || dmd_x >= DISPLAY_W) return;
    dmd.writePixel(dmd_x, dmd_y, GRAPHICS_NORMAL, val);
}

// ── Cross-row character draw ──────────────────────────────────────────────────
int drawCharRemapped(int bX, int VY_start, unsigned char letter,
                     const uint8_t* fnt, int baseY) {
    uint8_t height    = pgm_read_byte(fnt + FONT_HEIGHT);
    uint8_t firstChar = pgm_read_byte(fnt + FONT_FIRST_CHAR);
    uint8_t charCount = pgm_read_byte(fnt + FONT_CHAR_COUNT);
    if (letter == ' ') {
        unsigned char n = 'n';
        if (n >= firstChar && n < (firstChar + charCount))
            return pgm_read_byte(fnt + FONT_WIDTH_TABLE + (n - firstChar)) + 1;
        return 5;
    }
    if (letter < firstChar || letter >= (firstChar + charCount)) return 0;
    unsigned char c     = letter - firstChar;
    uint8_t       bytes = (height + 7) / 8;
    uint16_t      index = 0;
    uint8_t       width;
    bool fixedW = (pgm_read_byte(fnt + FONT_LENGTH) == 0 &&
                   pgm_read_byte(fnt + FONT_LENGTH + 1) == 0);
    if (fixedW) {
        width = pgm_read_byte(fnt + FONT_FIXED_WIDTH);
        index = (uint16_t)c * bytes * width + FONT_WIDTH_TABLE;
    } else {
        for (uint8_t i = 0; i < c; i++)
            index += pgm_read_byte(fnt + FONT_WIDTH_TABLE + i);
        index = index * bytes + charCount + FONT_WIDTH_TABLE;
        width = pgm_read_byte(fnt + FONT_WIDTH_TABLE + c);
    }
    for (uint8_t j = 0; j < width; j++) {
        for (uint8_t bi = bytes - 1; bi < 254; bi--) {
            uint8_t data   = pgm_read_byte(fnt + index + j + (bi * width));
            int     offset = (int)bi * 8;
            if (bi == bytes - 1 && bytes > 1) offset = height - 8;
            for (uint8_t k = 0; k < 8; k++) {
                int row = offset + k;
                if (row < (int)bi * 8 || row > (int)height) continue;
                int VY = VY_start + row;
                writePixelRemapped(bX + j, VY, baseY, (data & (1 << k)) ? 1 : 0);
            }
        }
    }
    return width;
}

void drawStringRemapped(int x, int VY_start, const char* s, int len,
                        const uint8_t* fnt, int baseY) {
    int cx = x;
    for (int i = 0; i < len; i++) {
        int w = drawCharRemapped(cx, VY_start, (unsigned char)s[i], fnt, baseY);
        if (w > 0) cx += w + 1;
    }
}

// ── Vertically-scaled character draw (cross-row, for score/clock zone) ───────
int drawCharScaledRemapped(int bX, int VY_start, unsigned char letter,
                           const uint8_t* fnt, int baseY, int srcH, int dstH) {
    uint8_t height    = pgm_read_byte(fnt + FONT_HEIGHT);
    uint8_t firstChar = pgm_read_byte(fnt + FONT_FIRST_CHAR);
    uint8_t charCount = pgm_read_byte(fnt + FONT_CHAR_COUNT);
    if (letter == ' ') {
        unsigned char n = 'n';
        if (n >= firstChar && n < (firstChar + charCount))
            return pgm_read_byte(fnt + FONT_WIDTH_TABLE + (n - firstChar)) + 1;
        return 5;
    }
    if (letter < firstChar || letter >= (firstChar + charCount)) return 0;
    unsigned char c     = letter - firstChar;
    uint8_t       bytes = (height + 7) / 8;
    uint16_t      index = 0;
    uint8_t       width;
    bool fixedW = (pgm_read_byte(fnt + FONT_LENGTH) == 0 &&
                   pgm_read_byte(fnt + FONT_LENGTH + 1) == 0);
    if (fixedW) {
        width = pgm_read_byte(fnt + FONT_FIXED_WIDTH);
        index = (uint16_t)c * bytes * width + FONT_WIDTH_TABLE;
    } else {
        for (uint8_t i = 0; i < c; i++)
            index += pgm_read_byte(fnt + FONT_WIDTH_TABLE + i);
        index = index * bytes + charCount + FONT_WIDTH_TABLE;
        width = pgm_read_byte(fnt + FONT_WIDTH_TABLE + c);
    }
    for (uint8_t j = 0; j < width; j++) {
        for (int sy = 0; sy < srcH; sy++) {
            uint8_t data = pgm_read_byte(fnt + index + j + ((sy / 8) * width));
            uint8_t pix  = (data & (1 << (sy % 8))) ? 1 : 0;
            int dy0 = sy * dstH / srcH;
            int dy1 = (sy + 1) * dstH / srcH;
            if (dy1 <= dy0) dy1 = dy0 + 1;
            for (int dy = dy0; dy < dy1; dy++)
                writePixelRemapped(bX + j, VY_start + dy, baseY, pix);
        }
    }
    return width;
}

void drawStringScaledRemapped(int x, int VY_start, const char* s, int len,
                               const uint8_t* fnt, int baseY, int srcH, int dstH) {
    int cx = x;
    for (int i = 0; i < len; i++) {
        int w = drawCharScaledRemapped(cx, VY_start, (unsigned char)s[i], fnt, baseY, srcH, dstH);
        if (w > 0) cx += w + 1;
    }
}

// ── Direct pixel write — for Y_TEAMS (single 16-px panel, no cross-row swap) ─
static void writePixelDirect(int x, int y, uint8_t val) {
    if (x < 0 || x >= DISPLAY_W || y < 0 || y >= DISPLAYS_DOWN * 16) return;
    dmd.writePixel(x, y, GRAPHICS_NORMAL, val);
}

// Height-only scaled character draw using direct pixel addressing.
// Width is NOT scaled (original font width preserved) → 11 chars still fit in 88 px.
// srcH=7, dstH=14 stretches SystemFont5x7 to fill nearly the full 16-px Y_TEAMS zone.
int drawCharScaledDirect(int bX, int baseY, unsigned char letter,
                          const uint8_t* fnt, int srcH, int dstH) {
    uint8_t height    = pgm_read_byte(fnt + FONT_HEIGHT);
    uint8_t firstChar = pgm_read_byte(fnt + FONT_FIRST_CHAR);
    uint8_t charCount = pgm_read_byte(fnt + FONT_CHAR_COUNT);
    if (letter == ' ') {
        unsigned char n = 'n';
        if (n >= firstChar && n < (firstChar + charCount))
            return pgm_read_byte(fnt + FONT_WIDTH_TABLE + (n - firstChar)) + 1;
        return 5;
    }
    if (letter < firstChar || letter >= (firstChar + charCount)) return 0;
    unsigned char c     = letter - firstChar;
    uint8_t       bytes = (height + 7) / 8;
    uint16_t      index = 0;
    uint8_t       width;
    bool fixedW = (pgm_read_byte(fnt + FONT_LENGTH) == 0 &&
                   pgm_read_byte(fnt + FONT_LENGTH + 1) == 0);
    if (fixedW) {
        width = pgm_read_byte(fnt + FONT_FIXED_WIDTH);
        index = (uint16_t)c * bytes * width + FONT_WIDTH_TABLE;
    } else {
        for (uint8_t i = 0; i < c; i++)
            index += pgm_read_byte(fnt + FONT_WIDTH_TABLE + i);
        index = index * bytes + charCount + FONT_WIDTH_TABLE;
        width = pgm_read_byte(fnt + FONT_WIDTH_TABLE + c);
    }
    for (uint8_t j = 0; j < width; j++) {
        for (int sy = 0; sy < srcH; sy++) {
            uint8_t data = pgm_read_byte(fnt + index + j + ((sy / 8) * width));
            uint8_t pix  = (data & (1 << (sy % 8))) ? 1 : 0;
            int dy0 = sy * dstH / srcH;
            int dy1 = (sy + 1) * dstH / srcH;
            if (dy1 <= dy0) dy1 = dy0 + 1;
            for (int dy = dy0; dy < dy1; dy++)
                writePixelDirect(bX + j, baseY + dy, pix);
        }
    }
    return width;
}

void drawStringScaledDirect(int x, int baseY, const char* s, int len,
                              const uint8_t* fnt, int srcH, int dstH) {
    int cx = x;
    for (int i = 0; i < len; i++) {
        int w = drawCharScaledDirect(cx, baseY, (unsigned char)s[i], fnt, srcH, dstH);
        if (w > 0) cx += w + 1;
    }
}

// ── Event name row (Row 1) ────────────────────────────────────────────────────
char  currentEventText[65]  = "WAITING";
bool  currentEventScroll    = false;
int   eventScrollX          = DISPLAY_W;
unsigned long lastScrollMs  = 0;

void drawEventStatic(const char* text) {
    int len = strnlen(text, 64);
    dmd.selectFont(SystemFont5x7);
    int textW = strPixelWidth(text, len);
    int drawX = max(0, (DISPLAY_W - textW) / 2);
    dmd.drawFilledBox(0, Y_EVENT, DISPLAY_W - 1, Y_EVENT + 15, GRAPHICS_INVERSE);
    dmd.drawString(drawX, Y_EVENT + 4, text, len, GRAPHICS_NORMAL);
}

void stepEventScroll() {
    unsigned long now = millis();
    if ((long)(now - lastScrollMs) < SCROLL_SPEED_MS) return;
    lastScrollMs = now;
    int len = strnlen(currentEventText, 64);
    dmd.selectFont(SystemFont5x7);
    int textW = strPixelWidth(currentEventText, len);
    dmd.drawFilledBox(0, Y_EVENT, DISPLAY_W - 1, Y_EVENT + 15, GRAPHICS_INVERSE);
    dmd.drawString(eventScrollX, Y_EVENT + 4, currentEventText, len, GRAPHICS_NORMAL);
    eventScrollX--;
    if (eventScrollX < -(textW + 4)) eventScrollX = DISPLAY_W;
}

// ── Team names row (Row 2) ────────────────────────────────────────────────────
// Layout: [Team A 66px] ["GAME CLOCK" 60px] [Team B 66px]
// Team names: SystemFont5x7 height-scaled srcH=7 → dstH=14 (fills 16-px zone)
//             Width unchanged (5 px per char + 1 px gap = 6 px per char)
//             11-char team name = 65 px → fits in 66-px zone
// "GAME CLOCK": unscaled SystemFont5x7, 7 px tall, single line (59 px wide)
char teamDisplay[33] = "TEAM A          TEAM B          ";

void drawTeamNamesRow(const char* combined) {
    dmd.selectFont(SystemFont5x7);
    dmd.drawFilledBox(0, Y_TEAMS, DISPLAY_W - 1, Y_TEAMS + 15, GRAPHICS_INVERSE);

    // ── Team A — height-scaled, max 10 chars centred in x=0..63 ─────────────
    char ta[17]; strncpy(ta, combined, 16); ta[16] = '\0';
    int la = strnlen(ta, 16);
    while (la > 0 && ta[la - 1] == ' ') la--;
    if (la > 11) la = 11;
    if (la > 0) {
        int wA = strPixelWidth(ta, la);
        int xA = TN_A_X + max(0, (TN_A_W - wA) / 2);
        drawStringScaledDirect(xA, Y_TEAMS + 1, ta, la, SystemFont5x7, 7, 14);
    }

    // ── Team B — height-scaled, max 10 chars centred in x=128..191 ──────────
    char tb[17]; strncpy(tb, combined + 16, 16); tb[16] = '\0';
    int lb = strnlen(tb, 16);
    while (lb > 0 && tb[lb - 1] == ' ') lb--;
    if (lb > 11) lb = 11;
    if (lb > 0) {
        int wB = strPixelWidth(tb, lb);
        int xB = TN_B_X + max(0, (TN_B_W - wB) / 2);
        drawStringScaledDirect(xB, Y_TEAMS + 1, tb, lb, SystemFont5x7, 7, 14);
    }
}

// ── Score zone helpers ────────────────────────────────────────────────────────
static void clearClockArea() {
    dmd.drawFilledBox(ZONE_CX, Y_ROW4,      ZONE_BX - 1, Y_ROW4 + 15, GRAPHICS_INVERSE);
    scanIfNeeded();
    dmd.drawFilledBox(ZONE_CX, Y_ROW4 + 16, ZONE_BX - 1, Y_ROW4 + 31, GRAPHICS_INVERSE);
}

static void drawClockDigits(int clockSecs, int clockTenths) {
    dmd.selectFont(Comic24);
    if (clockSecs >= 60) {
        int m = clockSecs / 60, s = clockSecs % 60;
        char cl[7]; snprintf(cl, sizeof(cl), "%d:%02d", m, s);
        int wC = strPixelWidth(cl, strlen(cl));
        int xC = ZONE_CX + max(0, (ZONE_W - wC) / 2);
        drawStringRemapped(xC, 0, cl, strlen(cl), Comic24, Y_ROW4);
    } else {
        char ss[4]; snprintf(ss, sizeof(ss), "%d", clockSecs);
        char tt[4]; snprintf(tt, sizeof(tt), ".%d", clockTenths);
        int wSS = strPixelWidth(ss, strlen(ss));
        int wTT = strPixelWidth(tt, strlen(tt));
        int xC  = ZONE_CX + max(0, (ZONE_W - (wSS + wTT)) / 2);
        drawStringRemapped(xC, 0, ss, strlen(ss), Comic24, Y_ROW4);
        drawStringScaledRemapped(xC + wSS, 12, tt, strlen(tt), Comic24, Y_ROW4, 24, 16);
    }
}

int lastDrawScoreA = -1;
int lastDrawScoreB = -1;

void drawScoreZone(int scoreA, int clockSecs, int clockTenths, int scoreB) {
    lastDrawScoreA = scoreA;
    lastDrawScoreB = scoreB;
    dmd.drawFilledBox(0, Y_ROW4,      DISPLAY_W - 1, Y_ROW4 + 15, GRAPHICS_INVERSE);
    scanIfNeeded();
    dmd.drawFilledBox(0, Y_ROW4 + 16, DISPLAY_W - 1, Y_ROW4 + 31, GRAPHICS_INVERSE);
    scanIfNeeded();

    dmd.selectFont(Comic24);
    char sa[6]; snprintf(sa, sizeof(sa), "%d", scoreA);
    int wA = strPixelWidth(sa, strlen(sa));
    drawStringScaledRemapped(max(0, (ZONE_W - wA) / 2), 0, sa, strlen(sa),
                             Comic24, Y_ROW4, 24, 32);

    char sb[6]; snprintf(sb, sizeof(sb), "%d", scoreB);
    int wB = strPixelWidth(sb, strlen(sb));
    drawStringScaledRemapped(ZONE_BX + max(0, (ZONE_W - wB) / 2), 0, sb, strlen(sb),
                             Comic24, Y_ROW4, 24, 32);

    drawClockDigits(clockSecs, clockTenths);
}

void drawClockOnly(int clockSecs, int clockTenths) {
    clearClockArea();
    scanIfNeeded();
    drawClockDigits(clockSecs, clockTenths);
}

// ── Timeout dot ───────────────────────────────────────────────────────────────
static void drawDot(int x, int VY) {
    for (int ry = 0; ry < DOT_H; ry++) {
        for (int rx = 0; rx < DOT_W; rx++) {
            if ((ry == 0 || ry == DOT_H - 1) && (rx == 0 || rx == DOT_W - 1)) continue;
            writePixelRemapped(x + rx, VY + ry, Y_ROW6, 1);
        }
    }
}

// ── TOL zone (Rows 5+6) ───────────────────────────────────────────────────────
void drawTolZone(int timeoutsA, char possession, int timeoutsB) {
    dmd.drawFilledBox(0, Y_ROW6, DISPLAY_W - 1, Y_ROW5 + 15, GRAPHICS_INVERSE);

    {
        int x0 = TOL_A_X + max(0, (TOL_A_W - DOT_ROW_W) / 2);
        for (int d = 0; d < timeoutsA && d < DOT_MAX; d++)
            drawDot(x0 + d * (DOT_W + DOT_GAP), DOT_VY);
    }
    {
        int x0 = TOL_B_X + max(0, (TOL_B_W - DOT_ROW_W) / 2);
        for (int d = 0; d < timeoutsB && d < DOT_MAX; d++)
            drawDot(x0 + d * (DOT_W + DOT_GAP), DOT_VY);
    }

    if (possession == 'A') {
        for (int vy = 3; vy <= 27; vy++) {
            int rowW  = 24 - (abs(vy - 15) * 2);
            if (rowW < 1) rowW = 1;
            int startX = 124 - rowW;
            for (int c = 0; c < rowW; c++) writePixelRemapped(startX + c, vy, Y_ROW6, 1);
        }
    }
    if (possession == 'B') {
        for (int vy = 3; vy <= 27; vy++) {
            int rowW  = 24 - (abs(vy - 15) * 2);
            if (rowW < 1) rowW = 1;
            for (int c = 0; c < rowW; c++) writePixelRemapped(132 + c, vy, Y_ROW6, 1);
        }
    }
}

// ── Local clock (runs independently, synced from software packets) ────────────
int           localSecs        = 600;
int           localTenths      = 0;
bool          localRunning     = false;
bool          lastRxRunning    = false;
unsigned long lastClockTickMs  = 0;

void tickLocalClock() {
    if (!localRunning) return;
    unsigned long now = millis();
    if ((long)(now - lastClockTickMs) < 100) return;
    lastClockTickMs = now;
    if (localSecs == 0 && localTenths == 0) { localRunning = false; return; }
    localTenths--;
    if (localTenths < 0) { localTenths = 9; localSecs--; }
    if (localSecs < 0)   { localSecs = 0; localTenths = 0; localRunning = false; }
}

// Sync rule: never move the clock backward (to a higher value) while running.
// Always snap on: STOP, START, big jump (>2 sec), or received value is ahead.
void syncClock(int rxSecs, int rxTenths, bool rxRunning) {
    int rxTotal  = rxSecs   * 10 + rxTenths;
    int locTotal = localSecs * 10 + localTenths;
    bool starting = ( rxRunning && !lastRxRunning);
    bool stopping = (!rxRunning &&  lastRxRunning);
    bool bigJump  = abs(rxTotal - locTotal) > 20;
    bool rxAhead  = (rxTotal < locTotal);   // lower value = further along countdown
    if (starting || stopping || bigJump || rxAhead) {
        localSecs   = rxSecs;
        localTenths = rxTenths;
    }
    if (starting) { localRunning = true;  lastClockTickMs = millis(); }
    if (stopping) { localRunning = false; }
    lastRxRunning = rxRunning;
}

// ── Cache ─────────────────────────────────────────────────────────────────────
int  lastTickSecs   = -1;
int  lastTickTenths = -1;

char eventText[65]    = "WAITING";
char teamAText[17]    = "TEAM A          ";
char teamBText[17]    = "TEAM B          ";
int  lastScoreA       = -1;
int  lastScoreB       = -1;
int  lastTimeoutsA    = -1;
int  lastTimeoutsB    = -1;
char lastPossession   = ' ';

// ── Process received data ─────────────────────────────────────────────────────
void checkNewData() {
    if (!newData) return;
    newData = false;

    // Sync local clock with received state (one-way: never go backward)
    syncClock(rxBuf.clockSecs, rxBuf.clockTenths, rxBuf.clockRunning != 0);

    bool eventChanged = (strncmp(eventText, rxBuf.eventName, 64) != 0) ||
                        (currentEventScroll != (bool)rxBuf.eventScroll);
    if (eventChanged) {
        strncpy(eventText, rxBuf.eventName, 64); eventText[64] = '\0';
        currentEventScroll = (rxBuf.eventScroll != 0);
        strncpy(currentEventText, eventText, 64); currentEventText[64] = '\0';
        eventScrollX = DISPLAY_W;
        if (!currentEventScroll) drawEventStatic(eventText);
        Serial.printf("Event: %s  scroll=%d\n", eventText, currentEventScroll ? 1 : 0);
    }

    bool teamsChanged = (strncmp(teamAText, rxBuf.teamA, 16) != 0) ||
                        (strncmp(teamBText, rxBuf.teamB, 16) != 0);
    if (teamsChanged) {
        strncpy(teamAText, rxBuf.teamA, 16); teamAText[16] = '\0';
        strncpy(teamBText, rxBuf.teamB, 16); teamBText[16] = '\0';
        memset(teamDisplay, ' ', 32);
        teamDisplay[32] = '\0';
        int la = strnlen(teamAText, 16);
        int lb = strnlen(teamBText, 16);
        memcpy(teamDisplay,      teamAText, la);
        memcpy(teamDisplay + 16, teamBText, lb);
        drawTeamNamesRow(teamDisplay);
        Serial.printf("Teams: [%s][%s]\n", teamAText, teamBText);
    }

    bool scoreChanged = (rxBuf.scoreA != lastScoreA) || (rxBuf.scoreB != lastScoreB);
    if (scoreChanged) {
        lastScoreA = rxBuf.scoreA;
        lastScoreB = rxBuf.scoreB;
        drawScoreZone(lastScoreA, localSecs, localTenths, lastScoreB);
        lastTickSecs   = localSecs;
        lastTickTenths = localTenths;
    }

    bool tolChanged = (rxBuf.timeoutsA != lastTimeoutsA) ||
                      (rxBuf.timeoutsB != lastTimeoutsB) ||
                      (rxBuf.possession != lastPossession);
    if (tolChanged) {
        lastTimeoutsA  = rxBuf.timeoutsA;
        lastTimeoutsB  = rxBuf.timeoutsB;
        lastPossession = rxBuf.possession;
        drawTolZone(rxBuf.timeoutsA, rxBuf.possession, rxBuf.timeoutsB);
    }
}

// ── Update clock display from local clock ─────────────────────────────────────
void updateClockDisplay() {
    bool changed = (localSecs != lastTickSecs) ||
                   (localSecs < 60 && localTenths != lastTickTenths);
    if (!changed) return;
    lastTickSecs   = localSecs;
    lastTickTenths = localTenths;
    if (lastScoreA >= 0)
        drawScoreZone(lastScoreA, localSecs, localTenths, lastScoreB);
    else
        drawClockOnly(localSecs, localTenths);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    waitMs(500);
    dmd.clearScreen(true);
    dmd.selectFont(SystemFont5x7);
    dmd.drawString(72, Y_EVENT + 4, "WAIT", 4, GRAPHICS_NORMAL);
    waitMs(1200);
    dmd.clearScreen(true);
    drawEventStatic(eventText);
    drawTeamNamesRow(teamDisplay);
    drawScoreZone(0, 600, 0, 0);
    drawTolZone(0, ' ', 0);
    localSecs      = 600;
    localTenths    = 0;
    lastTickSecs   = 600;
    lastTickTenths = 0;
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("Main board MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
        dmd.clearScreen(true);
        dmd.selectFont(SystemFont5x7);
        dmd.drawString(80, Y_EVENT + 4, "ERR", 3, GRAPHICS_NORMAL);
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Main board v7.0 ready");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();
    if (currentEventScroll) stepEventScroll();
    checkNewData();
    tickLocalClock();
    updateClockDisplay();
}
