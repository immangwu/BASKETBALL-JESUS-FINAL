/*
  Slave 1 v1.0 — Static Display
  ─────────────────────────────────────────────────────
  Hardware : 6 panels wide × 2 tall  (192 × 32 px)

  ROW 1 (y =  0..15) : Event name      — STATIC, centred
  ROW 2 (y = 16..31) : TeamA vs TeamB  — STATIC, centred

  Auto-size order (both rows):
    1. Arial_Black_16 (16 px) — try first
    2. Arial_14       (14 px) — if too wide
    3. SystemFont5x7  ( 7 px) — fallback

  No scrolling. No timer ISR.
  scanDisplayBySPI() polled via micros() from loop.
  Both rows update only when N packet arrives (OK press).
*/

#include <DMD32.h>
#include "fonts/Arial_black_16.h"
#include "fonts/Arial14.h"
#include "fonts/SystemFont5x7.h"
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define DISPLAYS_ACROSS 6
#define DISPLAYS_DOWN   2
#define DISPLAY_W       (32 * DISPLAYS_ACROSS)   // 192 px

DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);

// ── Scan polling (no timer ISR) ────────────────────────────────────────────────
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

// ── Data struct (must match master exactly) ────────────────────────────────────
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
volatile bool newData   = false;
char          eventText[33] = "WAITING";
char          teamAText[16] = "TEAM A";
char          teamBText[16] = "TEAM B";

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ── String width helper ────────────────────────────────────────────────────────
// Call after dmd.selectFont(). Matches drawString() char+gap accounting.
int strPixelWidth(const char* s, int len) {
    int w = 0;
    for (int i = 0; i < len; i++) {
        int cw = dmd.charWidth((unsigned char)s[i]);
        if (cw > 0) w += cw + 1;
    }
    return w;
}

// ── Draw one row: auto-size font, centre horizontally & vertically ─────────────
// y0 = top of the 16-px row (0 for ROW1, 16 for ROW2)
void drawRow(const char* text, int y0) {
    int len = strlen(text);

    // Pick largest font that fits in 192 px
    const uint8_t* font   = Arial_Black_16;
    int            fontH  = 16;

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
    int drawY = y0 + (16 - fontH) / 2;   // vertical centre in 16-px row

    // Clear only this row, then draw
    dmd.drawFilledBox(0, y0, DISPLAY_W - 1, y0 + 15, GRAPHICS_INVERSE);
    dmd.selectFont(font);
    dmd.drawString(drawX, drawY, text, len, GRAPHICS_NORMAL);
}

// ── Check for new data ─────────────────────────────────────────────────────────
void checkNewData() {
    if (!newData) return;
    newData = false;

    bool eventChanged = (strncmp(eventText, rxBuf.eventName, 32) != 0);
    bool teamAChanged = (strncmp(teamAText, rxBuf.teamA,     15) != 0);
    bool teamBChanged = (strncmp(teamBText, rxBuf.teamB,     15) != 0);

    if (eventChanged) {
        strncpy(eventText, rxBuf.eventName, 32);
        eventText[32] = '\0';
        drawRow(eventText, 0);
        Serial.print("Event: "); Serial.println(eventText);
    }
    if (teamAChanged || teamBChanged) {
        strncpy(teamAText, rxBuf.teamA, 15); teamAText[15] = '\0';
        strncpy(teamBText, rxBuf.teamB, 15); teamBText[15] = '\0';
        char buf[35];
        snprintf(buf, sizeof(buf), "%s vs %s", teamAText, teamBText);
        drawRow(buf, 16);
        Serial.print("Teams: "); Serial.println(buf);
    }
}

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    waitMs(500);

    // Startup splash
    dmd.clearScreen(true);
    drawRow("SLAVE1", 0);
    drawRow("V1.0",   16);
    waitMs(1200);

    // Default display — shown until first N packet arrives
    dmd.clearScreen(true);
    drawRow(eventText, 0);     // "WAITING"
    char defTeam[35];
    snprintf(defTeam, sizeof(defTeam), "%s vs %s", teamAText, teamBText);
    drawRow(defTeam, 16);      // "TEAM A vs TEAM B"

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());

    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init FAILED");
        dmd.clearScreen(true);
        drawRow("ERR", 0);
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 1 v1.0 ready");
    }
}

// ── Loop ───────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();
    checkNewData();
}
