/*
  Slave 1 — Event Title (always scrolling) + Team Matchup (static)
  6 panels wide × 2 tall (192 × 32 px)

  TOP ROW    (y= 0–15): event name  — always scrolling
  BOTTOM ROW (y=16–31): TeamA vs TeamB — static, updates only on OK press

  WHY THIS DOES NOT CRASH WITH WIFI:
    triggerScan() ISR does ONE thing: needScan = true  (single RAM write).
    A RAM write never needs the flash cache — safe from any ISR, always.
    scanDisplayBySPI() runs in loop() context where flash is always available.
    This works regardless of whether the DMD32 library has IRAM_ATTR or not.
*/

#include <DMD32.h>
#include "fonts/Arial_black_16.h"
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define DISPLAYS_ACROSS 6
#define DISPLAYS_DOWN   2          // 6 × 2 panels = 192 × 32 px
DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);

// ── Timer ISR — flag only ──────────────────────────────────────────────────────
hw_timer_t*   timer    = NULL;
volatile bool needScan = false;

// ISR body is one RAM write — no flash access, no crash possible.
void IRAM_ATTR triggerScan() {
    needScan = true;
}

// Call from loop to service the pending scan.
void scanPoll() {
    if (needScan) { needScan = false; dmd.scanDisplayBySPI(); }
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
char          teamText[35]  = "";     // "TeamA vs TeamB" — max 34 chars

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ── Draw helpers ───────────────────────────────────────────────────────────────
void drawBottomRow() {
    int w = 32 * DISPLAYS_ACROSS;
    dmd.selectFont(Arial_Black_16);
    dmd.drawFilledBox(0, 16, w - 1, 31, GRAPHICS_INVERSE);   // clear bottom row
    if (strlen(teamText) > 0)
        dmd.drawString(0, 16, teamText, strlen(teamText), GRAPHICS_NORMAL);
}

// ── Check for new data ─────────────────────────────────────────────────────────
// Returns true if event name changed (scroll must restart).
// Team change redraws bottom row without touching the scroll.
bool checkNewData() {
    if (!newData) return false;
    newData = false;

    bool eventChanged = (strncmp(eventText, rxBuf.eventName, 32) != 0);

    char newTeam[35];
    snprintf(newTeam, sizeof(newTeam), "%s vs %s", rxBuf.teamA, rxBuf.teamB);
    bool teamChanged = (strcmp(teamText, newTeam) != 0);

    if (eventChanged) {
        strncpy(eventText, rxBuf.eventName, 32);
        eventText[32] = '\0';
    }
    if (teamChanged) {
        strncpy(teamText, newTeam, 34);
        teamText[34] = '\0';
        drawBottomRow();
    }
    return eventChanged;
}

// delay-replacement: keeps scanning while waiting
void waitMs(long ms) {
    long end = millis() + ms;
    while (millis() < end) scanPoll();
}

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    // Timer — identical to working P1_LED_Scrolling reference
    uint8_t cpuClock = ESP.getCpuFreqMHz();
    timer = timerBegin(0, cpuClock, true);
    timerAttachInterrupt(timer, &triggerScan, true);
    timerAlarmWrite(timer, 300, true);
    timerAlarmEnable(timer);
    delay(500);

    // Startup splash
    dmd.clearScreen(true);
    dmd.selectFont(Arial_Black_16);
    dmd.drawString(0, 0,  "SLAVE1", 6, GRAPHICS_NORMAL);
    dmd.drawString(0, 16, "READY",  5, GRAPHICS_NORMAL);
    waitMs(1200);

    dmd.clearScreen(true);
    dmd.selectFont(Arial_Black_16);
    dmd.drawString(0, 0, "WAITING", 7, GRAPHICS_NORMAL);

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
        Serial.println("Slave 1 ready — waiting for master");
    }
}

// ── Loop ───────────────────────────────────────────────────────────────────────
void loop() {
    scanPoll();

    char buf[33];
    strncpy(buf, eventText, 32);
    buf[32] = '\0';
    int len = strlen(buf);
    if (len == 0) { waitMs(100); return; }

    dmd.selectFont(Arial_Black_16);

    // Begin scroll on top row (y=0). Bottom row is never touched here.
    dmd.drawMarquee(buf, len, (32 * DISPLAYS_ACROSS) - 1, 0);

    long    timer_1    = millis();
    boolean scrollDone = false;

    while (!scrollDone) {
        scanPoll();                          // service display scan
        if ((millis() - timer_1) >= 40) {
            scrollDone = dmd.stepMarquee(-1, 0);
            timer_1    = millis();
        }
        if (checkNewData()) return;          // event changed → restart scroll
    }
    // Scroll complete → loop() restarts, scrolls again. No clearScreen → bottom row intact.
}
