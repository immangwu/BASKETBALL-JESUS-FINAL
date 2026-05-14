/*
  ╔══════════════════════════════════════════════════════════════╗
  ║  Slave 3  v10.0  —  Shot Clock  TOP row                     ║
  ║  Hardware : 2 × P10 panels in one row  →  64 × 16 px        ║
  ╠══════════════════════════════════════════════════════════════╣
  ║  Layout:                                                     ║
  ║   ┌──────────────────────────────────────┐                  ║
  ║   │SC  ┌──────────────────────────────┐  │  ← Slave 3 TOP  ║
  ║   │    │   TOP HALF of large digit    │  │    (this file)   ║
  ║   └────┴──────────────────────────────┴──┘                  ║
  ║   ┌────┬──────────────────────────────┬──┐                  ║
  ║   │    │  BOTTOM HALF of large digit  │.X│  ← Slave 4 BOT  ║
  ║   └────┴──────────────────────────────┴──┘                  ║
  ║                                                              ║
  ║  "SC" label : size 1 (6×8 px), top-left corner (x=0, y=0)  ║
  ║  Large digit: size 4 (24×32 px), cursor y=0 → top 16 px    ║
  ║               visible on this panel, bottom 16 px on Slave4 ║
  ║                                                              ║
  ║    ≥ 10 s : show full seconds  "24"  (2 chars, x=8)        ║
  ║    <  10 s : show single digit "9"   (1 char,  x=20)       ║
  ║                                                              ║
  ║  BLUE (> 5 s)  →  RED (≤ 5 s)                             ║
  ╚══════════════════════════════════════════════════════════════╝
*/

#define PxMATRIX_SPI_FREQUENCY 10000000
#include <PxMatrix.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

// ── Pins ──────────────────────────────────────────────────────────────────────
#define P_LAT  5
#define P_A   19
#define P_B   23
#define P_C   18
#define P_OE   4

// 2 panels in a single row = 64 wide × 16 tall
PxMATRIX display(64, 16, P_LAT, P_OE, P_A, P_B, P_C);

uint8_t  display_draw_time = 30;
uint16_t C_BLACK, C_BLUE, C_RED;

// ── Polled scan (safe with ESP-NOW — no timer ISR) ────────────────────────────
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

// ── BoardData (must match master exactly) ─────────────────────────────────────
typedef struct __attribute__((packed)) {
    char eventName[32];
    char teamA[16];
    char teamB[16];
    int  scoreA, scoreB, clockSecs, clockTenths, quarter;
    char possession;
    int  foulsA, foulsB, timeoutsA, timeoutsB, screenMask;
    int  clockRunning, shotSecs, shotTenths, shotRunning, eventScroll;
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

// ── Draw top row ──────────────────────────────────────────────────────────────
//
//  Size 4 character = 24 px wide × 32 px tall.
//  This panel is 16 px tall, so only the TOP 16 px of the character are visible.
//  Slave 4 draws the same character at y = -16 to show the BOTTOM 16 px.
//
//  Centering:
//    ≥ 10 s  →  "24" : 2 × 24 = 48 px  →  x = (64 - 48) / 2 = 8
//    <  10 s  →  "9"  : 1 × 24 = 24 px  →  x = (64 - 24) / 2 = 20
void drawTop(int secs, int tenths) {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);

    uint16_t col = (secs <= 5) ? C_RED : C_BLUE;

    // "SC" label — size 1, top-left corner
    display.setTextSize(1);
    display.setTextColor(col);
    display.setCursor(0, 0);
    display.print("SC");

    // Large digit — size 3, top half visible on this panel
    display.setTextSize(3);
    display.setTextColor(col);

    char buf[3];
    snprintf(buf, sizeof(buf), "%d", secs);   // "24" or "9"

    int tw = (int)strlen(buf) * 18;           // 18 px per char at size 3
    int tx = (64 - tw) / 2;
    display.setCursor(tx, 0);
    display.print(buf);
}

// ── WAIT screen ───────────────────────────────────────────────────────────────
void showWait() {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);
    display.setTextSize(1);
    display.setTextColor(C_BLUE);
    display.setCursor(20, 4);
    display.print("WAIT");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    delay(1000);
    Serial.begin(115200);

    display.begin(8);
    delay(100);

    C_BLACK = display.color565(  0,   0,   0);
    C_BLUE  = display.color565(  0,   0, 255);  // swap R & B values if red appears
    C_RED   = display.color565(255,   0,   0);  // swap R & B values if blue appears

    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);

    showWait();

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("Slave3 MAC: ");
    Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 3 v10.0 ready — 64x16 top row");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();

    if (!newData) return;
    newData = false;

    if (!connected) {
        connected = true;
        Serial.println("Master connected");
    }

    drawTop(rxBuf.shotSecs, rxBuf.shotTenths);
}
