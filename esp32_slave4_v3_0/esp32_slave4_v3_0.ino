/*
  ╔══════════════════════════════════════════════════════════════╗
  ║  Slave 4  v3.0  —  Shot Clock  BOTTOM row                   ║
  ║  Hardware : 2 × P10 panels in one row  →  64 × 16 px        ║
  ╠══════════════════════════════════════════════════════════════╣
  ║  Layout:                                                     ║
  ║   ┌──────────────────────────────────────┐                  ║
  ║   │SC  ┌──────────────────────────────┐  │  ← Slave 3 TOP  ║
  ║   │    │   TOP HALF of large digit    │  │                  ║
  ║   └────┴──────────────────────────────┴──┘                  ║
  ║   ┌────┬──────────────────────────────┬──┐                  ║
  ║   │    │  BOTTOM HALF of large digit  │.X│  ← Slave 4 BOT  ║
  ║   └────┴──────────────────────────────┴──┘    (this file)   ║
  ║                                                              ║
  ║  Large digit: size 4, cursor y = -16 → PxMatrix clips the   ║
  ║               top 16 px, leaving BOTTOM 16 px visible here  ║
  ║                                                              ║
  ║    ≥ 10 s : "24"  x=8,  y=-16                             ║
  ║    <  10 s : "9"   x=20, y=-16  +  ".X" at bottom-right    ║
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

// ── Draw bottom row ───────────────────────────────────────────────────────────
//
//  Size 4 character = 24 px wide × 32 px tall.
//  Setting cursor y = -16 shifts the character up by 16 px.
//  PxMatrix discards pixels with y < 0, so only the BOTTOM 16 px of the
//  character (original rows 16..31) appear on this 16 px panel.
//
//  < 10 s : also draw ".X" tenths subscript at bottom-right corner.
//    ".X" at size 1 = 12 px wide × 8 px tall → x = 64-12-2 = 50, y = 8
void drawBottom(int secs, int tenths) {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);

    uint16_t col = (secs <= 5) ? C_RED : C_BLUE;

    // Bottom half of large digit
    display.setTextSize(3);
    display.setTextColor(col);

    char buf[3];
    snprintf(buf, sizeof(buf), "%d", secs);   // "24" or "9"

    int tw = (int)strlen(buf) * 18;           // 18 px per char at size 3
    int tx = (64 - tw) / 2;
    display.setCursor(tx, -16);               // shift up 16 px → bottom 8 px visible
    display.print(buf);

    // Tenths subscript — only when < 10 s
    if (secs < 10) {
        char sub[3];
        snprintf(sub, sizeof(sub), ".%d", tenths);   // ".2"
        display.setTextSize(2);               // size 2 = 12x16 px, full panel height
        display.setTextColor(col);
        display.setCursor(40, 0);             // right of digit, full height
        display.print(sub);
    }
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
    Serial.print("Slave4 MAC: ");
    Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 4 v3.0 ready — 64x16 bottom row");
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

    drawBottom(rxBuf.shotSecs, rxBuf.shotTenths);
}
