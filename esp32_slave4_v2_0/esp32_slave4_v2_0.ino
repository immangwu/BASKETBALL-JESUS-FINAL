/*
  ╔══════════════════════════════════════════════════════════════╗
  ║  Slave 4  v2.0  —  Shot Clock  BOTTOM row  (number)         ║
  ║  Hardware : 2 × P10 panels in one row  →  64 × 16 px        ║
  ╠══════════════════════════════════════════════════════════════╣
  ║  Displays shot clock value, size 2 (12×16 px per char):     ║
  ║    ≥ 10 s  →  "XX"   e.g. "24"                             ║
  ║    <  10 s  →  "X.X"  e.g. "2.2"  (matches reference)     ║
  ║                                                              ║
  ║  Colour:                                                     ║
  ║    shot > 5 s  →  BLUE                                      ║
  ║    shot ≤ 5 s  →  RED  (urgent)                            ║
  ║                                                              ║
  ║  Combined with Slave 3 (top row):                           ║
  ║   ┌──────────────────────────────────┐                      ║
  ║   │         SHOT CLOCK               │  ← Slave 3           ║
  ║   │              2.2                 │  ← Slave 4 (this)    ║
  ║   └──────────────────────────────────┘                      ║
  ║                                                              ║
  ║  Scan : polled from loop() — timer ISR removed to prevent   ║
  ║         ESP-NOW + ISR flash conflict                        ║
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

// ── BoardData  (must match master exactly) ────────────────────────────────────
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

// ── Draw number row ───────────────────────────────────────────────────────────
//   size 2  →  12 px wide × 16 px tall per character (fills full 16 px height)
//
//   ≥ 10 s : "24"  →  2 chars × 12 px = 24 px  →  centre x = (64-24)/2 = 20
//   <  10 s : "2.2" →  3 chars × 12 px = 36 px  →  centre x = (64-36)/2 = 14
void drawNumber(int secs, int tenths) {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);
    display.setTextSize(2);
    display.setTextColor(C_RED);

    char buf[6];
    if (secs > 5) {
        snprintf(buf, sizeof(buf), "%d",     secs);          // "24"
    } else {
        snprintf(buf, sizeof(buf), "%d.%d",  secs, tenths);  // "2.2"
    }

    int tx = (64 - (int)strlen(buf) * 12) / 2;
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
    C_BLUE  = display.color565(  0,   0, 255);  // swap to (255,0,0) if red appears
    C_RED   = display.color565(255,   0,   0);  // swap to (0,0,255) if blue appears

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
        Serial.println("Slave 4 v2.0 ready — 64x16 number row");
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

    drawNumber(rxBuf.shotSecs, rxBuf.shotTenths);
}
