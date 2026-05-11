/*
  Slave 1 v1.1 — 4-Row Static Display
  ─────────────────────────────────────────────────────────────────────────
  Hardware : 6 panels wide × 4 tall  (192 × 64 px)

  DMD32 y=0 = bottom-most physical panel row (same as 2-panel version).

  Physical row 1 — top    (y=48..63) : Event name       — auto-size, centred
  Physical row 2          (y=32..47) : TeamA vs TeamB   — auto-size, centred
  Physical row 3          (y=16..31) : ScoreA | [Clock] | ScoreB  Arial_Black_16
  Physical row 4 — bot    (y= 0..15) : Tenths subscript when clock < 60 s

  Clock box: thin border drawn around the time display.
  Last 60 s: clock column shows "SS" (Arial_Black_16) + ".t" (SystemFont5x7)
             as a subscript at the bottom-right of the clock area.

  No timer ISR — scanDisplayBySPI() polled via micros() from loop().
*/

#include <DMD32.h>
#include "fonts/Arial_black_16.h"
#include "fonts/Arial14.h"
#include "fonts/SystemFont5x7.h"
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define DISPLAYS_ACROSS 6
#define DISPLAYS_DOWN   4
#define DISPLAY_W       (32 * DISPLAYS_ACROSS)   // 192 px

// Physical y-starts  (DISPLAYS_DOWN=4 → y=0 is bottom-most physical row)
#define Y_EVENT   48   // physical row 1 — top    16 px
#define Y_TEAMS   32   // physical row 2 — second 16 px
#define Y_ROW3    16   // physical row 3 — score row   (ScoreA | Clock | ScoreB)
#define Y_ROW4     0   // physical row 4 — tenths row  (dark normally)

// Three equal 64-px column zones across 192 px
#define ZONE_W    64
#define ZONE_CX   64   // clock zone left  edge
#define ZONE_BX  128   // score-B zone left edge

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

// ── Draw one 16-px row: auto-size font, centre horizontally & vertically ──────
void drawRow(const char* text, int y0) {
    int len = strlen(text);

    const uint8_t* font  = Arial_Black_16;
    int            fontH = 16;

    dmd.selectFont(Arial_Black_16);
    if (strPixelWidth(text, len) > DISPLAY_W) {
        dmd.selectFont(Arial_14);
        font  = Arial_14;
        fontH = 14;
        if (strPixelWidth(text, len) > DISPLAY_W) {
            dmd.selectFont(SystemFont5x7);
            font  = SystemFont5x7;
            fontH = 7;
        }
    }

    int textW = strPixelWidth(text, len);
    int drawX = max(0, (DISPLAY_W - textW) / 2);
    int drawY = y0 + (16 - fontH) / 2;

    dmd.drawFilledBox(0, y0, DISPLAY_W - 1, y0 + 15, GRAPHICS_INVERSE);
    dmd.selectFont(font);
    dmd.drawString(drawX, drawY, text, len, GRAPHICS_NORMAL);
}

// ── Draw score zone (physical rows 3 + 4) ─────────────────────────────────────
//
//  Row 3 (y=16..31): [ScoreA]  [  Clock  ]  [ScoreB]
//                               +---------+
//                               | box border around clock |
//                               +---------+
//  Row 4 (y=0..15):  dark normally.
//                    When clockSecs < 60: tenths ".t" appears as subscript
//                    in the clock column at the bottom of row 4.
//
void drawScoreZone(int scoreA, int clockSecs, int clockTenths, int scoreB) {
    // Clear both rows 3 and 4
    dmd.drawFilledBox(0, Y_ROW4, DISPLAY_W - 1, Y_ROW3 + 15, GRAPHICS_INVERSE);

    // ── Row 3: scores and clock ───────────────────────────────────────────────
    // Score A — left zone (x = 0..63)
    dmd.selectFont(Arial_Black_16);
    char sa[5];
    snprintf(sa, sizeof(sa), "%d", scoreA);
    int wA = strPixelWidth(sa, strlen(sa));
    int xA = max(0, (ZONE_W - wA) / 2);
    dmd.drawString(xA, Y_ROW3, sa, strlen(sa), GRAPHICS_NORMAL);

    // Score B — right zone (x = 128..191)
    dmd.selectFont(Arial_Black_16);
    char sb[5];
    snprintf(sb, sizeof(sb), "%d", scoreB);
    int wB = strPixelWidth(sb, strlen(sb));
    int xB = ZONE_BX + max(0, (ZONE_W - wB) / 2);
    dmd.drawString(xB, Y_ROW3, sb, strlen(sb), GRAPHICS_NORMAL);

    // Clock — centre zone (x = 64..127)
    if (clockSecs >= 60) {
        // ── Normal time: M:SS in Arial_Black_16 ──────────────────────────────
        int m = clockSecs / 60, s = clockSecs % 60;
        char cl[7];
        snprintf(cl, sizeof(cl), "%d:%02d", m, s);
        dmd.selectFont(Arial_Black_16);
        int wC = strPixelWidth(cl, strlen(cl));
        int xC = ZONE_CX + max(0, (ZONE_W - wC) / 2);

        // Outer box around clock text (tight, 2 px margin left/right)
        dmd.drawBox(xC - 2, Y_ROW3, xC + wC + 1, Y_ROW3 + 15, GRAPHICS_NORMAL);
        dmd.drawString(xC, Y_ROW3, cl, strlen(cl), GRAPHICS_NORMAL);

    } else {
        // ── Last 60 s: "SS" big + ".t" subscript ─────────────────────────────
        char ss[4];
        snprintf(ss, sizeof(ss), "%d", clockSecs);
        dmd.selectFont(Arial_Black_16);
        int wSS = strPixelWidth(ss, strlen(ss));

        char tt[3];
        snprintf(tt, sizeof(tt), ".%d", clockTenths);
        dmd.selectFont(SystemFont5x7);
        int wTT = strPixelWidth(tt, strlen(tt));

        int totalW = wSS + wTT;
        int xC     = ZONE_CX + max(0, (ZONE_W - totalW) / 2);

        // Box around the combined "SS.t" display
        dmd.drawBox(xC - 2, Y_ROW3, xC + totalW + 1, Y_ROW3 + 15, GRAPHICS_NORMAL);

        // "SS" in full 16-px height
        dmd.selectFont(Arial_Black_16);
        dmd.drawString(xC, Y_ROW3, ss, strlen(ss), GRAPHICS_NORMAL);

        // ".t" subscript: SystemFont5x7 (7 px) pinned to bottom of row 3
        // yDraw = Y_ROW3 + (16 - 7) = Y_ROW3 + 9
        dmd.selectFont(SystemFont5x7);
        dmd.drawString(xC + wSS, Y_ROW3 + 9, tt, strlen(tt), GRAPHICS_NORMAL);
    }
}

// ── Check for new data, redraw only changed sections ──────────────────────────
void checkNewData() {
    if (!newData) return;
    newData = false;

    bool eventChanged = (strncmp(eventText, rxBuf.eventName, 32) != 0);
    bool teamsChanged = (strncmp(teamAText, rxBuf.teamA,     15) != 0) ||
                        (strncmp(teamBText, rxBuf.teamB,     15) != 0);
    bool scoreChanged = (rxBuf.scoreA != lastScoreA) ||
                        (rxBuf.scoreB != lastScoreB);
    // Redraw clock every second; also every tenth when in last-60-s mode
    bool clockChanged = (rxBuf.clockSecs != lastClockSecs) ||
                        (rxBuf.clockSecs < 60 && rxBuf.clockTenths != lastClockTenths);

    if (eventChanged) {
        strncpy(eventText, rxBuf.eventName, 32);
        eventText[32] = '\0';
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
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    waitMs(500);

    // Startup splash
    dmd.clearScreen(true);
    drawRow("SLAVE1", Y_EVENT);
    drawRow("V1.1",   Y_TEAMS);
    drawScoreZone(0, 0, 0, 0);
    waitMs(1200);

    // Default display
    dmd.clearScreen(true);
    drawRow(eventText, Y_EVENT);
    char defTeam[35];
    snprintf(defTeam, sizeof(defTeam), "%s vs %s", teamAText, teamBText);
    drawRow(defTeam, Y_TEAMS);
    drawScoreZone(0, 600, 0, 0);   // 0 — 10:00 — 0

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
        Serial.println("Slave 1 v1.1 ready");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();
    checkNewData();
}
