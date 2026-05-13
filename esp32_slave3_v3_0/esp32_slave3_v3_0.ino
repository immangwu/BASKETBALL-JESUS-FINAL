/*
  Slave 3 v3.0 — 64×32 (1 row × 2 columns of P10 RGB panels)

  Startup  : "WAIT" on all panels until first ESP-NOW packet arrives.
  Connected: three horizontal zones

    x=  0..15  (16 px) : Team A Fouls  — label "A",  number GREEN → RED at max
    x= 16..47  (32 px) : Shot Clock    — label "SC", number CYAN
    x= 48..63  (16 px) : Team B Fouls  — label "B",  number GREEN → RED at max

  Row mapping (P10 scan):
    buffer y=16..31 → physical TOP row    (labels, size 1)
    buffer y= 0..15 → physical BOTTOM row (numbers, size 2)

  Scan: polled from loop() every 1500 µs — no timer ISR, safe with ESP-NOW.
  ─────────────────────────────────────────────────────────────────────────────
  If nothing shows: change display.begin(8) → display.begin(4) in setup().
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

PxMATRIX display(64, 32, P_LAT, P_OE, P_A, P_B, P_C);
uint8_t display_draw_time = 40;

uint16_t C_BLACK, C_GREEN, C_RED, C_YELLOW, C_CYAN;

// ── Polled scan — keeps display alive from loop(), safe with ESP-NOW ──────────
unsigned long lastScan = 0;
void scanIfNeeded() {
    unsigned long now = micros();
    if ((long)(now - lastScan) >= 1500) {
        display.display(display_draw_time);
        lastScan = now;
    }
}

void waitMs(long ms) {
    long end = millis() + ms;
    while ((long)(millis() - end) < 0) scanIfNeeded();
}

// ── BoardData (must match master exactly) ────────────────────────────────────
typedef struct __attribute__((packed)) {
    char eventName[32]; char teamA[16]; char teamB[16];
    int  scoreA, scoreB, clockSecs, clockTenths, quarter;
    char possession;
    int  foulsA, foulsB, timeoutsA, timeoutsB, screenMask;
    int  clockRunning, shotSecs, shotTenths, shotRunning, eventScroll;
} BoardData;

BoardData     rxBuf;
volatile bool newData   = false;
bool          connected = false;

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) { memcpy(&rxBuf, data, sizeof(rxBuf)); newData = true; }
}

// ── Zone geometry ─────────────────────────────────────────────────────────────
#define FOULS_A_X   0
#define FOULS_A_W  16
#define CLOCK_X    16
#define CLOCK_W    32
#define FOULS_B_X  48
#define FOULS_B_W  16
#define LABEL_Y    16   // buffer y → physical TOP row
#define NUM_Y       0   // buffer y → physical BOTTOM row (size 2 = 16 px tall, fits 0..15)
#define FOULS_MAX   5

// ── Drawing helpers ───────────────────────────────────────────────────────────
void clearZone(int x, int w) {
    display.fillRect(x, 0, w, 32, C_BLACK);
}

void drawLabel(int zoneX, int zoneW, const char* s, uint16_t color) {
    int tw = (int)strlen(s) * 6;
    display.setTextSize(1);
    display.setTextColor(color);
    display.setTextWrap(false);
    display.setCursor(zoneX + max(0, (zoneW - tw) / 2), LABEL_Y);
    display.print(s);
}

void drawNum2(int zoneX, int zoneW, const char* s, uint16_t color) {
    int tw = (int)strlen(s) * 12;   // size 2 → 12 px per char
    display.setTextSize(2);
    display.setTextColor(color);
    display.setTextWrap(false);
    display.setCursor(zoneX + max(0, (zoneW - tw) / 2), NUM_Y);
    display.print(s);
}

// ── Fouls zone ────────────────────────────────────────────────────────────────
void drawFouls(int zoneX, int zoneW, const char* label, int fouls) {
    clearZone(zoneX, zoneW);
    drawLabel(zoneX, zoneW, label, C_YELLOW);
    uint16_t col = (fouls >= FOULS_MAX) ? C_RED : C_GREEN;
    char buf[4]; snprintf(buf, sizeof(buf), "%d", fouls);
    drawNum2(zoneX, zoneW, buf, col);
}

// ── Shot clock zone ───────────────────────────────────────────────────────────
void drawShotClock(int secs, int tenths) {
    clearZone(CLOCK_X, CLOCK_W);
    drawLabel(CLOCK_X, CLOCK_W, "SC", C_CYAN);

    if (secs >= 10) {
        char buf[4]; snprintf(buf, sizeof(buf), "%d", secs);
        drawNum2(CLOCK_X, CLOCK_W, buf, C_CYAN);
    } else {
        // Large seconds (size 2) + tenths subscript (size 1) side by side
        char ss[3]; snprintf(ss, sizeof(ss), "%d", secs);
        char tt[4]; snprintf(tt, sizeof(tt), ".%d", tenths);
        int totalW = 12 + (int)strlen(tt) * 6;   // 12px (size-2 digit) + 6px×chars
        int x0 = CLOCK_X + max(0, (CLOCK_W - totalW) / 2);
        display.setTextSize(2); display.setTextColor(C_CYAN); display.setTextWrap(false);
        display.setCursor(x0, NUM_Y);
        display.print(ss);
        display.setTextSize(1);
        display.setCursor(x0 + 12, NUM_Y + 8);   // subscript baseline
        display.print(tt);
    }
}

// ── WAIT screen — shows on both physical rows ─────────────────────────────────
void showWait() {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextWrap(false);
    display.setTextColor(C_CYAN);
    display.setCursor(8, 0);    // physical BOTTOM row
    display.print("WAIT");
    display.setCursor(8, 16);   // physical TOP row
    display.print("WAIT");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    delay(1000);
    Serial.begin(115200);

    display.begin(8);   // ← change to begin(4) if nothing shows
    delay(100);

    C_BLACK  = display.color565(  0,   0,   0);
    C_GREEN  = display.color565(  0, 255,   0);
    C_RED    = display.color565(  0,   0, 255);   // R<->B swap
    C_YELLOW = display.color565(  0, 255, 255);   // R<->B swap
    C_CYAN   = display.color565(255, 255,   0);   // R<->B swap

    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);

    // Splash
    display.setTextSize(1);
    display.setTextColor(C_GREEN);
    display.setCursor(4, 16);
    display.print("S3 v3.0");
    waitMs(1200);

    showWait();
    waitMs(200);

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(C_RED);
        display.setCursor(4, 16);
        display.print("ESP FAIL");
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 3 v3.0 ready — 64x32, waiting for master...");
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
        Serial.println("Master connected — showing live data");
    }

    drawFouls(FOULS_A_X, FOULS_A_W, "A", rxBuf.foulsA);
    drawShotClock(rxBuf.shotSecs, rxBuf.shotTenths);
    drawFouls(FOULS_B_X, FOULS_B_W, "B", rxBuf.foulsB);
}
