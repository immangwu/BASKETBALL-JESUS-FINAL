/*
  Slave 3 — 4-Panel Test  (64×32 px : 2 cols × 2 rows)

  PxMatrix row mapping (physical):
    buffer y=16..31 → TOP    physical panels
    buffer y= 0..15 → BOTTOM physical panels

  Panel layout (32×16 each, value textSize=2 fills full panel height):
    TOP-LEFT   (x= 0..31, buf y=16..31) : Fouls A   — GREEN / RED at max
    TOP-RIGHT  (x=32..63, buf y=16..31) : Shot Clock — CYAN
    BOT-LEFT   (x= 0..31, buf y= 0..15) : Fouls B   — GREEN / RED at max
    BOT-RIGHT  (x=32..63, buf y= 0..15) : Quarter   — YELLOW

  WAIT: single "WAIT" textSize=2 centred across all 4 panels.
*/

#define PxMATRIX_SPI_FREQUENCY 2000000
#include <PxMatrix.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define P_LAT  5
#define P_A   19
#define P_B   23
#define P_C   18
#define P_OE   4

PxMATRIX display(64, 32, P_LAT, P_OE, P_A, P_B, P_C);
uint8_t display_draw_time = 30;

uint16_t COLOR_GREEN, COLOR_RED, COLOR_YELLOW, COLOR_CYAN, COLOR_BLACK;

void displayTask(void* pvParameters) {
    for (;;) { display.display(display_draw_time); vTaskDelay(1); }
}

// ── BoardData ─────────────────────────────────────────────────────────────────
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

// ── Constants ─────────────────────────────────────────────────────────────────
#define PNL_W     32
#define PNL_H     16
#define TOP_Y     16    // buffer y for top panels
#define BOT_Y      0    // buffer y for bottom panels
#define FOULS_MAX  5

// ── Draw one panel: value textSize=2 (12×16px), fills full panel height ───────
void drawPanel(int px, int py, const char* value, uint16_t valCol) {
    display.fillRect(px, py, PNL_W, PNL_H, COLOR_BLACK);
    int vw = strlen(value) * 12;   // textSize=2 → 12px per char
    display.setTextSize(2);
    display.setTextColor(valCol);
    display.setTextWrap(false);
    display.setCursor(px + max(0, (PNL_W - vw) / 2), py);
    display.print(value);
}

// ── Draw all 4 panels with live data ─────────────────────────────────────────
void drawAllData() {
    char buf[8];

    // TOP-LEFT: Fouls A (GREEN → RED at FOULS_MAX)
    snprintf(buf, sizeof(buf), "%d", rxBuf.foulsA);
    drawPanel(0, TOP_Y, buf, (rxBuf.foulsA >= FOULS_MAX) ? COLOR_RED : COLOR_GREEN);

    // TOP-RIGHT: Shot Clock (CYAN)
    if (rxBuf.shotSecs >= 10) snprintf(buf, sizeof(buf), "%d",    rxBuf.shotSecs);
    else                      snprintf(buf, sizeof(buf), "%d.%d", rxBuf.shotSecs, rxBuf.shotTenths);
    drawPanel(32, TOP_Y, buf, COLOR_CYAN);

    // BOT-LEFT: Fouls B (GREEN → RED at FOULS_MAX)
    snprintf(buf, sizeof(buf), "%d", rxBuf.foulsB);
    drawPanel(0, BOT_Y, buf, (rxBuf.foulsB >= FOULS_MAX) ? COLOR_RED : COLOR_GREEN);

    // BOT-RIGHT: Quarter (YELLOW)
    snprintf(buf, sizeof(buf), "%d", rxBuf.quarter);
    drawPanel(32, BOT_Y, buf, COLOR_YELLOW);
}

// ── WAIT: single "WAIT" spanning full 64×32 using FreeSansBold18pt7b ─────────
// Renders to GFXcanvas1 then copies with PxMatrix row-swap remap:
//   canvas y=0..15  → buffer y=16..31 (physical TOP panels)
//   canvas y=16..31 → buffer y= 0..15 (physical BOT panels)
void showWait() {
    GFXcanvas1 canvas(64, 32);
    canvas.fillScreen(0);
    canvas.setFont(&FreeSansBold18pt7b);
    canvas.setTextWrap(false);

    // Measure to auto-centre horizontally and vertically
    int16_t x1, y1;
    uint16_t tw, th;
    canvas.getTextBounds("WAIT", 0, 24, &x1, &y1, &tw, &th);
    int baseline = (32 - (int)th) / 2 - y1;
    int cx       = max(0, (int)(64 - (int)tw) / 2) - x1;
    canvas.setCursor(cx, baseline);
    canvas.print("WAIT");

    // Copy canvas → display with row-swap remap
    display.clearDisplay();
    for (int y = 0; y < 32; y++) {
        int dy = (y < 16) ? y + 16 : y - 16;
        for (int x = 0; x < 64; x++) {
            if (canvas.getPixel(x, y))
                display.drawPixel(x, dy, COLOR_CYAN);
        }
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    delay(1000);
    Serial.begin(115200);

    display.begin(8);
    delay(100);

    xTaskCreatePinnedToCore(displayTask, "disp", 2048, NULL, 2, NULL, 0);
    delay(100);

    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);

    COLOR_BLACK  = display.color565(  0,   0,   0);
    COLOR_GREEN  = display.color565(  0, 255,   0);
    COLOR_RED    = display.color565(  0,   0, 255);
    COLOR_YELLOW = display.color565(  0, 255, 255);
    COLOR_CYAN   = display.color565(255, 255,   0);

    // Splash: "S3" in top-left panel
    display.setTextSize(2);
    display.setTextColor(COLOR_GREEN);
    display.setCursor(4, TOP_Y);
    display.print("S3");
    delay(1200);

    showWait();

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
        display.clearDisplay();
        display.setTextSize(2);
        display.setTextColor(COLOR_RED);
        display.setCursor(16, TOP_Y);
        display.print("ERR");
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave3-test 4-panel ready");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    if (newData) {
        newData = false;
        if (!connected) {
            connected = true;
            display.clearDisplay();
            Serial.println("Master connected");
        }
        drawAllData();
    }
}
