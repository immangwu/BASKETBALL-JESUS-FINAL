/*
  ╔══════════════════════════════════════════════════════════════╗
  ║  Slave 3  v8.0  —  Shot Clock TOP panel                     ║
  ║  Hardware : 2 rows × 1 col of 32×16 panels  →  32×32 px    ║
  ╠══════════════════════════════════════════════════════════════╣
  ║  DISPLAY LOGIC                                               ║
  ║    shot ≥ 10 s  →  show TENS digit   (e.g. "2" from "24")  ║
  ║    shot <  10 s  →  show main DIGIT  (e.g. "9" from "9.5") ║
  ║                                                              ║
  ║  COLOR                                                       ║
  ║    > 5 s  →  BLUE                                           ║
  ║    ≤ 5 s  →  RED  (urgent warning)                         ║
  ║                                                              ║
  ║  FONT  :  size 3  (18 × 24 px per char)                    ║
  ║  SCAN  :  polled from loop() via micros() — no timer ISR   ║
  ╚══════════════════════════════════════════════════════════════╝
*/

#define PxMATRIX_SPI_FREQUENCY 20000000
#include <PxMatrix.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

// ── Pin definitions ───────────────────────────────────────────────────────────
#define P_LAT  5
#define P_A   19
#define P_B   23
#define P_C   18
#define P_OE   4

// 2 rows × 1 col of 32×16 panels = 32 wide × 32 tall
PxMATRIX display(32, 32, P_LAT, P_OE, P_A, P_B, P_C);

// Colours — initialised after display.begin()
uint16_t C_BLACK, C_BLUE, C_RED;

// ── Polled scan (safe with ESP-NOW — no timer ISR) ────────────────────────────
static unsigned long lastScan = 0;
void scanIfNeeded() {
    unsigned long now = micros();
    if ((long)(now - lastScan) >= 2000) {
        display.display(40);
        lastScan = now;
    }
}
void waitMs(long ms) {
    long end = (long)millis() + ms;
    while ((long)(millis() - end) < 0) scanIfNeeded();
}

// ── BoardData — must match master struct exactly ──────────────────────────────
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

// ── Draw digit (Slave 3 = TOP half of combined display) ──────────────────────
//
//   size 3  →  18 px wide × 24 px tall per character
//   Centre horizontally : x = (32 - 18) / 2 = 7
//   Anchor vertically   : y = 4  →  fills y = 4..27  (24 px centred in 32 px)
//
//   Colour:  BLUE when shotSecs > 5,  RED when shotSecs ≤ 5
void drawTop(int secs, int tenths) {
    display.fillRect(0, 0, 32, 32, C_BLACK);
    display.setTextWrap(false);
    display.setTextSize(3);

    uint16_t col = (secs <= 5) ? C_RED : C_BLUE;
    display.setTextColor(col);

    char buf[3];
    if (secs >= 10) snprintf(buf, sizeof(buf), "%d", secs / 10);   // tens digit
    else            snprintf(buf, sizeof(buf), "%d", secs);          // single digit

    // Centre 18 px wide char horizontally in 32 px panel
    display.setCursor(7, 4);
    display.print(buf);
}

// ── WAIT screen ───────────────────────────────────────────────────────────────
void showWait() {
    display.fillRect(0, 0, 32, 32, C_BLACK);
    display.setTextWrap(false);
    display.setTextSize(1);
    display.setTextColor(C_BLUE);
    display.setCursor(2, 0);   display.print("WAIT");
    display.setCursor(2, 16);  display.print("WAIT");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    delay(500);
    Serial.begin(115200);

    display.begin(8);
    delay(100);

    // R↔B hardware swap on these panels:
    //   colour565(255, 0, 0)  →  appears BLUE
    //   colour565(  0, 0, 255) →  appears RED
    C_BLACK = display.color565(  0,   0,   0);
    C_BLUE  = display.color565(255,   0,   0);
    C_RED   = display.color565(  0,   0, 255);

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
        Serial.println("Slave 3 v8.0 ready — 32x32 top panel");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();          // must stay first — keeps display alive

    if (!newData) return;
    newData = false;

    if (!connected) {
        connected = true;
        Serial.println("Master connected");
    }

    drawTop(rxBuf.shotSecs, rxBuf.shotTenths);
}
