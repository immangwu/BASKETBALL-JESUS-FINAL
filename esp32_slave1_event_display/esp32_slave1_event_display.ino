/*
  Slave 1 — Event Name Scroll
  6 panels wide × 1 tall (192 × 16 px)

  NO TIMER ISR — avoids the WiFi + flash-cache crash completely.
  scanDisplayBySPI() is called directly from loop() every ~300 µs
  using micros() polling. Loop context always has flash access — no crash.
*/

#include <DMD32.h>
#include "fonts/Arial_black_16.h"
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define DISPLAYS_ACROSS 6
#define DISPLAYS_DOWN   1
DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);

// ── Scan polling — replaces timer ISR ─────────────────────────────────────────
unsigned long lastScan = 0;

void scanIfNeeded() {
    if ((long)(micros() - lastScan) >= 300) {
        dmd.scanDisplayBySPI();
        lastScan = micros();
    }
}

// Use instead of delay() so the display keeps refreshing
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

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// Returns true if event name changed (restart scroll)
bool checkNewData() {
    if (!newData) return false;
    newData = false;
    if (strncmp(eventText, rxBuf.eventName, 32) == 0) return false;
    strncpy(eventText, rxBuf.eventName, 32);
    eventText[32] = '\0';
    Serial.print("Event: "); Serial.println(eventText);
    return true;
}

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    waitMs(500);

    // Show startup message (scan runs via waitMs)
    dmd.clearScreen(true);
    dmd.selectFont(Arial_Black_16);
    dmd.drawString(0, 0, "SLAVE1", 6, GRAPHICS_NORMAL);
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
        Serial.println("Slave 1 ready — waiting for master");
    }
}

// ── Loop ───────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();

    char buf[33];
    strncpy(buf, eventText, 32);
    buf[32] = '\0';
    int len = strlen(buf);
    if (len == 0) { waitMs(100); return; }

    dmd.selectFont(Arial_Black_16);
    dmd.clearScreen(true);
    waitMs(500);

    // Start marquee — same pattern as reference P1_LED_Scrolling
    dmd.drawMarquee(buf, len, (32 * DISPLAYS_ACROSS) - 1, 0);

    long    timer_1    = millis();
    boolean scrollDone = false;

    while (!scrollDone) {
        scanIfNeeded();
        if ((millis() - timer_1) >= 40) {
            scrollDone = dmd.stepMarquee(-1, 0);
            timer_1    = millis();
        }
        if (checkNewData()) return;   // new event name → restart
    }
    // scroll done → loop() restarts → scrolls again
}
