/*
  Slave 1 — Event Title + Team Matchup
  6 panels wide × 2 tall (192 × 32 px)

  Top row    (y= 0) : event name  — always scrolling
  Bottom row (y=16) : "TeamA vs TeamB" — static, updates only on OK press

  Timer ISR pattern identical to the working P1_LED_Scrolling reference.
  scanDisplayBySPI() is IRAM_ATTR in DMD32 lib so the ISR is WiFi-safe.
*/

#include <DMD32.h>
#include "fonts/Arial_black_16.h"
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define DISPLAYS_ACROSS 6
#define DISPLAYS_DOWN   2          // 6×2 panels = 192×32 px
DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);

// ── Timer ISR ─────────────────────────────────────────────────────────────────
hw_timer_t*   timer  = NULL;
volatile bool scanOK = true;

void IRAM_ATTR triggerScan() {
    if (scanOK) dmd.scanDisplayBySPI();
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
char          teamText[35]  = "";      // "TeamA vs TeamB" max 15+4+15=34 chars

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ── Draw bottom row (y=16‥31) only — top row scroll is unaffected ─────────────
void drawTeamRow() {
    int w = 32 * DISPLAYS_ACROSS;
    scanOK = false;
    dmd.selectFont(Arial_Black_16);
    dmd.drawFilledBox(0, 16, w - 1, 31, GRAPHICS_INVERSE);   // clear bottom row
    if (strlen(teamText) > 0)
        dmd.drawString(0, 16, teamText, strlen(teamText), GRAPHICS_NORMAL);
    scanOK = true;
}

// ── Check incoming data ────────────────────────────────────────────────────────
// Returns true only if the event name changed (→ restart scroll).
// Team change redraws bottom row in place without interrupting the scroll.
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
        drawTeamRow();                // update bottom row without clearing top
    }
    return eventChanged;
}

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);

    // Timer ISR — same as working P1_LED_Scrolling reference
    uint8_t cpuClock = ESP.getCpuFreqMHz();
    timer = timerBegin(0, cpuClock, true);
    timerAttachInterrupt(timer, &triggerScan, true);
    timerAlarmWrite(timer, 300, true);
    timerAlarmEnable(timer);
    delay(500);

    // Startup splash
    scanOK = false;
    dmd.clearScreen(true);
    dmd.selectFont(Arial_Black_16);
    dmd.drawString(0, 0,  "SLAVE1",   6, GRAPHICS_NORMAL);
    dmd.drawString(0, 16, "READY",    5, GRAPHICS_NORMAL);
    scanOK = true;
    delay(1200);

    scanOK = false;
    dmd.clearScreen(true);
    dmd.selectFont(Arial_Black_16);
    dmd.drawString(0, 0,  "WAITING",  7, GRAPHICS_NORMAL);
    scanOK = true;

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());

    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init FAILED");
        scanOK = false;
        dmd.clearScreen(true);
        dmd.drawString(0, 0, "ERR", 3, GRAPHICS_NORMAL);
        scanOK = true;
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 1 ready — waiting for master");
    }
}

// ── Loop ───────────────────────────────────────────────────────────────────────
void loop() {
    char buf[33];
    strncpy(buf, eventText, 32);
    buf[32] = '\0';
    int len = strlen(buf);
    if (len == 0) { delay(100); return; }

    dmd.selectFont(Arial_Black_16);

    // Start scroll on TOP row only (y=0). Bottom row is never touched here.
    scanOK = false;
    dmd.drawMarquee(buf, len, (32 * DISPLAYS_ACROSS) - 1, 0);
    scanOK = true;

    long    timer_1    = millis();
    boolean scrollDone = false;

    while (!scrollDone) {
        if ((millis() - timer_1) >= 40) {
            scanOK     = false;
            scrollDone = dmd.stepMarquee(-1, 0);
            scanOK     = true;
            timer_1    = millis();
        }
        if (checkNewData()) return;   // event name changed → restart scroll
    }
    // Scroll finished → loop() restarts immediately, scrolling again.
    // No clearScreen → bottom row stays intact.
}
