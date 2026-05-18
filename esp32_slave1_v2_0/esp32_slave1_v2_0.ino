/*
  Slave 1  v2.0  —  6-Row Static Display
  ─────────────────────────────────────────────────────────────────────────
  Hardware : 6 panels wide × 6 tall  (192 × 96 px)

  Row layout (y=0 is bottom):
    y=80..95  Row 1 : Event name              — auto-size, full-width centred
    y=64..79  Row 2 : TeamA (x=0..63)  |  —  |  TeamB (x=128..191)
    y=32..63  Rows 3+4 (32px zone) : ScoreA | Clock | ScoreB
    y=0..31   Rows 5+6 (32px zone) : TOL-A | Arrow | Arrow | TOL-B

  Changes vs v1.4:
    • Teams row: Team A centred over score-A zone (x=0..63),
                 Team B centred over score-B zone (x=128..191).  No "vs".
    • Scores   : Comic24 VY=0 (full 24 px), buffer 6 chars (3-digit safe).
    • Clock tenths: Comic24 (same font as seconds) — both always identical size.
    • Timeout dots: 7×7 px (was 5×5), DOT_MAX=3, filled only (no hollow rings).
*/

#include <DMD32.h>
#include "fonts/Arial_black_16.h"
#include "fonts/Arial14.h"
#include "fonts/SystemFont5x7.h"
#include "fonts/Comic24.h"
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define DISPLAYS_ACROSS 6
#define DISPLAYS_DOWN   6
#define DISPLAY_W       (32 * DISPLAYS_ACROSS)   // 192 px

// Physical y-starts  (y=0 = bottom-most physical row)
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

// Rows 5+6 TOL layout (4 panels, 128 px, mapped to DMD x=64..191)
#define TOL_A_X   64
#define TOL_A_W   32
#define ARR_A_X   96
#define ARR_B_X  128
#define ARR_W     32
#define TOL_B_X  160
#define TOL_B_W   32

// Dot parameters — 7×7 px, 2 px gap, max 3 dots
#define DOT_W      7
#define DOT_H      7
#define DOT_GAP    2
#define DOT_MAX    3
#define DOT_ROW_W  (DOT_MAX * DOT_W + (DOT_MAX - 1) * DOT_GAP)   // 25 px
#define DOT_VY    20   // centre in lower physical panel (VY=16..31)

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
    char eventName[32];
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
} BoardData;

BoardData    rxBuf;
volatile bool newData = false;

char eventText[33] = "WAITING";
char teamAText[16] = "TEAM A";
char teamBText[16] = "TEAM B";
int  lastScoreA      = -1;
int  lastScoreB      = -1;
int  lastClockSecs   = -1;
int  lastClockTenths = -1;
int  lastTimeoutsA   = -1;
int  lastTimeoutsB   = -1;
char lastPossession  = ' ';

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == 1 && data[0] == 0xAA) { ESP.restart(); return; }
    if (len == sizeof(BoardData)) { memcpy(&rxBuf, data, sizeof(rxBuf)); newData = true; }
}

// ── String pixel-width (uses currently selected font) ─────────────────────────
int strPixelWidth(const char* s, int len) {
    int w = 0;
    for (int i = 0; i < len; i++) {
        int cw = dmd.charWidth((unsigned char)s[i]);
        if (cw > 0) w += cw + 1;
    }
    return w;
}

// ── Full-width row (event name) ────────────────────────────────────────────────
void drawRow(const char* text, int y0) {
    int len = strlen(text);
    const uint8_t* font = Arial_Black_16;
    int            fontH = 16;
    dmd.selectFont(Arial_Black_16);
    if (strPixelWidth(text, len) > DISPLAY_W) {
        dmd.selectFont(Arial_14); font = Arial_14; fontH = 14;
        if (strPixelWidth(text, len) > DISPLAY_W) {
            dmd.selectFont(SystemFont5x7); font = SystemFont5x7; fontH = 7;
        }
    }
    int textW = strPixelWidth(text, len);
    int drawX = max(0, (DISPLAY_W - textW) / 2);
    int drawY = y0 + (16 - fontH) / 2;
    dmd.drawFilledBox(0, y0, DISPLAY_W - 1, y0 + 15, GRAPHICS_INVERSE);
    dmd.selectFont(font);
    dmd.drawString(drawX, drawY, text, len, GRAPHICS_NORMAL);
}

// ── Zone text: always Arial_Black_16, truncate to fit zone width ───────────────
void drawZoneText(const char* text, int x0, int zoneW, int y0) {
    int len = (int)strlen(text);
    dmd.selectFont(Arial_Black_16);
    // Trim characters from the end until the text fits
    while (len > 0 && strPixelWidth(text, len) > zoneW) len--;
    int textW = strPixelWidth(text, len);
    int drawX = x0 + max(0, (zoneW - textW) / 2);
    dmd.drawFilledBox(x0, y0, x0 + zoneW - 1, y0 + 15, GRAPHICS_INVERSE);
    dmd.drawString(drawX, y0, text, len, GRAPHICS_NORMAL);
}

// ── Team names: A over score-A column, B over score-B column ──────────────────
void drawTeamNames(const char* teamA, const char* teamB) {
    drawZoneText(teamA,  0,       ZONE_W, Y_TEAMS);  // x=0..63
    dmd.drawFilledBox(ZONE_W, Y_TEAMS, ZONE_BX - 1, Y_TEAMS + 15, GRAPHICS_INVERSE); // clear clock column
    drawZoneText(teamB,  ZONE_BX, ZONE_W, Y_TEAMS);  // x=128..191
}

// ── Cross-row pixel helper ─────────────────────────────────────────────────────
static void writePixelRemapped(int x, int VY, int baseY, uint8_t val) {
    int dmd_y = (VY < 16) ? (baseY + VY + 16) : (baseY + VY - 16);
    int dmd_x = x;
    if (baseY == Y_ROW6 && VY >= 16) dmd_x = x - 64;
    if (dmd_y < 0 || dmd_y >= DISPLAYS_DOWN * 16) return;
    if (dmd_x < 0 || dmd_x >= DISPLAY_W) return;
    dmd.writePixel(dmd_x, dmd_y, GRAPHICS_NORMAL, val);
}

// ── Cross-row character draw ───────────────────────────────────────────────────
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

// ── Vertically-scaled character draw (nearest-neighbour, srcH→dstH) ───────────
// Only valid for fonts where height is a multiple of 8 (e.g. Comic24 = 24px).
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

// ── Score zone (rows 3+4, 32 px virtual) ──────────────────────────────────────
//   Scores: Comic24 at VY=0 (full 24px used, 3-digit safe).
//   Clock:  ≥60s  → Comic24 "M:SS"
//           <60s  → Comic24 seconds + Comic24 tenths (both same font, VY=0)
//   No border box around clock.
void drawScoreZone(int scoreA, int clockSecs, int clockTenths, int scoreB) {
    dmd.drawFilledBox(0, Y_ROW4, DISPLAY_W - 1, Y_ROW3 + 15, GRAPHICS_INVERSE);

    // Score A — Comic24 stretched vertically 24→32 px (fills full zone height)
    dmd.selectFont(Comic24);
    char sa[6]; snprintf(sa, sizeof(sa), "%d", scoreA);
    int wA = strPixelWidth(sa, strlen(sa));
    drawStringScaledRemapped(max(0, (ZONE_W - wA) / 2), 0, sa, strlen(sa), Comic24, Y_ROW4, 24, 32);

    // Score B — Comic24 stretched vertically 24→32 px (fills full zone height)
    dmd.selectFont(Comic24);
    char sb[6]; snprintf(sb, sizeof(sb), "%d", scoreB);
    int wB = strPixelWidth(sb, strlen(sb));
    drawStringScaledRemapped(ZONE_BX + max(0, (ZONE_W - wB) / 2), 0, sb, strlen(sb), Comic24, Y_ROW4, 24, 32);

    // Clock
    if (clockSecs >= 60) {
        int m = clockSecs / 60, s = clockSecs % 60;
        char cl[7]; snprintf(cl, sizeof(cl), "%d:%02d", m, s);
        dmd.selectFont(Comic24);
        int wC = strPixelWidth(cl, strlen(cl));
        int xC = ZONE_CX + max(0, (ZONE_W - wC) / 2);
        drawStringRemapped(xC, 0, cl, strlen(cl), Comic24, Y_ROW4);
    } else {
        // seconds: Comic24 (24px); tenths: Comic24 scaled to 12px (1/2), bottom-aligned
        char ss[4]; snprintf(ss, sizeof(ss), "%d", clockSecs);
        char tt[4]; snprintf(tt, sizeof(tt), ".%d", clockTenths);
        dmd.selectFont(Comic24);
        int wSS = strPixelWidth(ss, strlen(ss));
        int wTT = strPixelWidth(tt, strlen(tt));
        int totalW = wSS + wTT;
        int xC = ZONE_CX + max(0, (ZONE_W - totalW) / 2);
        drawStringRemapped(xC,             0,  ss, strlen(ss), Comic24, Y_ROW4);
        drawStringScaledRemapped(xC + wSS, 12, tt, strlen(tt), Comic24, Y_ROW4, 24, 12);
    }
}

// ── Timeout dot (filled only — no hollow ring for used slots) ─────────────────
static void drawDot(int x, int VY) {
    for (int ry = 0; ry < DOT_H; ry++) {
        for (int rx = 0; rx < DOT_W; rx++) {
            // Shave 4 corners for a round look
            if ((ry == 0 || ry == DOT_H - 1) && (rx == 0 || rx == DOT_W - 1)) continue;
            writePixelRemapped(x + rx, VY + ry, Y_ROW6, 1);
        }
    }
}

// ── TOL zone (rows 5+6) ────────────────────────────────────────────────────────
void drawTolZone(int timeoutsA, char possession, int timeoutsB) {
    dmd.drawFilledBox(0, Y_ROW6, DISPLAY_W - 1, Y_ROW5 + 15, GRAPHICS_INVERSE);

    dmd.selectFont(SystemFont5x7);

    // Team A — only draw filled dots for remaining timeouts
    {
        int x0 = TOL_A_X + max(0, (TOL_A_W - DOT_ROW_W) / 2);
        for (int d = 0; d < timeoutsA && d < DOT_MAX; d++)
            drawDot(x0 + d * (DOT_W + DOT_GAP), DOT_VY);
    }

    // Team B — only draw filled dots for remaining timeouts
    {
        int x0 = TOL_B_X + max(0, (TOL_B_W - DOT_ROW_W) / 2);
        for (int d = 0; d < timeoutsB && d < DOT_MAX; d++)
            drawDot(x0 + d * (DOT_W + DOT_GAP), DOT_VY);
    }

    // Possession arrows
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

// ── Process received data ──────────────────────────────────────────────────────
void checkNewData() {
    if (!newData) return;
    newData = false;

    bool eventChanged = (strncmp(eventText, rxBuf.eventName, 32) != 0);
    bool teamsChanged = (strncmp(teamAText, rxBuf.teamA, 15) != 0) ||
                        (strncmp(teamBText, rxBuf.teamB, 15) != 0);
    bool scoreChanged = (rxBuf.scoreA != lastScoreA) || (rxBuf.scoreB != lastScoreB);
    bool clockChanged = (rxBuf.clockSecs != lastClockSecs) ||
                        (rxBuf.clockSecs < 60 && rxBuf.clockTenths != lastClockTenths);
    bool tolChanged   = (rxBuf.timeoutsA != lastTimeoutsA) ||
                        (rxBuf.timeoutsB != lastTimeoutsB) ||
                        (rxBuf.possession != lastPossession);

    if (eventChanged) {
        strncpy(eventText, rxBuf.eventName, 32); eventText[32] = '\0';
        drawRow(eventText, Y_EVENT);
        Serial.print("Event: "); Serial.println(eventText);
    }
    if (teamsChanged) {
        strncpy(teamAText, rxBuf.teamA, 15); teamAText[15] = '\0';
        strncpy(teamBText, rxBuf.teamB, 15); teamBText[15] = '\0';
        drawTeamNames(teamAText, teamBText);
        Serial.printf("Teams: %s | %s\n", teamAText, teamBText);
    }
    if (scoreChanged || clockChanged) {
        lastScoreA      = rxBuf.scoreA;
        lastScoreB      = rxBuf.scoreB;
        lastClockSecs   = rxBuf.clockSecs;
        lastClockTenths = rxBuf.clockTenths;
        drawScoreZone(rxBuf.scoreA, rxBuf.clockSecs, rxBuf.clockTenths, rxBuf.scoreB);
    }
    if (tolChanged) {
        lastTimeoutsA  = rxBuf.timeoutsA;
        lastTimeoutsB  = rxBuf.timeoutsB;
        lastPossession = rxBuf.possession;
        drawTolZone(rxBuf.timeoutsA, rxBuf.possession, rxBuf.timeoutsB);
    }
}

void setup() {
    Serial.begin(115200);
    waitMs(500);
    dmd.clearScreen(true);
    drawRow("SLAVE1 v2.0", Y_EVENT);
    drawTeamNames("TEAM A", "TEAM B");
    drawScoreZone(0, 600, 0, 0);
    drawTolZone(0, ' ', 0);
    waitMs(1200);
    dmd.clearScreen(true);
    drawRow(eventText, Y_EVENT);
    drawTeamNames(teamAText, teamBText);
    drawScoreZone(0, 600, 0, 0);
    drawTolZone(0, ' ', 0);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
        dmd.clearScreen(true); drawRow("ERR", Y_EVENT);
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 1 v2.0 ready");
    }
}

void loop() {
    scanIfNeeded();
    checkNewData();
}
