/*
  ╔══════════════════════════════════════════════════════════════╗
  ║  Slave 6  v1.0  —  Fouls Team A  BOTTOM row                 ║
  ║  Hardware : 2 × P10 panels in one row  →  64 × 16 px        ║
  ╠══════════════════════════════════════════════════════════════╣
  ║  Layout (combined with Slave 5):                            ║
  ║   ┌──────────────────────────────────────┐                  ║
  ║   │FA  ┌──────────────────────────────┐  │  ← Slave 5 TOP  ║
  ║   │    │   TOP HALF of foul digit     │  │                  ║
  ║   └────┴──────────────────────────────┴──┘                  ║
  ║   ┌────┬──────────────────────────────┬──┐                  ║
  ║   │    │  BOTTOM HALF of foul digit   │  │  ← Slave 6 BOT  ║
  ║   └────┴──────────────────────────────┴──┘    (this file)   ║
  ║                                                              ║
  ║  Foul digit : size 3 (18×24 px), cursor y=-16               ║
  ║               PxMatrix clips top 16 px → bottom 8 px shown ║
  ║                                                              ║
  ║  COLOR                                                       ║
  ║    foulsA < 5  →  GREEN                                     ║
  ║    foulsA ≥ 5  →  RED  (foul limit reached)                ║
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

#define FOULS_MAX 5

// 2 panels in a single row = 64 wide × 16 tall
PxMATRIX display(64, 16, P_LAT, P_OE, P_A, P_B, P_C);

uint8_t  display_draw_time = 30;
uint16_t C_BLACK, C_GREEN, C_RED;

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
    if (len == 1 && data[0] == 0xAA) { ESP.restart(); return; }
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ── Draw bottom row ───────────────────────────────────────────────────────────
//   Full digit, size 2 (12×16 px) — fills panel height exactly.
//   fouls 0-4 : GREEN    fouls ≥ 5 : RED
void drawBottom(int fouls) {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);
    display.setTextSize(2);

    uint16_t col = (fouls >= FOULS_MAX) ? C_RED : C_GREEN;
    display.setTextColor(col);

    char buf[3];
    snprintf(buf, sizeof(buf), "%d", fouls);  // "0".."5"

    int tw = (int)strlen(buf) * 12;           // 12 px per char at size 2
    int tx = (64 - tw) / 2;
    display.setCursor(tx, 0);
    display.print(buf);
}

// ── WAIT screen ───────────────────────────────────────────────────────────────
void showWait() {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);
    display.setTextSize(1);
    display.setTextColor(C_GREEN);
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
    C_GREEN = display.color565(  0, 255,   0);  // green is swap-safe
    C_RED   = display.color565(255,   0,   0);  // swap to (0,0,255) if blue appears

    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);

    showWait();

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("Slave6 MAC: ");
    Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 6 v1.0 ready — 64x16 Fouls-A bottom row");
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

    drawBottom(rxBuf.foulsA);
}
