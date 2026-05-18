/*
  Quarter Display  v3.0  —  TOP panel  (64 × 16 px)
  ─────────────────────────────────────────────────────────────────────────
  Layout:
    y=0..7   "Quarter" label  — size 1, centred, YELLOW
    y=8..15  Number/OT top half — size 3 (24px tall), only top 16px visible

  Numbers: 1 / 2 / 3 / 4 / OT  (no leading "Q")
*/

#define PxMATRIX_SPI_FREQUENCY 10000000
#include <PxMatrix.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define P_LAT  5
#define P_A   19
#define P_B   23
#define P_C   18
#define P_OE   4

PxMATRIX display(64, 16, P_LAT, P_OE, P_A, P_B, P_C);

uint8_t  display_draw_time = 30;
uint16_t C_BLACK, C_YELLOW;

static unsigned long lastScan = 0;
void scanIfNeeded() {
    unsigned long now = micros();
    if ((long)(now - lastScan) >= 2000) { display.display(display_draw_time); lastScan = now; }
}
void waitMs(long ms) {
    long end = (long)millis() + ms;
    while ((long)(millis() - end) < 0) scanIfNeeded();
}

typedef struct __attribute__((packed)) {
    char eventName[32]; char teamA[16]; char teamB[16];
    int  scoreA, scoreB, clockSecs, clockTenths, quarter;
    char possession;
    int  foulsA, foulsB, timeoutsA, timeoutsB, screenMask;
    int  clockRunning, shotSecs, shotTenths, shotRunning, eventScroll;
    char marketingText[32];
} BoardData;

BoardData rxBuf; volatile bool newData = false;
int lastQuarter = -1;

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == 1 && data[0] == 0xAA) { ESP.restart(); return; }
    if (len == sizeof(BoardData)) { memcpy(&rxBuf, data, sizeof(rxBuf)); newData = true; }
}

// Returns "1".."4" or "OT"
void getNumStr(int quarter, char* buf, int bufLen) {
    if (quarter >= 5) snprintf(buf, bufLen, "OT");
    else              snprintf(buf, bufLen, "%d", quarter);
}

void drawTop(int quarter) {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);

    // "Quarter" label — size 1 (8px tall), top row
    display.setTextSize(1);
    display.setTextColor(C_YELLOW);
    const char* lbl = "Quarter";
    int lw = (int)strlen(lbl) * 6;
    display.setCursor((64 - lw) / 2, 0);
    display.print(lbl);

    // Number — size 3 (24px tall), cursor y=8 → top 8px of glyph visible
    display.setTextSize(3);
    display.setTextColor(C_YELLOW);
    char buf[4];
    getNumStr(quarter, buf, sizeof(buf));
    int tw = (int)strlen(buf) * 18;
    display.setCursor((64 - tw) / 2, 8);
    display.print(buf);
}

void showWait() {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);
    display.setTextSize(1);
    display.setTextColor(C_YELLOW);
    display.setCursor(20, 4);
    display.print("WAIT");
}

void setup() {
    delay(1000);
    Serial.begin(115200);
    display.begin(8);
    delay(100);
    C_BLACK  = display.color565(  0,   0,   0);
    C_YELLOW = display.color565(255, 255,   0);
    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);
    showWait();
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("Quarter top MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Quarter top v3.0 ready");
    }
}

void loop() {
    scanIfNeeded();
    if (!newData) return;
    newData = false;
    if (rxBuf.quarter != lastQuarter) {
        lastQuarter = rxBuf.quarter;
        drawTop(rxBuf.quarter);
    }
}
