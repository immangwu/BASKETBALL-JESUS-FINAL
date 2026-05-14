/*
  ╔══════════════════════════════════════════════════════════════╗
  ║  Slave 3  v9.0  —  Shot Clock  TOP row  (label)             ║
  ║  Hardware : 2 × P10 panels in one row  →  64 × 16 px        ║
  ╠══════════════════════════════════════════════════════════════╣
  ║  Displays "SHOT CLOCK" label, same colour as Slave 4:       ║
  ║    shot > 5 s  →  BLUE                                      ║
  ║    shot ≤ 5 s  →  RED  (urgent)                            ║
  ║                                                              ║
  ║  Combined with Slave 4 (bottom row):                        ║
  ║   ┌──────────────────────────────────┐                      ║
  ║   │         SHOT CLOCK               │  ← Slave 3 (this)   ║
  ║   │              2.2                 │  ← Slave 4           ║
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
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ── Draw label row ────────────────────────────────────────────────────────────
//   "SHOT CLOCK" = 10 chars × 6 px = 60 px at size 1
//   Centre in 64 px → x = (64-60)/2 = 2
//   Centre vertically in 16 px (font 8 px tall) → y = (16-8)/2 = 4
void drawLabel(int secs) {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);
    display.setTextSize(1);
    display.setTextColor((secs <= 5) ? C_RED : C_BLUE);
    display.setCursor(2, 4);
    display.print("SHOT CLOCK");
}

// ── WAIT screen ───────────────────────────────────────────────────────────────
//   "WAIT" = 4 chars × 6 px = 24 px  →  centre x = (64-24)/2 = 20
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
    Serial.print("Slave3 MAC: ");
    Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 3 v9.0 ready — 64x16 label row");
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

    drawLabel(rxBuf.shotSecs);
}
