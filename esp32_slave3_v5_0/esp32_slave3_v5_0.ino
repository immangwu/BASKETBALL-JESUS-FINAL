/*
  ╔══════════════════════════════════════════════════════════════════════════╗
  ║  Slave 3  v5.0  —  Fouls A  |  Shot Clock  |  Fouls B                  ║
  ║  Hardware : 2 rows × 2 columns of P10 RGB panels  →  64 × 64 px        ║
  ╠══════════════════════════════════════════════════════════════════════════╣
  ║  STARTUP                                                                 ║
  ║    "WAIT" shown until first ESP-NOW packet arrives.                     ║
  ║                                                                          ║
  ║  CONNECTED — three vertical zones (widths 20 | 24 | 20 px):            ║
  ║                                                                          ║
  ║    ┌──────────┬────────────┬──────────┐                                 ║
  ║    │  FA      │    SCLK    │    FB    │  ← top row (y=32..63) — label  ║
  ║    ├──────────┼────────────┼──────────┤                                 ║
  ║    │   #      │     ##     │    #     │  ← bot row (y= 0..31) — value  ║
  ║    └──────────┴────────────┴──────────┘                                 ║
  ║                                                                          ║
  ║  Foul colours  : GREEN (normal) → RED (>= FOULS_MAX)                   ║
  ║  Shot clock    : CYAN  — textSize=1 (reduced from v4.0's textSize=2)   ║
  ║  Labels        : YELLOW                                                  ║
  ╠══════════════════════════════════════════════════════════════════════════╣
  ║  Changes vs v4.0:                                                        ║
  ║    - BoardData gains marketingText[32] to match master struct exactly   ║
  ║    - Shot clock value: textSize 2->1 (8 px), fouls keep textSize=3     ║
  ║    - < 10 s: digit + tenths both textSize=1, tenths as subscript        ║
  ╚══════════════════════════════════════════════════════════════════════════╝
*/

// ── Library & pin config ──────────────────────────────────────────────────────
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

PxMATRIX display(64, 32, P_LAT, P_OE, P_A, P_B, P_C);
uint8_t  display_draw_time = 40;

uint16_t C_BLACK, C_GREEN, C_RED, C_YELLOW, C_CYAN, C_WHITE;

// ── Polled scan ───────────────────────────────────────────────────────────────
static unsigned long lastScan = 0;

void scanIfNeeded() {
    unsigned long now = micros();
    if ((long)(now - lastScan) >= 2000) {
        display.display(display_draw_time);
        lastScan = now;
    }
}

void waitMs(long ms) {
    long end = (long)millis() + ms;
    while ((long)(millis() - end) < 0) scanIfNeeded();
}

// ── BoardData — must match master exactly (includes marketingText) ────────────
typedef struct __attribute__((packed)) {
    char eventName[64];
    char teamA[16];
    char teamB[16];
    int  scoreA, scoreB, clockSecs, clockTenths, quarter;
    char possession;
    int  foulsA, foulsB, timeoutsA, timeoutsB, screenMask;
    int  clockRunning, shotSecs, shotTenths, shotRunning, eventScroll;
    char marketingText[32];
} BoardData;

BoardData     rxBuf;
volatile bool newData   = false;
bool          connected = false;

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ── Zone geometry ─────────────────────────────────────────────────────────────
#define ZONE_A_X       0
#define ZONE_A_W      20
#define ZONE_SC_X     20
#define ZONE_SC_W     24
#define ZONE_B_X      44
#define ZONE_B_W      20

#define LABEL_ROW_Y   32
#define LABEL_ROW_H   32
#define VALUE_ROW_Y    0
#define VALUE_ROW_H   32

#define FOULS_MAX      5

// ── Helpers ───────────────────────────────────────────────────────────────────
void clearZone(int x, int w, int y, int h) {
    display.fillRect(x, y, w, h, C_BLACK);
}

void drawText(const char* s, int zoneX, int zoneW,
              int bandY, int bandH, uint16_t color, uint8_t sz) {
    int tw = (int)strlen(s) * 6 * sz;
    int th = 8 * sz;
    int tx = zoneX + max(0, (zoneW - tw) / 2);
    int ty = bandY + max(0, (bandH - th) / 2);
    display.setTextSize(sz);
    display.setTextColor(color);
    display.setTextWrap(false);
    display.setCursor(tx, ty);
    display.print(s);
}

// ── Fouls zone ────────────────────────────────────────────────────────────────
void drawFoulsZone(int zoneX, int zoneW, const char* label, int fouls) {
    clearZone(zoneX, zoneW, LABEL_ROW_Y, LABEL_ROW_H);
    drawText(label, zoneX, zoneW, LABEL_ROW_Y, LABEL_ROW_H, C_YELLOW, 1);
    clearZone(zoneX, zoneW, VALUE_ROW_Y, VALUE_ROW_H);
    uint16_t col = (fouls >= FOULS_MAX) ? C_RED : C_GREEN;
    char buf[4]; snprintf(buf, sizeof(buf), "%d", fouls);
    drawText(buf, zoneX, zoneW, VALUE_ROW_Y, VALUE_ROW_H, col, 3);
}

// ── Shot clock zone — value at textSize=1 (reduced from textSize=2) ───────────
void drawShotClockZone(int secs, int tenths) {
    clearZone(ZONE_SC_X, ZONE_SC_W, LABEL_ROW_Y, LABEL_ROW_H);
    drawText("SCLK", ZONE_SC_X, ZONE_SC_W, LABEL_ROW_Y, LABEL_ROW_H, C_CYAN, 1);
    clearZone(ZONE_SC_X, ZONE_SC_W, VALUE_ROW_Y, VALUE_ROW_H);

    if (secs >= 10) {
        char buf[4]; snprintf(buf, sizeof(buf), "%d", secs);
        drawText(buf, ZONE_SC_X, ZONE_SC_W, VALUE_ROW_Y, VALUE_ROW_H, C_CYAN, 1);
    } else {
        char ss[3]; snprintf(ss, sizeof(ss), "%d",  secs);
        char tt[4]; snprintf(tt, sizeof(tt), ".%d", tenths);
        int totalW = ((int)strlen(ss) + (int)strlen(tt)) * 6;
        int x0     = ZONE_SC_X + max(0, (ZONE_SC_W - totalW) / 2);
        int y0     = VALUE_ROW_Y + max(0, (VALUE_ROW_H - 8) / 2);
        display.setTextSize(1);
        display.setTextColor(C_CYAN);
        display.setTextWrap(false);
        display.setCursor(x0, y0);
        display.print(ss);
        display.setCursor(x0 + (int)strlen(ss) * 6, y0 + 4);
        display.print(tt);
    }
}

// ── WAIT screen ───────────────────────────────────────────────────────────────
void showWait() {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextWrap(false);
    display.setTextColor(C_CYAN);
    display.setCursor(8, 0);
    display.print("WAIT");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    delay(1000);
    Serial.begin(115200);
    display.begin(8);
    delay(100);

    C_BLACK  = display.color565(  0,   0,   0);
    C_WHITE  = display.color565(255, 255, 255);
    C_GREEN  = display.color565(  0, 255,   0);
    C_RED    = display.color565(  0,   0, 255);
    C_YELLOW = display.color565(  0, 255, 255);
    C_CYAN   = display.color565(255, 255,   0);

    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);

    drawText("SLAVE 3", 0, 64, 20, 24, C_GREEN, 1);
    waitMs(1500);
    showWait();
    waitMs(200);

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init FAILED");
        display.clearDisplay();
        drawText("ESP-NOW", 0, 64, 16, 16, C_RED, 1);
        drawText("FAILED",  0, 64, 32, 16, C_RED, 1);
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 3 v5.0 ready");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();
    if (!newData) return;
    newData = false;
    if (!connected) {
        connected = true;
        display.clearDisplay();
    }
    drawFoulsZone(ZONE_A_X,  ZONE_A_W,  "FA", rxBuf.foulsA);
    drawShotClockZone(rxBuf.shotSecs, rxBuf.shotTenths);
    drawFoulsZone(ZONE_B_X,  ZONE_B_W,  "FB", rxBuf.foulsB);
}
