/*
  Slave 1 v1.0
  ─────────────────────────────────────────────────────
  Hardware : 6 panels wide × 2 tall  (192 × 32 px)
  ROW 1 (y =  0..15) : Event name   — always scrolling
  ROW 2 (y = 16..31) : TeamA vs TeamB — static

  No timer ISR — avoids WiFi + flash-cache crash.
  scanDisplayBySPI() polled via micros() from loop context.

  ROW 2 only redraws when team names change (N packet / OK press).
  ROW 1 scroll is never interrupted by a team-row update.
*/

#include <DMD32.h>
#include "fonts/Arial_black_16.h"
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define DISPLAYS_ACROSS 6
#define DISPLAYS_DOWN   2          // 6 × 2 panels = 192 × 32 px
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
char          teamAText[16] = "";
char          teamBText[16] = "";

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
//  showTeamRow()
//  Draws "TeamA vs TeamB" on ROW 2 (y=16..31) only.
//  Clears just that row with drawFilledBox — ROW 1 scroll is untouched.
//  Called automatically when team names change; never called from scroll loop.
// ══════════════════════════════════════════════════════════════════════════════
void showTeamRow() {
    dmd.selectFont(Arial_Black_16);
    // Clear ROW 2 only
    dmd.drawFilledBox(0, 16, (32 * DISPLAYS_ACROSS) - 1, 31, GRAPHICS_INVERSE);
    // Build and draw "TeamA vs TeamB"
    char buf[35];
    snprintf(buf, sizeof(buf), "%s vs %s", teamAText, teamBText);
    int len = strlen(buf);
    if (len > 0)
        dmd.drawString(0, 16, buf, len, GRAPHICS_NORMAL);
    Serial.print("Team row: "); Serial.println(buf);
}

// ── Check for new data ─────────────────────────────────────────────────────────
// Returns true  → event name changed, scroll must restart.
// Returns false → only team names changed (showTeamRow called here, scroll continues).
bool checkNewData() {
    if (!newData) return false;
    newData = false;

    bool eventChanged = (strncmp(eventText, rxBuf.eventName, 32) != 0);
    bool teamAChanged = (strncmp(teamAText, rxBuf.teamA,     15) != 0);
    bool teamBChanged = (strncmp(teamBText, rxBuf.teamB,     15) != 0);

    if (eventChanged) {
        strncpy(eventText, rxBuf.eventName, 32);
        eventText[32] = '\0';
        Serial.print("Event: "); Serial.println(eventText);
    }
    if (teamAChanged || teamBChanged) {
        strncpy(teamAText, rxBuf.teamA, 15); teamAText[15] = '\0';
        strncpy(teamBText, rxBuf.teamB, 15); teamBText[15] = '\0';
        showTeamRow();       // update ROW 2 — does NOT restart scroll
    }
    return eventChanged;
}

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    waitMs(500);

    dmd.clearScreen(true);
    dmd.selectFont(Arial_Black_16);
    dmd.drawString(0, 0,  "SLAVE1", 6, GRAPHICS_NORMAL);
    dmd.drawString(0, 16, "V1.0",   4, GRAPHICS_NORMAL);
    waitMs(1200);

    dmd.clearScreen(true);
    dmd.selectFont(Arial_Black_16);
    dmd.drawString(0, 0, "WAITING", 7, GRAPHICS_NORMAL);
    waitMs(200);

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());

    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init FAILED");
        dmd.clearScreen(true);
        dmd.drawString(0, 0, "ERR", 3, GRAPHICS_NORMAL);
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 1 v1.0 ready — waiting for master");
    }
}

// ── Loop — ROW 1 scroll ────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();

    char buf[33];
    strncpy(buf, eventText, 32);
    buf[32] = '\0';
    int len = strlen(buf);
    if (len == 0) { waitMs(100); return; }

    dmd.selectFont(Arial_Black_16);

    // Clear ROW 1 only before starting new scroll — ROW 2 is untouched
    dmd.drawFilledBox(0, 0, (32 * DISPLAYS_ACROSS) - 1, 15, GRAPHICS_INVERSE);
    waitMs(300);

    // Marquee on ROW 1 (y=0) — drawMarquee / stepMarquee never touch y=16..31
    dmd.drawMarquee(buf, len, (32 * DISPLAYS_ACROSS) - 1, 0);

    long    timer_1    = millis();
    boolean scrollDone = false;

    while (!scrollDone) {
        scanIfNeeded();
        if ((millis() - timer_1) >= 40) {
            scrollDone = dmd.stepMarquee(-1, 0);
            timer_1    = millis();
        }
        if (checkNewData()) return;   // event name changed → restart scroll
    }
    // scroll complete → loop() restarts → scrolls again
}
