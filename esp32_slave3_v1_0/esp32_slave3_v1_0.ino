/*
  Slave 3 v1.0 — Fouls | Shot Clock | Fouls | Quarter  (RGB P10 — PxMatrix)
  ─────────────────────────────────────────────────────────────────────────
  Hardware : 8 panels wide × 2 tall  (256 × 32 px)

  Zone layout (64 px each):
    x=  0.. 63 : Team A Fouls   — "FOULS" label + number (GREEN → RED at max)
    x= 64..127 : Shot Clock     — "SHOT CLOCK" label + SS or S.t (CYAN)
    x=128..191 : Team B Fouls   — "FOULS" label + number (GREEN → RED at max)
    x=192..255 : Quarter        — "QUARTER" label + number (GREEN)

  FIX: Timer ISR + ESP-NOW causes flash-cache crash.
       Replaced with FreeRTOS task on core 0 — safe alongside WiFi/ESP-NOW.

  Startup: each individual 32×16 panel shows "WAIT" in a different colour.
  On first ESP-NOW packet: WAIT clears and live data fills all zones.
*/

#define PxMATRIX_SPI_FREQUENCY 2000000
#include <PxMatrix.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

// ── Panel pins ────────────────────────────────────────────────────────────────
#define P_LAT  5
#define P_A   19
#define P_B   23
#define P_C   18
#define P_OE   4

// ── Display: 8 panels wide × 2 tall = 256 × 32 px ────────────────────────────
PxMATRIX display(256, 32, P_LAT, P_OE, P_A, P_B, P_C);

uint8_t display_draw_time = 30;

// ── Colors (R<->B swapped) — set in setup() after display.begin() ─────────────
uint16_t C_BLACK, C_GREEN, C_RED, C_YELLOW, C_CYAN, C_ORANGE;

// ── FreeRTOS display task — replaces timer ISR to avoid WiFi flash conflict ───
void displayTask(void* pvParameters) {
    for (;;) {
        display.display(display_draw_time);
        vTaskDelay(1);   // yield to other tasks, ~1 ms
    }
}

// ── Layout ────────────────────────────────────────────────────────────────────
#define ZONE_W     64
#define FOULS_A_X   0
#define CLOCK_X    64
#define FOULS_B_X 128
#define QUART_X   192
#define FOULS_MAX   5    // number turns RED at or above this

// Individual panel size (for WAIT startup pattern)
#define PANEL_W    32
#define PANEL_H    16
#define PANELS_COL  8
#define PANELS_ROW  2

// ── Data struct (must match master exactly) ───────────────────────────────────
typedef struct __attribute__((packed)) {
    char eventName[32];
    char teamA[16];
    char teamB[16];
    int  scoreA,    scoreB;
    int  clockSecs, clockTenths;
    int  quarter;
    char possession;
    int  foulsA,    foulsB;
    int  timeoutsA, timeoutsB;
    int  screenMask;
    int  clockRunning;
    int  shotSecs,  shotTenths;
    int  shotRunning;
    int  eventScroll;
} BoardData;

BoardData     rxBuf;
volatile bool newData   = false;
bool          connected = false;

int lastFoulsA     = -1, lastFoulsB     = -1;
int lastShotSecs   = -1, lastShotTenths = -1;
int lastQuarter    = -1;

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ── Draw helpers ──────────────────────────────────────────────────────────────
void clearZone(int zoneX) {
    display.fillRect(zoneX, 0, ZONE_W, 32, C_BLACK);
}

// Centre text in a zone of width zoneW starting at zoneX.
// Adafruit GFX built-in font: char width = 6*sz, height = 8*sz.
void drawCentred(const char* s, int zoneX, int zoneW,
                 int y, uint16_t color, uint8_t sz) {
    int w = (int)strlen(s) * 6 * sz;
    display.setTextSize(sz);
    display.setTextColor(color);
    display.setTextWrap(false);
    display.setCursor(zoneX + max(0, (zoneW - w) / 2), y);
    display.print(s);
}

// ── WAIT startup: each 32×16 panel shows "WAIT" in a cycling colour ──────────
void showWaitPanels() {
    display.clearDisplay();
    // 6 colours, one per panel column (repeats for row 2)
    uint16_t pal[6] = { C_GREEN, C_YELLOW, C_CYAN, C_RED, C_ORANGE, C_GREEN };
    // "WAIT" textSize=1: 4×6=24 px wide, 8 px tall
    // Centre in 32×16 panel: x+4, y+4
    for (int row = 0; row < PANELS_ROW; row++) {
        for (int col = 0; col < PANELS_COL; col++) {
            display.setTextSize(1);
            display.setTextColor(pal[col % 6]);
            display.setTextWrap(false);
            display.setCursor(col * PANEL_W + 4, row * PANEL_H + 4);
            display.print("WAIT");
        }
    }
}

// ── Fouls zone ────────────────────────────────────────────────────────────────
void drawFoulsZone(int zoneX, int fouls) {
    clearZone(zoneX);
    drawCentred("FOULS", zoneX, ZONE_W, 0, C_YELLOW, 1);
    uint16_t col = (fouls >= FOULS_MAX) ? C_RED : C_GREEN;
    char buf[4]; snprintf(buf, sizeof(buf), "%d", fouls);
    // textSize=3 → 24 px tall, y=8..31
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
        // Large seconds + small tenths at lower baseline
        char ss[3]; snprintf(ss, sizeof(ss), "%d", secs);
        char tt[4]; snprintf(tt, sizeof(tt), ".%d", tenths);
        int wSS    = 1 * 6 * 3;                    // 18 px (1 digit × size 3)
        int wTT    = (int)strlen(tt) * 6 * 2;       // 24 px (size 2)
        int totalW = wSS + wTT;
        int x0     = CLOCK_X + max(0, (ZONE_W - totalW) / 2);
        display.setTextSize(3);
        display.setTextColor(C_CYAN);
        display.setTextWrap(false);
        display.setCursor(x0, 8);
        display.print(ss);
        // Tenths at y=16 → bottom aligns at y=32 (lower baseline, ~40% of zone)
        display.setTextSize(2);
        display.setCursor(x0 + wSS, 16);
        display.print(tt);
    }
}

// ── Quarter zone ──────────────────────────────────────────────────────────────
void drawQuarterZone(int quarter) {
    clearZone(QUART_X);
    drawCentred("QUARTER", QUART_X, ZONE_W, 0, C_YELLOW, 1);
    char buf[4]; snprintf(buf, sizeof(buf), "%d", quarter);
    drawCentred(buf, QUART_X, ZONE_W, 8, C_GREEN, 3);
}

// ── Check for new data ────────────────────────────────────────────────────────
void checkNewData() {
    if (!newData) return;
    newData = false;

    // First packet — clear WAIT screen and force all zones to draw
    if (!connected) {
        connected = true;
        display.clearDisplay();
        lastFoulsA = lastFoulsB = lastShotSecs = lastShotTenths = lastQuarter = -1;
        Serial.println("Master connected — drawing all zones");
    }

    bool foulsAChanged  = (rxBuf.foulsA   != lastFoulsA);
    bool foulsBChanged  = (rxBuf.foulsB   != lastFoulsB);
    bool shotChanged    = (rxBuf.shotSecs != lastShotSecs) ||
                          (rxBuf.shotSecs < 10 && rxBuf.shotTenths != lastShotTenths);
    bool quarterChanged = (rxBuf.quarter  != lastQuarter);

    if (foulsAChanged) {
        lastFoulsA = rxBuf.foulsA;
        drawFoulsZone(FOULS_A_X, rxBuf.foulsA);
        Serial.printf("Fouls A: %d\n", rxBuf.foulsA);
    }
    if (shotChanged) {
        lastShotSecs   = rxBuf.shotSecs;
        lastShotTenths = rxBuf.shotTenths;
        drawShotClockZone(rxBuf.shotSecs, rxBuf.shotTenths);
    }
    if (foulsBChanged) {
        lastFoulsB = rxBuf.foulsB;
        drawFoulsZone(FOULS_B_X, rxBuf.foulsB);
        Serial.printf("Fouls B: %d\n", rxBuf.foulsB);
    }
    if (quarterChanged) {
        lastQuarter = rxBuf.quarter;
        drawQuarterZone(rxBuf.quarter);
        Serial.printf("Quarter: %d\n", rxBuf.quarter);
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    delay(1000);
    Serial.begin(115200);

    display.begin(4);   // 1/4 scan — most P10 RGB panels; change to 8 if still wrong
    delay(100);

    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);

    // Init colors BEFORE any drawing
    C_BLACK  = display.color565(  0,   0,   0);
    C_GREEN  = display.color565(  0, 255,   0);
    C_RED    = display.color565(  0,   0, 255);   // R<->B swap
    C_YELLOW = display.color565(  0, 255, 255);   // R<->B swap
    C_CYAN   = display.color565(255, 255,   0);   // R<->B swap
    C_ORANGE = display.color565(  0, 165, 255);   // R<->B swap (orange)

    // Start display refresh task on core 0 — avoids ISR+WiFi flash conflict
    xTaskCreatePinnedToCore(displayTask, "disp", 2048, NULL, 2, NULL, 0);
    delay(100);

    // Splash
    drawCentred("SLAVE 3", 0, 256, 8, C_GREEN, 2);   // 256-wide, textSize=2
    delay(1500);

    // WAIT on each individual panel with different colours
    showWaitPanels();

    // WiFi + ESP-NOW
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());

    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init FAILED");
        display.clearDisplay();
        drawCentred("ESP-NOW FAIL", 0, 256, 12, C_RED, 1);
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 3 v1.0 ready — waiting for master");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    checkNewData();
}
