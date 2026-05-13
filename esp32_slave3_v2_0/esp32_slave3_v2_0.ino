/*
  Slave 3 v2.0 — Fouls A | Shot Clock | Fouls B
  ─────────────────────────────────────────────────────────────────────────
  Hardware : 6 panels wide × 2 tall  (192 × 32 px, RGB P10)

  Scan method : polled from loop() via micros()  ← same as Slave 1.
                No timer ISR, no FreeRTOS task.
                Eliminates ESP-NOW + timer-ISR flash conflict.

  Zone layout (64 px each):
    x=  0.. 63 : Team A Fouls  — "FOULS" label + number (GREEN → RED at max)
    x= 64..127 : Shot Clock    — "SHOT CLOCK" label + SS or S.t (CYAN)
    x=128..191 : Team B Fouls  — "FOULS" label + number (GREEN → RED at max)

  Each zone (64 × 32 px):
    y= 0..  7 : label  textSize=1 (6×8 px)
    y= 8.. 31 : number textSize=3 (18×24 px), centred
    Shot clock <10s → seconds sz=3 at y=8, tenths sz=2 at y=16
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

PxMATRIX display(192, 32, P_LAT, P_OE, P_A, P_B, P_C);
uint8_t display_draw_time = 40;

// Colors defined globally (R↔B swap for this hardware)
uint16_t C_BLACK  = display.color565(  0,   0,   0);
uint16_t C_GREEN  = display.color565(  0, 255,   0);
uint16_t C_RED    = display.color565(  0,   0, 255);   // R<->B swap
uint16_t C_YELLOW = display.color565(  0, 255, 255);   // R<->B swap
uint16_t C_CYAN   = display.color565(255, 255,   0);   // R<->B swap

// ── Polled scan — same pattern as Slave 1 scanDisplayBySPI() ─────────────────
unsigned long lastScan = 0;
void scanIfNeeded() {
    unsigned long now = micros();
    if ((long)(now - lastScan) >= 1500) {
        display.display(display_draw_time);
        lastScan = now;
    }
}

// Blocking delay that keeps the display scanning
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

int lastFoulsA     = -1, lastFoulsB     = -1;
int lastShotSecs   = -1, lastShotTenths = -1;

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) { memcpy(&rxBuf, data, sizeof(rxBuf)); newData = true; }
}

// ── Zone helpers ──────────────────────────────────────────────────────────────
#define ZONE_W    64
#define FOULS_A_X  0
#define CLOCK_X   64
#define FOULS_B_X 128
#define FOULS_MAX  5

void clearZone(int zoneX) {
    display.fillRect(zoneX, 0, ZONE_W, 32, C_BLACK);
}

void drawCentred(const char* s, int zoneX, int zoneW, int y, uint16_t color, uint8_t sz) {
    int w = (int)strlen(s) * 6 * sz;
    display.setTextSize(sz);
    display.setTextColor(color);
    display.setTextWrap(false);
    display.setCursor(zoneX + max(0, (zoneW - w) / 2), y);
    display.print(s);
}

// ── Fouls zone ────────────────────────────────────────────────────────────────
void drawFoulsZone(int zoneX, int fouls) {
    clearZone(zoneX);
    drawCentred("FOULS", zoneX, ZONE_W, 0, C_YELLOW, 1);
    uint16_t col = (fouls >= FOULS_MAX) ? C_RED : C_GREEN;
    char buf[4]; snprintf(buf, sizeof(buf), "%d", fouls);
    drawCentred(buf, zoneX, ZONE_W, 8, col, 3);
}

// ── Shot clock zone ───────────────────────────────────────────────────────────
void drawShotClockZone(int secs, int tenths) {
    clearZone(CLOCK_X);
    drawCentred("SHOT CLOCK", CLOCK_X, ZONE_W, 0, C_CYAN, 1);

    if (secs >= 10) {
        char buf[4]; snprintf(buf, sizeof(buf), "%d", secs);
        drawCentred(buf, CLOCK_X, ZONE_W, 8, C_CYAN, 3);
    } else {
        char ss[3]; snprintf(ss, sizeof(ss), "%d", secs);
        char tt[4]; snprintf(tt, sizeof(tt), ".%d", tenths);
        int wSS    = 6 * 3;                         // 18 px (1 digit × size 3)
        int wTT    = (int)strlen(tt) * 6 * 2;        // 24 px (size 2)
        int totalW = wSS + wTT;
        int x0     = CLOCK_X + max(0, (ZONE_W - totalW) / 2);
        display.setTextSize(3); display.setTextColor(C_CYAN); display.setTextWrap(false);
        display.setCursor(x0, 8);
        display.print(ss);
        display.setTextSize(2);
        display.setCursor(x0 + wSS, 16);
        display.print(tt);
    }
}

// ── WAIT startup — each 32×16 panel shows "WAIT" ────────────────────────────
void showWait() {
    display.clearDisplay();
    uint16_t pal[6] = { C_GREEN, C_YELLOW, C_CYAN, C_RED, C_GREEN, C_YELLOW };
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 6; col++) {
            display.setTextSize(1);
            display.setTextColor(pal[col % 6]);
            display.setTextWrap(false);
            display.setCursor(col * 32 + 4, row * 16 + 4);
            display.print("WAIT");
        }
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    delay(1000);
    Serial.begin(115200);

    display.begin(8);   // 1/8 scan — try begin(4) if nothing shows
    delay(100);

    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);

    // Splash — use waitMs() so display scans while waiting
    drawCentred("SLAVE 3", 0, 192, 8, C_GREEN, 2);
    waitMs(1500);

    showWait();
    // Give showWait a moment to render before WiFi init starts
    waitMs(200);

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
        display.clearDisplay();
        drawCentred("ESP-NOW FAIL", 0, 192, 12, C_RED, 1);
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 3 v2.0 ready — 192x32");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();   // keeps display refreshing — same role as Slave 1's scanIfNeeded()

    if (!newData) return;
    newData = false;

    if (!connected) {
        connected = true;
        display.clearDisplay();
        lastFoulsA = lastFoulsB = lastShotSecs = lastShotTenths = -1;
        Serial.println("Master connected");
    }

    if (rxBuf.foulsA != lastFoulsA) {
        lastFoulsA = rxBuf.foulsA;
        drawFoulsZone(FOULS_A_X, rxBuf.foulsA);
        Serial.printf("Fouls A: %d\n", rxBuf.foulsA);
    }
    if (rxBuf.foulsB != lastFoulsB) {
        lastFoulsB = rxBuf.foulsB;
        drawFoulsZone(FOULS_B_X, rxBuf.foulsB);
        Serial.printf("Fouls B: %d\n", rxBuf.foulsB);
    }
    if (rxBuf.shotSecs != lastShotSecs ||
        (rxBuf.shotSecs < 10 && rxBuf.shotTenths != lastShotTenths)) {
        lastShotSecs   = rxBuf.shotSecs;
        lastShotTenths = rxBuf.shotTenths;
        drawShotClockZone(rxBuf.shotSecs, rxBuf.shotTenths);
    }
}
