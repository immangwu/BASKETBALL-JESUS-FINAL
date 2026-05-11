/*
  Slave 1 v1.1 — 4-Row Static Display
  ─────────────────────────────────────────────────────────────────────────
  Hardware : 6 panels wide × 4 tall  (192 × 64 px)

  Physical row 1 — top    (y=48..63) : Event name       — auto-size, centred
  Physical row 2          (y=32..47) : TeamA vs TeamB   — auto-size, centred
  Physical rows 3+4 — bot (y= 0..31) : ScoreA | Clock | ScoreB — Comic24 (29px)

  DMD32 with DISPLAYS_DOWN=4 maps y=0 to the bottom-most physical panel row.

  Clock mirrors master in real time — every S-packet (≈100 ms) updates
  clockSecs so the display changes exactly when the software clock ticks.

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
#define DISPLAYS_DOWN   4
#define DISPLAY_W       (32 * DISPLAYS_ACROSS)   // 192 px

// Physical y-starts  (y=0 = bottom-most panel with DISPLAYS_DOWN=4)
#define Y_EVENT  48   // physical row 1 — top 16 px
#define Y_TEAMS  32   // physical row 2 — second 16 px
#define Y_SCORE   0   // physical rows 3+4 — bottom 32 px zone

// Score zone column zones (192 px split into three equal 64 px bands)
#define ZONE_W   64
#define ZONE_B   128  // start of right-score zone

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
int  lastScoreA    = -1;
int  lastScoreB    = -1;
int  lastClockSecs = -1;

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ── String pixel-width (call after selectFont) ────────────────────────────────
int strPixelWidth(const char* s, int len) {
    int w = 0;
    for (int i = 0; i < len; i++) {
        int cw = dmd.charWidth((unsigned char)s[i]);
        if (cw > 0) w += cw + 1;
    }
    return w;
}

// ── Draw one 16-px row: auto-size font, centre horizontally & vertically ──────
// y0 = physical top pixel of the 16-px row
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

// ── Draw 32-px score zone: ScoreA | Clock (MM:SS) | ScoreB ───────────────────
// Uses Comic24 (29 px tall), centred vertically in the 32-px zone.
// Each of the three sections occupies an equal 64-px column.
void drawScoreZone(int scoreA, int clockSecs, int scoreB) {
    dmd.drawFilledBox(0, Y_SCORE, DISPLAY_W - 1, Y_SCORE + 31, GRAPHICS_INVERSE);

    dmd.selectFont(Comic24);
    const int fontH = 29;
    const int yDraw = Y_SCORE + (32 - fontH) / 2;   // vertical centre = Y_SCORE + 1

    // ── Score A (left zone x=0..63) ───────────────────────────────────────────
    char sa[5];
    snprintf(sa, sizeof(sa), "%d", scoreA);
    int wA = strPixelWidth(sa, strlen(sa));
    int xA = max(0, (ZONE_W - wA) / 2);
    dmd.drawString(xA, yDraw, sa, strlen(sa), GRAPHICS_NORMAL);

    // ── Clock (centre zone x=64..127) ─────────────────────────────────────────
    int m = clockSecs / 60;
    int s = clockSecs % 60;
    char cl[7];
    snprintf(cl, sizeof(cl), "%d:%02d", m, s);
    int wC = strPixelWidth(cl, strlen(cl));
    int xC = ZONE_W + max(0, (ZONE_W - wC) / 2);
    dmd.drawString(xC, yDraw, cl, strlen(cl), GRAPHICS_NORMAL);

    // ── Score B (right zone x=128..191) ───────────────────────────────────────
    char sb[5];
    snprintf(sb, sizeof(sb), "%d", scoreB);
    int wB = strPixelWidth(sb, strlen(sb));
    int xB = ZONE_B + max(0, (ZONE_W - wB) / 2);
    dmd.drawString(xB, yDraw, sb, strlen(sb), GRAPHICS_NORMAL);
}

// ── Check for new data and redraw only changed sections ───────────────────────
void checkNewData() {
    if (!newData) return;
    newData = false;

    bool eventChanged = (strncmp(eventText, rxBuf.eventName, 32) != 0);
    bool teamsChanged = (strncmp(teamAText, rxBuf.teamA,     15) != 0) ||
                        (strncmp(teamBText, rxBuf.teamB,     15) != 0);
    bool scoreChanged = (rxBuf.scoreA    != lastScoreA)    ||
                        (rxBuf.scoreB    != lastScoreB);
    bool clockChanged = (rxBuf.clockSecs != lastClockSecs);

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
        lastScoreA    = rxBuf.scoreA;
        lastScoreB    = rxBuf.scoreB;
        lastClockSecs = rxBuf.clockSecs;
        drawScoreZone(rxBuf.scoreA, rxBuf.clockSecs, rxBuf.scoreB);
        Serial.printf("Score %d:%d  Clock %d:%02d\n",
            rxBuf.scoreA, rxBuf.scoreB,
            rxBuf.clockSecs / 60, rxBuf.clockSecs % 60);
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    waitMs(500);

    // Startup splash
    dmd.clearScreen(true);
    drawRow("SLAVE1",   Y_EVENT);
    drawRow("V1.1",     Y_TEAMS);
    drawScoreZone(0, 0, 0);
    waitMs(1200);

    // Default display — shown until first packet arrives
    dmd.clearScreen(true);
    drawRow(eventText, Y_EVENT);              // "WAITING"
    char defTeam[35];
    snprintf(defTeam, sizeof(defTeam), "%s vs %s", teamAText, teamBText);
    drawRow(defTeam, Y_TEAMS);                // "TEAM A vs TEAM B"
    drawScoreZone(0, 600, 0);                 // 0 — 10:00 — 0

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
