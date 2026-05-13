/*
  Slave 1 v1.3 — 6-Row Static Display
  ─────────────────────────────────────────────────────────────────────────
  Hardware : 6 panels wide × 6 tall  (192 × 96 px)

  Physical row 1 (y=80..95) : Event name       — auto-size, centred
  Physical row 2 (y=64..79) : TeamA vs TeamB   — auto-size, centred
  Physical rows 3+4 combined : ScoreA | Clock | ScoreB  — Comic24 (29 px)
  Physical rows 5+6 combined : Timeout-A | Arrow-A | Arrow-B | Timeout-B

  Rows 5+6 column layout  (first 128 px used, last 64 px blank):
    x=  0.. 31 (32 px) : "TOL" label + 5 dots — Team A
    x= 32.. 63 (32 px) : left-pointing  ◄ if possession=='A'
    x= 64.. 95 (32 px) : right-pointing ► if possession=='B'
    x= 96..127 (32 px) : "TOL" label + 5 dots — Team B
    x=128..191 (64 px) : blank

  Cross-row remap (same rule for both score zone and TOL zone):
    VY  0..15 → dmd_y = baseY + VY + 16   (upper physical row of the pair)
    VY 16..31 → dmd_y = baseY + VY - 16   (lower physical row of the pair)
  Score zone: baseY = Y_ROW4 = 32
  TOL zone  : baseY = Y_ROW6 = 0

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

// Physical y-starts  (y=0 = bottom-most physical row)
#define Y_EVENT  80
#define Y_TEAMS  64
#define Y_ROW3   48
#define Y_ROW4   32
#define Y_ROW5   16
#define Y_ROW6    0

// Score zone — three 64-px columns
#define ZONE_W    64
#define ZONE_CX   64
#define ZONE_BX  128
#define SCORE_VY   1

// Rows 5+6 have only 4 panels (128px) occupying DMD x=64..191
#define TOL_A_X   64   
#define TOL_A_W   32
#define ARR_A_X   96   
#define ARR_B_X  128   
#define ARR_W     32
#define TOL_B_X  160   
#define TOL_B_W   32

// Dot parameters — Perfect 5x5 Circles
#define DOT_W      5
#define DOT_H      5
#define DOT_GAP    1
#define DOT_MAX    5
#define DOT_ROW_W  (DOT_MAX * DOT_W + (DOT_MAX - 1) * DOT_GAP)   // 29 px wide
#define DOT_VY    23   // Centred in the lower physical panel


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

BoardData    rxBuf;
volatile bool newData = false;

char eventText[33]  = "WAITING";
char teamAText[16]  = "TEAM A";
char teamBText[16]  = "TEAM B";
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

// ── String pixel-width helper ─────────────────────────────────────────────────
int strPixelWidth(const char* s, int len) {
    int w = 0;
    for (int i = 0; i < len; i++) {
        int cw = dmd.charWidth((unsigned char)s[i]);
        if (cw > 0) w += cw + 1;
    }
    return w;
}

// ── Draw one 16-px row: auto-size font, centred ───────────────────────────────
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

// ── Cross-row pixel helper (HARD-SHIFTED TO 0..127) ───────────────────────────
static void writePixelRemapped(int x, int VY, int baseY, uint8_t val) {
    int dmd_y = (VY < 16) ? (baseY + VY + 16) : (baseY + VY - 16);
    int dmd_x = x;

    // TARGETED TEST: Force 6th row graphics to map exactly to 0 - 127
    if (baseY == Y_ROW6 && VY >= 16) {
        dmd_x = x - 64; 
    }

    if (dmd_y < 0 || dmd_y >= DISPLAYS_DOWN * 16) return;
    if (dmd_x < 0 || dmd_x >= DISPLAY_W) return; // Prevent out-of-bounds drawing

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

void drawStringRemapped(int x, int VY_start, const char* s, int len,
                        const uint8_t* fnt, int baseY) {
    int cx = x;
    for (int i = 0; i < len; i++) {
        int w = drawCharRemapped(cx, VY_start, (unsigned char)s[i], fnt, baseY);
        if (w > 0) cx += w + 1;
    }
}

// ── Clock border box ──────────────────────────────────────────────────────────
void drawClockBox(int x1, int x2) {
    dmd.drawLine(x1, Y_ROW3,      x2, Y_ROW3,      GRAPHICS_NORMAL);
    dmd.drawLine(x1, Y_ROW4 + 15, x2, Y_ROW4 + 15, GRAPHICS_NORMAL);
    dmd.drawLine(x1, Y_ROW4,      x1, Y_ROW3 + 15,  GRAPHICS_NORMAL);
    dmd.drawLine(x2, Y_ROW4,      x2, Y_ROW3 + 15,  GRAPHICS_NORMAL);
}

// ── Score zone (rows 3+4, 32 px visual) ──────────────────────────────────────
void drawScoreZone(int scoreA, int clockSecs, int clockTenths, int scoreB) {
    dmd.drawFilledBox(0, Y_ROW4, DISPLAY_W - 1, Y_ROW3 + 15, GRAPHICS_INVERSE);

    dmd.selectFont(Comic24);
    char sa[5]; snprintf(sa, sizeof(sa), "%d", scoreA);
    int wA = strPixelWidth(sa, strlen(sa));
    drawStringRemapped(max(0, (ZONE_W - wA) / 2), SCORE_VY, sa, strlen(sa), Comic24, Y_ROW4);

    dmd.selectFont(Comic24);
    char sb[5]; snprintf(sb, sizeof(sb), "%d", scoreB);
    int wB = strPixelWidth(sb, strlen(sb));
    drawStringRemapped(ZONE_BX + max(0, (ZONE_W - wB) / 2), SCORE_VY, sb, strlen(sb), Comic24, Y_ROW4);

    dmd.selectFont(Comic24);
    if (clockSecs >= 60) {
        int m = clockSecs / 60, s = clockSecs % 60;
        char cl[7]; snprintf(cl, sizeof(cl), "%d:%02d", m, s);
        int wC = strPixelWidth(cl, strlen(cl));
        int xC = ZONE_CX + max(0, (ZONE_W - wC) / 2);
        drawClockBox(xC - 2, xC + wC + 1);
        drawStringRemapped(xC, SCORE_VY, cl, strlen(cl), Comic24, Y_ROW4);
    } else {
        char ss[4]; snprintf(ss, sizeof(ss), "%d", clockSecs);
        int wSS = strPixelWidth(ss, strlen(ss));
        char tt[3]; snprintf(tt, sizeof(tt), ".%d", clockTenths);
        dmd.selectFont(SystemFont5x7);
        int wTT = strPixelWidth(tt, strlen(tt));
        int totalW = wSS + wTT;
        int xC = ZONE_CX + max(0, (ZONE_W - totalW) / 2);
        drawClockBox(xC - 2, xC + totalW + 1);
        drawStringRemapped(xC, SCORE_VY, ss, strlen(ss), Comic24, Y_ROW4);
        drawStringRemapped(xC + wSS, 32 - 7, tt, strlen(tt), SystemFont5x7, Y_ROW4);
    }
}

// ── Draw one timeout dot (5x5 circular shape) ────────────────────────────────
static void drawDot(int x, int VY, bool filled) {
    for (int ry = 0; ry < DOT_H; ry++) {
        for (int rx = 0; rx < DOT_W; rx++) {
            uint8_t val = 0; 
            
            if (filled) {
                // Solid circle (shave off the 4 corners)
                if (!((ry==0 && rx==0) || (ry==0 && rx==4) || 
                      (ry==4 && rx==0) || (ry==4 && rx==4))) {
                    val = 1;
                }
            } else {
                // Hollow circle ring
                if ((ry==0 && rx>0 && rx<4) || 
                    (ry==4 && rx>0 && rx<4) || 
                    (rx==0 && ry>0 && ry<4) || 
                    (rx==4 && ry>0 && ry<4)) {
                    val = 1;
                }
            }
            writePixelRemapped(x + rx, VY + ry, Y_ROW6, val);
        }
    }
}

// ── TOL zone (rows 5+6) — perfectly aligned layout math ─────────────────────
void drawTolZone(int timeoutsA, char possession, int timeoutsB) {
    dmd.drawFilledBox(0, Y_ROW6, DISPLAY_W - 1, Y_ROW5 + 15, GRAPHICS_INVERSE);

    dmd.selectFont(SystemFont5x7);

    // ── TOL label + 5 dots — Team A ──────────────────────────────────────────
    {
        const char* lbl = "TOL";
        int w  = strPixelWidth(lbl, 3);
        int x  = TOL_A_X + max(0, (TOL_A_W - w) / 2);
        drawStringRemapped(x, 4, lbl, 3, SystemFont5x7, Y_ROW6);
        
        int x0 = TOL_A_X + max(0, (TOL_A_W - DOT_ROW_W) / 2);
        for (int d = 0; d < DOT_MAX; d++)
            drawDot(x0 + d * (DOT_W + DOT_GAP), DOT_VY, d < timeoutsA);
    }

    // ── TOL label + 5 dots — Team B ──────────────────────────────────────────
    {
        const char* lbl = "TOL";
        int w  = strPixelWidth(lbl, 3);
        int x  = TOL_B_X + max(0, (TOL_B_W - w) / 2);
        drawStringRemapped(x, 4, lbl, 3, SystemFont5x7, Y_ROW6);
        
        int x0 = TOL_B_X + max(0, (TOL_B_W - DOT_ROW_W) / 2);
        for (int d = 0; d < DOT_MAX; d++)
            drawDot(x0 + d * (DOT_W + DOT_GAP), DOT_VY, d < timeoutsB);
    }

    // ── ◄ Team A arrow — FULL Solid Triangle ─────────────────────────────────
    if (possession == 'A') {
        for (int vy = 3; vy <= 27; vy++) {
            int rowW = 24 - (abs(vy - 15) * 2);
            if (rowW < 1) rowW = 1;
            int startX = 124 - rowW; // Base is drawn at the right (124), pointing left
            for (int c = 0; c < rowW; c++) {
                writePixelRemapped(startX + c, vy, Y_ROW6, 1);
            }
        }
    }

    // ── ► Team B arrow — FULL Solid Triangle ─────────────────────────────────
    if (possession == 'B') {
        for (int vy = 3; vy <= 27; vy++) {
            int rowW = 24 - (abs(vy - 15) * 2);
            if (rowW < 1) rowW = 1;
            int startX = 132; // Base is drawn at the left (132), pointing right
            for (int c = 0; c < rowW; c++) {
                writePixelRemapped(startX + c, vy, Y_ROW6, 1);
            }
        }
    }
}

// ── Check for new data ────────────────────────────────────────────────────────
void checkNewData() {
    if (!newData) return;
    newData = false;

    bool eventChanged = (strncmp(eventText, rxBuf.eventName, 32) != 0);
    bool teamsChanged = (strncmp(teamAText, rxBuf.teamA,     15) != 0) ||
                        (strncmp(teamBText, rxBuf.teamB,     15) != 0);
    bool scoreChanged = (rxBuf.scoreA != lastScoreA) || (rxBuf.scoreB != lastScoreB);
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
        Serial.printf("TOL A=%d B=%d  Poss=%c\n",
            rxBuf.timeoutsA, rxBuf.timeoutsB, rxBuf.possession);
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    waitMs(500);

    dmd.clearScreen(true);
    drawRow("SLAVE1", Y_EVENT);
    drawRow("V1.3",   Y_TEAMS);
    drawScoreZone(0, 0, 0, 0);
    drawTolZone(0, ' ', 0);
    waitMs(1200);

    dmd.clearScreen(true);
    drawRow(eventText, Y_EVENT);
    char defTeam[35];
    snprintf(defTeam, sizeof(defTeam), "%s vs %s", teamAText, teamBText);
    drawRow(defTeam, Y_TEAMS);
    drawScoreZone(0, 600, 0, 0);
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
        Serial.println("Slave 1 v1.3 ready");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();
    checkNewData();
}