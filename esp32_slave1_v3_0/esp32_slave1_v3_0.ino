/*
  Main Scoreboard  v3.0  —  6×6 panels  (192 × 96 px)
  ─────────────────────────────────────────────────────────────────────────
  Row layout (y=0 is bottom-most physical row):
    y=80..95  Row 1 : Event name   — SystemFont5x7 (single-pixel, thin)
    y=64..79  Row 2 : TeamA left  |  TeamB right  (right-aligned)
    y=32..63  Rows 3+4 (32px) : ScoreA | Game Clock | ScoreB
    y=0..31   Rows 5+6 (32px) : TOL-A  | Arrow      | TOL-B

  Changes vs v2.0:
    • Event name: always SystemFont5x7 (single-pixel strokes), scrolls when
      eventScroll=1 or text too wide.
    • Team names: right-aligned in their zones.
    • Game clock: slave-side local timer, syncs to master every packet.
    • Tenths at ½ size (12px) when clockSecs < 60 (last minute).
    • No "Slave" word in startup message.
    • BoardData includes marketingText field.
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

#define Y_EVENT  80
#define Y_TEAMS  64
#define Y_ROW3   48
#define Y_ROW4   32
#define Y_ROW5   16
#define Y_ROW6    0

#define ZONE_W   64
#define ZONE_CX  64
#define ZONE_BX 128

#define TOL_A_X   64
#define TOL_A_W   32
#define ARR_A_X   96
#define ARR_B_X  128
#define ARR_W     32
#define TOL_B_X  160
#define TOL_B_W   32

#define DOT_W      7
#define DOT_H      7
#define DOT_GAP    2
#define DOT_MAX    3
#define DOT_ROW_W  (DOT_MAX * DOT_W + (DOT_MAX - 1) * DOT_GAP)
#define DOT_VY    20

DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);

unsigned long lastScanUs = 0;
void scanIfNeeded() {
    if ((long)(micros() - lastScanUs) >= 300) {
        dmd.scanDisplayBySPI();
        lastScanUs = micros();
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
    char marketingText[32];
} BoardData;

BoardData    rxBuf;
volatile bool newData = false;

char eventText[33] = "WAITING";
char teamAText[16] = "TEAM A";
char teamBText[16] = "TEAM B";
int  lastScoreA = -1, lastScoreB = -1;
int  lastClockSecs = -1, lastClockTenths = -1;
int  lastTimeoutsA = -1, lastTimeoutsB = -1;
char lastPossession = ' ';

// ── Slave-side game clock ─────────────────────────────────────────────────────
int  localClockSecs    = 0;
int  localClockTenths  = 0;
bool localClockRunning = false;
unsigned long lastTickMs = 0;

void syncClock(int ms, int mt, int mr) {
    if (mr && !localClockRunning) {
        localClockSecs = ms; localClockTenths = mt;
        localClockRunning = true; lastTickMs = millis();
    } else if (!mr && localClockRunning) {
        localClockRunning = false;
        localClockSecs = ms; localClockTenths = mt;
    } else if (localClockRunning) {
        int masterTotal = ms * 10 + mt;
        int localTotal  = localClockSecs * 10 + localClockTenths;
        if (abs(masterTotal - localTotal) > 1) {
            localClockSecs = ms; localClockTenths = mt;
        }
    } else {
        localClockSecs = ms; localClockTenths = mt;
    }
}

void tickClock() {
    if (!localClockRunning) return;
    unsigned long now = millis();
    if ((long)(now - lastTickMs) < 100) return;
    lastTickMs = now;
    localClockTenths--;
    if (localClockTenths < 0) {
        localClockTenths = 9;
        localClockSecs--;
        if (localClockSecs < 0) {
            localClockSecs = 0; localClockTenths = 0;
            localClockRunning = false;
        }
    }
}

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == 1 && data[0] == 0xAA) { ESP.restart(); return; }
    if (len == sizeof(BoardData)) { memcpy(&rxBuf, data, sizeof(rxBuf)); newData = true; }
}

// ── String pixel-width ────────────────────────────────────────────────────────
int strPixelWidth(const char* s, int len) {
    int w = 0;
    for (int i = 0; i < len; i++) {
        int cw = dmd.charWidth((unsigned char)s[i]);
        if (cw > 0) w += cw + 1;
    }
    return w;
}

// ── Event name row — always SystemFont5x7 (single-pixel thin) ─────────────────
// Scrolling handled via scrollX offset when eventScroll=1 or text too wide
int  scrollX        = 0;
bool scrollEnabled  = false;
int  eventTextPixW  = 0;
unsigned long lastScrollUs = 0;

void drawEventRow() {
    dmd.selectFont(SystemFont5x7);
    eventTextPixW = strPixelWidth(eventText, strlen(eventText));
    scrollEnabled = (rxBuf.eventScroll == 1) || (eventTextPixW > DISPLAY_W);

    dmd.drawFilledBox(0, Y_EVENT, DISPLAY_W - 1, Y_EVENT + 15, GRAPHICS_INVERSE);
    if (!scrollEnabled) {
        int drawX = max(0, (DISPLAY_W - eventTextPixW) / 2);
        dmd.drawString(drawX, Y_EVENT + 4, eventText, strlen(eventText), GRAPHICS_NORMAL);
    }
    // Scrolling handled in loop()
}

void stepEventScroll() {
    if (!scrollEnabled) return;
    unsigned long now = micros();
    if ((long)(now - lastScrollUs) < 40000) return; // ~25 px/s
    lastScrollUs = now;

    dmd.selectFont(SystemFont5x7);
    dmd.drawFilledBox(0, Y_EVENT, DISPLAY_W - 1, Y_EVENT + 15, GRAPHICS_INVERSE);
    dmd.drawString(scrollX, Y_EVENT + 4, eventText, strlen(eventText), GRAPHICS_NORMAL);

    scrollX--;
    if (scrollX < -(int)eventTextPixW) scrollX = DISPLAY_W;
}

// ── Team names — right-aligned in their zone ─────────────────────────────────
void drawTeamNames(const char* teamA, const char* teamB) {
    dmd.selectFont(Arial_Black_16);

    // Team A — right-aligned in x=0..63
    {
        int len = strlen(teamA);
        while (len > 0 && strPixelWidth(teamA, len) > ZONE_W) len--;
        int textW = strPixelWidth(teamA, len);
        int drawX = max(0, ZONE_W - textW - 1); // right-aligned
        dmd.drawFilledBox(0, Y_TEAMS, ZONE_BX - 1, Y_TEAMS + 15, GRAPHICS_INVERSE);
        dmd.drawString(drawX, Y_TEAMS, teamA, len, GRAPHICS_NORMAL);
    }
    // Team B — right-aligned in x=128..191
    {
        int len = strlen(teamB);
        while (len > 0 && strPixelWidth(teamB, len) > ZONE_W) len--;
        int textW = strPixelWidth(teamB, len);
        int drawX = max(ZONE_BX, ZONE_BX + ZONE_W - textW - 1); // right-aligned
        dmd.drawFilledBox(ZONE_BX, Y_TEAMS, DISPLAY_W - 1, Y_TEAMS + 15, GRAPHICS_INVERSE);
        dmd.drawString(drawX, Y_TEAMS, teamB, len, GRAPHICS_NORMAL);
    }
}

// ── Cross-row pixel helper ────────────────────────────────────────────────────
static void writePixelRemapped(int x, int VY, int baseY, uint8_t val) {
    int dmd_y = (VY < 16) ? (baseY + VY + 16) : (baseY + VY - 16);
    int dmd_x = x;
    if (baseY == Y_ROW6 && VY >= 16) dmd_x = x - 64;
    if (dmd_y < 0 || dmd_y >= DISPLAYS_DOWN * 16) return;
    if (dmd_x < 0 || dmd_x >= DISPLAY_W) return;
    dmd.writePixel(dmd_x, dmd_y, GRAPHICS_NORMAL, val);
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
    unsigned char c = letter - firstChar;
    uint8_t bytes = (height + 7) / 8;
    uint16_t index = 0;
    uint8_t width;
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
            uint8_t data = pgm_read_byte(fnt + index + j + (bi * width));
            int offset = (int)bi * 8;
            if (bi == bytes - 1 && bytes > 1) offset = height - 8;
            for (uint8_t k = 0; k < 8; k++) {
                int row = offset + k;
                if (row < (int)bi * 8 || row > (int)height) continue;
                writePixelRemapped(bX + j, VY_start + row, baseY, (data & (1 << k)) ? 1 : 0);
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
    unsigned char c = letter - firstChar;
    uint8_t bytes = (height + 7) / 8;
    uint16_t index = 0;
    uint8_t width;
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

// ── Score zone (rows 3+4, 32px) ───────────────────────────────────────────────
void drawScoreZone(int scoreA, int clockSecs, int clockTenths, int scoreB) {
    dmd.drawFilledBox(0, Y_ROW4, DISPLAY_W - 1, Y_ROW3 + 15, GRAPHICS_INVERSE);

    dmd.selectFont(Comic24);
    char sa[6]; snprintf(sa, sizeof(sa), "%d", scoreA);
    int wA = strPixelWidth(sa, strlen(sa));
    drawStringScaledRemapped(max(0, (ZONE_W - wA) / 2), 0, sa, strlen(sa), Comic24, Y_ROW4, 24, 32);

    dmd.selectFont(Comic24);
    char sb[6]; snprintf(sb, sizeof(sb), "%d", scoreB);
    int wB = strPixelWidth(sb, strlen(sb));
    drawStringScaledRemapped(ZONE_BX + max(0, (ZONE_W - wB) / 2), 0, sb, strlen(sb), Comic24, Y_ROW4, 24, 32);

    if (clockSecs >= 60) {
        int m = clockSecs / 60, s = clockSecs % 60;
        char cl[7]; snprintf(cl, sizeof(cl), "%d:%02d", m, s);
        dmd.selectFont(Comic24);
        int wC = strPixelWidth(cl, strlen(cl));
        int xC = ZONE_CX + max(0, (ZONE_W - wC) / 2);
        drawStringRemapped(xC, 0, cl, strlen(cl), Comic24, Y_ROW4);
    } else {
        // Last minute: seconds full size, tenths at ½ size (12px), bottom-aligned
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

// ── Timeout dot ───────────────────────────────────────────────────────────────
static void drawDot(int x, int VY) {
    for (int ry = 0; ry < DOT_H; ry++) {
        for (int rx = 0; rx < DOT_W; rx++) {
            if ((ry == 0 || ry == DOT_H - 1) && (rx == 0 || rx == DOT_W - 1)) continue;
            writePixelRemapped(x + rx, VY + ry, Y_ROW6, 1);
        }
    }
}

// ── TOL zone (rows 5+6) ───────────────────────────────────────────────────────
void drawTolZone(int timeoutsA, char possession, int timeoutsB) {
    dmd.drawFilledBox(0, Y_ROW6, DISPLAY_W - 1, Y_ROW5 + 15, GRAPHICS_INVERSE);
    dmd.selectFont(SystemFont5x7);

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

    // Possession arrows — slightly reduced (max rowW=18 instead of 24)
    if (possession == 'A') {
        for (int vy = 5; vy <= 25; vy++) {
            int rowW = 18 - (abs(vy - 15) * 2);
            if (rowW < 1) rowW = 1;
            int startX = 124 - rowW;
            for (int c = 0; c < rowW; c++) writePixelRemapped(startX + c, vy, Y_ROW6, 1);
        }
    }
    if (possession == 'B') {
        for (int vy = 5; vy <= 25; vy++) {
            int rowW = 18 - (abs(vy - 15) * 2);
            if (rowW < 1) rowW = 1;
            for (int c = 0; c < rowW; c++) writePixelRemapped(132 + c, vy, Y_ROW6, 1);
        }
    }
}

// ── Process received data ─────────────────────────────────────────────────────
void checkNewData() {
    if (!newData) return;
    newData = false;

    bool eventChanged = (strncmp(eventText, rxBuf.eventName, 32) != 0);
    bool teamsChanged = (strncmp(teamAText, rxBuf.teamA, 15) != 0) ||
                        (strncmp(teamBText, rxBuf.teamB, 15) != 0);
    bool tolChanged   = (rxBuf.timeoutsA != lastTimeoutsA) ||
                        (rxBuf.timeoutsB != lastTimeoutsB) ||
                        (rxBuf.possession != lastPossession);

    syncClock(rxBuf.clockSecs, rxBuf.clockTenths, rxBuf.clockRunning);

    if (eventChanged) {
        strncpy(eventText, rxBuf.eventName, 32); eventText[32] = '\0';
        scrollX = DISPLAY_W;
        drawEventRow();
    }
    if (teamsChanged) {
        strncpy(teamAText, rxBuf.teamA, 15); teamAText[15] = '\0';
        strncpy(teamBText, rxBuf.teamB, 15); teamBText[15] = '\0';
        drawTeamNames(teamAText, teamBText);
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
    drawEventRow();
    drawTeamNames("TEAM A", "TEAM B");
    drawScoreZone(0, 600, 0, 0);
    drawTolZone(0, ' ', 0);
    waitMs(1200);
    dmd.clearScreen(true);
    drawEventRow();
    drawTeamNames(teamAText, teamBText);
    drawScoreZone(0, 600, 0, 0);
    drawTolZone(0, ' ', 0);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("Main board MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
        dmd.clearScreen(true); dmd.drawString(0, 0, "ERR", 3, GRAPHICS_NORMAL);
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Main board v3.0 ready");
    }
}

void loop() {
    scanIfNeeded();
    tickClock();
    stepEventScroll();
    checkNewData();

    // Redraw score zone from local clock every tick
    if (localClockSecs != lastClockSecs ||
        (localClockSecs < 60 && localClockTenths != lastClockTenths)) {
        lastClockSecs   = localClockSecs;
        lastClockTenths = localClockTenths;
        drawScoreZone(rxBuf.scoreA, localClockSecs, localClockTenths, rxBuf.scoreB);
    }
    // Redraw score if changed
    if (rxBuf.scoreA != lastScoreA || rxBuf.scoreB != lastScoreB) {
        lastScoreA = rxBuf.scoreA;
        lastScoreB = rxBuf.scoreB;
        drawScoreZone(rxBuf.scoreA, localClockSecs, localClockTenths, rxBuf.scoreB);
    }
}
