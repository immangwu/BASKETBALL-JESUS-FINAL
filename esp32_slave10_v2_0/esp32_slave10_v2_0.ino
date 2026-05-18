/*
  ╔══════════════════════════════════════════════════════════════╗
  ║  Slave 10  v2.0  —  Quarter  BOTTOM row                     ║
  ║  Hardware : 2 × P10 panels in one row  →  64 × 16 px        ║
  ╠══════════════════════════════════════════════════════════════╣
  ║  Draws "Q1".."Q4" or "OT" at size 3, cursor y=-16 so the    ║
  ║  bottom 8 px of the 24-px glyph appear on this panel.       ║
  ║  Q and digit both size 3 (same as Slave 9 top row).         ║
  ║  Color: YELLOW                                              ║
  ╚══════════════════════════════════════════════════════════════╝
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
} BoardData;

BoardData rxBuf; volatile bool newData = false; bool connected = false;

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == 1 && data[0] == 0xAA) { ESP.restart(); return; }
    if (len == sizeof(BoardData)) { memcpy(&rxBuf, data, sizeof(rxBuf)); newData = true; }
}

void getQuarterStr(int quarter, char* buf, int bufLen) {
    if (quarter >= 5) snprintf(buf, bufLen, "OT");
    else              snprintf(buf, bufLen, "Q%d", quarter);
}

void drawBottom(int quarter) {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);
    display.setTextSize(3);
    display.setTextColor(C_YELLOW);
    char buf[4];
    getQuarterStr(quarter, buf, sizeof(buf));
    int tw = (int)strlen(buf) * 18;
    display.setCursor((64 - tw) / 2, -8);  // glyph rows 8-23 visible (bottom 16 px)
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
    Serial.print("Slave10 MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 10 v2.0 ready — 64x16 Quarter bottom row");
    }
}

void loop() {
    scanIfNeeded();
    if (!newData) return;
    newData = false;
    if (!connected) { connected = true; Serial.println("Master connected"); }
    drawBottom(rxBuf.quarter);
}
