/*
  Slave 1 v1.2 — 6-Row Static Display
  ─────────────────────────────────────────────────────────────────────────
  Hardware : 6 panels wide × 6 tall  (192 × 96 px)

  Physical row 1 (y=80..95) : Event name       — auto-size, centred
  Physical row 2 (y=64..79) : TeamA vs TeamB   — auto-size, centred
  Physical rows 3+4 combined : ScoreA | Clock | ScoreB  — Comic24 (29 px)
  Physical rows 5+6 combined : TOL dots | Possession arrow | TOL dots

  Cross-row fix (same for both 3+4 and 5+6 zones):
      visual y  0..15  → DMD y = VY + 16   (upper physical row of the pair)
      visual y 16..31  → DMD y = VY - 16   (lower physical row of the pair)

  Rows 3+4 base y = Y_ROW4 = 32  (dmd_y = VY + 32 adjusted inside drawCharRemapped)
  Rows 5+6 base y = Y_ROW6 = 0   (dmd_y uses same 0..31 remap)

  TOL zone (rows 5+6):
    Column 1 (x=0..47)   : "TOL" label top + up to 5 dots = timeoutsA
    Column 2 (x=48..95)  : filled left-pointing triangle if possession=='A'
    Column 3 (x=96..143) : filled right-pointing triangle if possession=='B'
    Column 4 (x=144..191): "TOL" label top + up to 5 dots = timeoutsB

  No timer ISR — scanDisplayBySPI() polled via micros() from loop().
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

// Physical y-starts  (DISPLAYS_DOWN=6 → y=0 = bottom-most physical row)
#define Y_EVENT  80   // physical row 1 — top    16 px
#define Y_TEAMS  64   // physical row 2 — second 16 px
#define Y_ROW3   48   // physical row 3 — upper half of score zone
#define Y_ROW4   32   // physical row 4 — lower half of score zone
#define Y_ROW5   16   // physical row 5 — upper half of TOL zone
#define Y_ROW6    0   // physical row 6 — lower half of TOL zone

// Score zone: three equal 64-px column zones across 192 px
#define ZONE_W   64
#define ZONE_CX  64    // clock zone left edge
#define ZONE_BX  128   // score-B zone left edge

// Comic24 is 29 px tall; centre in 32 px visual zone: (32-29)/2 = 1
#define SCORE_VY  1

// TOL zone: four equal 48-px column zones across 192 px
#define TOL_ZONE_W   48
#define TOL_COL2_X   48    // possession-A column left edge
#define TOL_COL3_X   96    // possession-B column left edge
#define TOL_COL4_X   144   // TOL-B column left edge

DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);

// ── Scan polling (no timer ISR) ───────────────────────────────────────────────
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

// ── Data struct (must match master exactly) ───────────────────────────────────
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

BoardData     rxBuf;
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
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ── String pixel-width helper (call after selectFont) ─────────────────────────
int strPixelWidth(const char* s, int len) {
    int w = 0;
    for (int i = 0; i < len; i++) {
        int cw = dmd.charWidth((unsigned char)s[i]);
        if (cw > 0) w += cw + 1;
    }
    return w;
}

// ── Draw one 16-px row: auto-size font, centre ────────────────────────────────
void drawRow(const char* text, int y0) {
    int len = strlen(text);
    const uint8_t* font  = Arial_Black_16;
    int            fontH = 16;
    dmd.selectFont(Arial_Black_16);
    if (strPixelWidth(text, len) > DISPLAY_W) {
        dmd.selectFont(Arial_14);
        font = Arial_14; fontH = 14;
        if (strPixelWidth(text, len) > DISPLAY_W) {
            dmd.selectFont(SystemFont5x7);
            font = SystemFont5x7; fontH = 7;
        }
    }
    int textW = strPixelWidth(text, len);
    int drawX = max(0, (DISPLAY_W - textW) / 2);
    int drawY = y0 + (16 - fontH) / 2;
    dmd.drawFilledBox(0, y0, DISPLAY_W - 1, y0 + 15, GRAPHICS_INVERSE);
    dmd.selectFont(font);
    dmd.drawString(drawX, drawY, text, len, GRAPHICS_NORMAL);
}

// ── Cross-row pixel drawing (shared by both score zone and TOL zone) ──────────
// baseY: Y_ROW4 (32) for score zone, Y_ROW6 (0) for TOL zone
// Within each zone the remap is:
//   VY 0..15  → dmd_y = baseY + VY + 16  (upper physical row)
//   VY 16..31 → dmd_y = baseY + VY - 16  (lower physical row)
static void writePixelRemapped(int x, int VY, int baseY, uint8_t val) {
    int dmd_y = (VY < 16) ? (baseY + VY + 16) : (baseY + VY - 16);
    if (dmd_y < 0 || dmd_y > (DISPLAYS_DOWN * 16 - 1)) return;
    dmd.writePixel(x, dmd_y, GRAPHICS_NORMAL, val);
}

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

    bool fixedW = (pgm_read_byte(fnt + FONT_LENGTH)     == 0 &&
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

void drawStringRemapped(int x, int VY_start,
                        const char* s, int len, const uint8_t* fnt, int baseY) {
    int cx = x;
    for (int i = 0; i < len; i++) {
        int w = drawCharRemapped(cx, VY_start, (unsigned char)s[i], fnt, baseY);
        if (w > 0) cx += w + 1;
    }
}

// ── Clock border box spanning full visual height of rows 3+4 ─────────────────
void drawClockBox(int x1, int x2) {
    dmd.drawLine(x1, Y_ROW3,      x2, Y_ROW3,      GRAPHICS_NORMAL); // top
    dmd.drawLine(x1, Y_ROW4 + 15, x2, Y_ROW4 + 15, GRAPHICS_NORMAL); // bottom
    dmd.drawLine(x1, Y_ROW4,      x1, Y_ROW3 + 15,  GRAPHICS_NORMAL); // left
    dmd.drawLine(x2, Y_ROW4,      x2, Y_ROW3 + 15,  GRAPHICS_NORMAL); // right
}

// ── Draw score zone (rows 3+4 combined, 32 px visual) ────────────────────────
void drawScoreZone(int scoreA, int clockSecs, int clockTenths, int scoreB) {
    dmd.drawFilledBox(0, Y_ROW4, DISPLAY_W - 1, Y_ROW3 + 15, GRAPHICS_INVERSE);

    // Score A — left zone x=0..63
    dmd.selectFont(Comic24);
    char sa[5];
    snprintf(sa, sizeof(sa), "%d", scoreA);
    int wA = strPixelWidth(sa, strlen(sa));
    int xA = max(0, (ZONE_W - wA) / 2);
    drawStringRemapped(xA, SCORE_VY, sa, strlen(sa), Comic24, Y_ROW4);

    // Score B — right zone x=128..191
    dmd.selectFont(Comic24);
    char sb[5];
    snprintf(sb, sizeof(sb), "%d", scoreB);
    int wB = strPixelWidth(sb, strlen(sb));
    int xB = ZONE_BX + max(0, (ZONE_W - wB) / 2);
    drawStringRemapped(xB, SCORE_VY, sb, strlen(sb), Comic24, Y_ROW4);

    // Clock — centre zone x=64..127
    dmd.selectFont(Comic24);
    if (clockSecs >= 60) {
        int m = clockSecs / 60, s = clockSecs % 60;
        char cl[7];
        snprintf(cl, sizeof(cl), "%d:%02d", m, s);
        int wC = strPixelWidth(cl, strlen(cl));
        int xC = ZONE_CX + max(0, (ZONE_W - wC) / 2);
        drawClockBox(xC - 2, xC + wC + 1);
        drawStringRemapped(xC, SCORE_VY, cl, strlen(cl), Comic24, Y_ROW4);
    } else {
        char ss[4];
        snprintf(ss, sizeof(ss), "%d", clockSecs);
        int wSS = strPixelWidth(ss, strlen(ss));

        char tt[3];
        snprintf(tt, sizeof(tt), ".%d", clockTenths);
        dmd.selectFont(SystemFont5x7);
        int wTT = strPixelWidth(tt, strlen(tt));

        int totalW = wSS + wTT;
        int xC     = ZONE_CX + max(0, (ZONE_W - totalW) / 2);

        drawClockBox(xC - 2, xC + totalW + 1);
        drawStringRemapped(xC, SCORE_VY, ss, strlen(ss), Comic24, Y_ROW4);
        // ".t" subscript at visual bottom: VY_start = 32-7 = 25
        drawStringRemapped(xC + wSS, 32 - 7, tt, strlen(tt), SystemFont5x7, Y_ROW4);
    }
}

// ── Draw TOL zone (rows 5+6 combined, 32 px visual) ──────────────────────────
// Column 1 (x=0..47)  : "TOL" small font top + filled dots for timeoutsA
// Column 2 (x=48..95) : left-pointing triangle if possession=='A'
// Column 3 (x=96..143): right-pointing triangle if possession=='B'
// Column 4 (x=144..191): "TOL" + dots for timeoutsB
void drawTolZone(int timeoutsA, char possession, int timeoutsB) {
    // Clear the entire rows 5+6 zone (dmd y=0..31)
    dmd.drawFilledBox(0, Y_ROW6, DISPLAY_W - 1, Y_ROW5 + 15, GRAPHICS_INVERSE);

    // ── "TOL" label at visual top (VY_start=0) for both columns ──────────────
    dmd.selectFont(SystemFont5x7);

    // Column 1 TOL label — centred in 48px
    {
        const char* lbl = "TOL";
        int w = strPixelWidth(lbl, 3);
        int x = max(0, (TOL_ZONE_W - w) / 2);
        drawStringRemapped(x, 0, lbl, 3, SystemFont5x7, Y_ROW6);
    }
    // Column 4 TOL label — centred in 48px
    {
        const char* lbl = "TOL";
        int w = strPixelWidth(lbl, 3);
        int x = TOL_COL4_X + max(0, (TOL_ZONE_W - w) / 2);
        drawStringRemapped(x, 0, lbl, 3, SystemFont5x7, Y_ROW6);
    }

    // ── Dots for timeoutsA (column 1, below TOL label) ───────────────────────
    // 5 dots max, each 4x4 px with 2px gap, starting VY=9 (below 7px label+1gap)
    {
        int dotSize   = 4;
        int dotGap    = 2;
        int totalDotsW = 5 * dotSize + 4 * dotGap;   // 28 px
        int dotX0     = max(0, (TOL_ZONE_W - totalDotsW) / 2);
        int dotVY0    = 9;   // just below "TOL" (7px) + 1px gap

        for (int d = 0; d < 5; d++) {
            int dx = dotX0 + d * (dotSize + dotGap);
            bool filled = (d < timeoutsA);
            for (int ry = 0; ry < dotSize; ry++) {
                for (int rx = 0; rx < dotSize; rx++) {
                    uint8_t val;
                    if (filled) {
                        val = 1;
                    } else {
                        // hollow: only border pixels
                        val = (ry == 0 || ry == dotSize-1 ||
                               rx == 0 || rx == dotSize-1) ? 1 : 0;
                    }
                    writePixelRemapped(dx + rx, dotVY0 + ry, Y_ROW6, val);
                }
            }
        }
    }

    // ── Dots for timeoutsB (column 4, same layout) ───────────────────────────
    {
        int dotSize   = 4;
        int dotGap    = 2;
        int totalDotsW = 5 * dotSize + 4 * dotGap;
        int dotX0     = TOL_COL4_X + max(0, (TOL_ZONE_W - totalDotsW) / 2);
        int dotVY0    = 9;

        for (int d = 0; d < 5; d++) {
            int dx = dotX0 + d * (dotSize + dotGap);
            bool filled = (d < timeoutsB);
            for (int ry = 0; ry < dotSize; ry++) {
                for (int rx = 0; rx < dotSize; rx++) {
                    uint8_t val;
                    if (filled) {
                        val = 1;
                    } else {
                        val = (ry == 0 || ry == dotSize-1 ||
                               rx == 0 || rx == dotSize-1) ? 1 : 0;
                    }
                    writePixelRemapped(dx + rx, dotVY0 + ry, Y_ROW6, val);
                }
            }
        }
    }

    // ── Possession triangles ──────────────────────────────────────────────────
    // Left-pointing triangle (Team A) centred in column 2 (x=48..95)
    // Points left: apex at left, base at right — 16 wide, 17 tall
    if (possession == 'A') {
        int triH   = 17;   // height (odd for clean apex)
        int triW   = 16;
        int triX   = TOL_COL2_X + (TOL_ZONE_W - triW) / 2;
        int triVY0 = (32 - triH) / 2;   // vertically centred in 32px zone

        for (int row = 0; row < triH; row++) {
            // Distance from centre row determines how wide this row is
            int half   = triH / 2;
            int distC  = abs(row - half);          // 0 at apex-side tip end
            // For left-pointing: full width at row=half (centre), narrows toward row=0 and row=triH-1
            int rowW   = triW - (distC * triW / half);
            if (rowW <= 0) rowW = 1;
            int rowX   = triX + (triW - rowW);     // apex at left edge
            for (int c = 0; c < rowW; c++) {
                writePixelRemapped(rowX + c, triVY0 + row, Y_ROW6, 1);
            }
        }
    }

    // Right-pointing triangle (Team B) centred in column 3 (x=96..143)
    if (possession == 'B') {
        int triH   = 17;
        int triW   = 16;
        int triX   = TOL_COL3_X + (TOL_ZONE_W - triW) / 2;
        int triVY0 = (32 - triH) / 2;

        for (int row = 0; row < triH; row++) {
            int half  = triH / 2;
            int distC = abs(row - half);
            int rowW  = triW - (distC * triW / half);
            if (rowW <= 0) rowW = 1;
            // apex at right edge
            for (int c = 0; c < rowW; c++) {
                writePixelRemapped(triX + c, triVY0 + row, Y_ROW6, 1);
            }
        }
    }
}

// ── Check for new data, redraw changed sections ───────────────────────────────
void checkNewData() {
    if (!newData) return;
    newData = false;

    bool eventChanged = (strncmp(eventText, rxBuf.eventName, 32) != 0);
    bool teamsChanged = (strncmp(teamAText, rxBuf.teamA,     15) != 0) ||
                        (strncmp(teamBText, rxBuf.teamB,     15) != 0);
    bool scoreChanged = (rxBuf.scoreA != lastScoreA) ||
                        (rxBuf.scoreB != lastScoreB);
    bool clockChanged = (rxBuf.clockSecs != lastClockSecs) ||
                        (rxBuf.clockSecs < 60 && rxBuf.clockTenths != lastClockTenths);
    bool tolChanged   = (rxBuf.timeoutsA  != lastTimeoutsA)  ||
                        (rxBuf.timeoutsB  != lastTimeoutsB)  ||
                        (rxBuf.possession != lastPossession);

    if (eventChanged) {
        strncpy(eventText, rxBuf.eventName, 32); eventText[32] = '\0';
        drawRow(eventText, Y_EVENT);
        Serial.print("Event: "); Serial.println(eventText);
    }
    if (teamsChanged) {
        strncpy(teamAText, rxBuf.teamA, 15); teamAText[15] = '\0';
        strncpy(teamBText, rxBuf.teamB, 15); teamBText[15] = '\0';
        char buf[35];
        snprintf(buf, sizeof(buf), "%s vs %s", teamAText, teamBText);
        drawRow(buf, Y_TEAMS);
        Serial.print("Teams: "); Serial.println(buf);
    }
    if (scoreChanged || clockChanged) {
        lastScoreA      = rxBuf.scoreA;
        lastScoreB      = rxBuf.scoreB;
        lastClockSecs   = rxBuf.clockSecs;
        lastClockTenths = rxBuf.clockTenths;
        drawScoreZone(rxBuf.scoreA, rxBuf.clockSecs, rxBuf.clockTenths, rxBuf.scoreB);
        Serial.printf("Score %d:%d  Clock %d:%02d.%d\n",
            rxBuf.scoreA, rxBuf.scoreB,
            rxBuf.clockSecs / 60, rxBuf.clockSecs % 60, rxBuf.clockTenths);
    }
    if (tolChanged) {
        lastTimeoutsA  = rxBuf.timeoutsA;
        lastTimeoutsB  = rxBuf.timeoutsB;
        lastPossession = rxBuf.possession;
        drawTolZone(rxBuf.timeoutsA, rxBuf.possession, rxBuf.timeoutsB);
        Serial.printf("TOL A=%d B=%d  Possession=%c\n",
            rxBuf.timeoutsA, rxBuf.timeoutsB, rxBuf.possession);
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    waitMs(500);

    dmd.clearScreen(true);
    drawRow("SLAVE1", Y_EVENT);
    drawRow("V1.2",   Y_TEAMS);
    drawScoreZone(0, 0, 0, 0);
    drawTolZone(0, ' ', 0);
    waitMs(1200);

    dmd.clearScreen(true);
    drawRow(eventText, Y_EVENT);
    char defTeam[35];
    snprintf(defTeam, sizeof(defTeam), "%s vs %s", teamAText, teamBText);
    drawRow(defTeam, Y_TEAMS);
    drawScoreZone(0, 600, 0, 0);   // 0 — 10:00 — 0
    drawTolZone(0, ' ', 0);

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());

    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init FAILED");
        dmd.clearScreen(true);
        drawRow("ERR", Y_EVENT);
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 1 v1.2 ready");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();
    checkNewData();
}
