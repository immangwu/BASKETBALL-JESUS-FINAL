/*
  Slave 1 — Event Name Display
  6 panels wide x 1 tall (192 x 16 px)  |  Font: Arial_Black_16

  Timer ISR drives the scan — safe with WiFi because scanDisplayBySPI()
  is IRAM_ATTR (lives in internal RAM, not flash), so WiFi temporarily
  disabling the flash cache does NOT crash the ISR.

  Behaviour:
    STATIC mode  — draw once, hold forever until OK is pressed again
    SCROLL mode  — scroll across, hold static, scroll again, repeat
    Both modes switch immediately when master broadcasts new data.
*/

#include <DMD32.h>
#include "fonts/Arial_black_16.h"
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define DISPLAYS_ACROSS 6
#define DISPLAYS_DOWN   1
DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);

// ── Timer ISR ─────────────────────────────────────────────────────────────────
// scanOK pauses the ISR while we rewrite screen RAM (prevents torn frames).
// The ISR itself is IRAM_ATTR; scanDisplayBySPI() is also IRAM_ATTR in DMD32.
hw_timer_t*  timer  = NULL;
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
    int  eventScroll;   // 0 = static, 1 = scroll
} BoardData;

BoardData     rxBuf;
volatile bool newData    = false;
char          displayText[33] = "WAITING";
bool          scrollMode      = false;

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ── Helpers ────────────────────────────────────────────────────────────────────
void showStatic(const char* text) {
    scanOK = false;
    dmd.clearScreen(true);
    dmd.selectFont(Arial_Black_16);
    dmd.drawString(0, 0, text, strlen(text), GRAPHICS_NORMAL);
    scanOK = true;
}

// Returns true only when eventName or scrollMode actually changed.
// S-packet clock/score updates arrive constantly but change nothing here.
bool checkNewData() {
    if (!newData) return false;
    newData = false;
    bool newScroll   = (rxBuf.eventScroll == 1);
    bool nameChanged = (strncmp(displayText, rxBuf.eventName, 32) != 0);
    bool modeChanged = (newScroll != scrollMode);
    if (!nameChanged && !modeChanged) return false;
    strncpy(displayText, rxBuf.eventName, 32);
    displayText[32] = '\0';
    scrollMode = newScroll;
    Serial.print("Event: ");
    Serial.print(displayText);
    Serial.println(scrollMode ? "  [SCROLL]" : "  [STATIC]");
    return true;
}

// ── Setup ──────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);

    // Timer ISR — same pattern as working P1_LED_Scrolling reference.
    // scanDisplayBySPI() is IRAM_ATTR so this is safe alongside WiFi.
    uint8_t cpuClock = ESP.getCpuFreqMHz();
    timer = timerBegin(0, cpuClock, true);
    timerAttachInterrupt(timer, &triggerScan, true);
    timerAlarmWrite(timer, 300, true);
    timerAlarmEnable(timer);

    showStatic("SLAVE1");
    delay(1200);
    showStatic("WAITING");

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);

    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());

    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init FAILED");
        showStatic("ERR");
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 1 ready — waiting for master");
    }
}

// ── Loop ───────────────────────────────────────────────────────────────────────
void loop() {
    checkNewData();

    char buf[33];
    strncpy(buf, displayText, 32);
    buf[32] = '\0';
    int len = strlen(buf);
    if (len == 0) { delay(100); return; }

    // ── STATIC mode ────────────────────────────────────────────────────────────
    if (!scrollMode) {
        showStatic(buf);
        for (;;) {
            if (checkNewData()) return;
            delay(20);
        }
    }

    // ── SCROLL mode ────────────────────────────────────────────────────────────
    scanOK = false;
    dmd.selectFont(Arial_Black_16);
    dmd.clearScreen(true);
    dmd.drawMarquee(buf, len, (32 * DISPLAYS_ACROSS) - 1, 0);
    scanOK = true;

    long    lastStep   = millis();
    boolean scrollDone = false;
    while (!scrollDone) {
        if ((millis() - lastStep) >= 40) {
            scanOK     = false;
            scrollDone = dmd.stepMarquee(-1, 0);
            scanOK     = true;
            lastStep   = millis();
        }
        if (checkNewData()) return;
    }

    // Hold static after scroll, then loop scrolls again
    showStatic(buf);
    for (;;) {
        if (checkNewData()) return;
        delay(20);
    }
}
