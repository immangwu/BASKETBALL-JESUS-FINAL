#include <RGBmatrixPanel.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

// ── Pin Configuration ────────────────────────────────────────────────────────
// NOTE: RGBmatrixPanel requires you to define the CLK pin explicitly, 
// along with your RGB data pins (R1, G1, B1, R2, G2, B2) depending on the specific ESP32 port you are using.
#define CLK  15 // <-- VERIFY YOUR CLOCK PIN
#define P_LAT 5
#define P_A   19
#define P_B   23
#define P_C   18
#define P_OE  4
#define P_D   14 // 32px tall panels need Pin D

// Constructor for 32-row panels: A, B, C, D, CLK, LAT, OE, double_buffer, width
RGBmatrixPanel display(P_A, P_B, P_C, P_D, CLK, P_LAT, P_OE, false, 64);

uint16_t C_BLACK, C_GREEN, C_RED, C_YELLOW, C_CYAN, C_WHITE;

// ── Struct ───────────────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    char eventName[32];
    char teamA[16];
    char teamB[16];
    int  scoreA, scoreB, clockSecs, clockTenths, quarter;
    char possession;
    int  foulsA, foulsB, timeoutsA, timeoutsB, screenMask;
    int  clockRunning, shotSecs, shotTenths, shotRunning, eventScroll;
} BoardData;

BoardData rxBuf;
volatile bool newData = false;
bool connected = false;

// ── Helpers ──────────────────────────────────────────────────────────────────
void waitMs(long ms) {
    // No manual scanning needed with RGBmatrixPanel
    delay(ms); 
}

// Callback for ESP32 Core 1.0.6 compatibility
void onReceive(const uint8_t *mac_addr, const uint8_t *data, int len) {
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ── Drawing Logic ────────────────────────────────────────────────────────────
#define ZONE_A_X 0
#define ZONE_A_W 20
#define ZONE_SC_X 20
#define ZONE_SC_W 24
#define ZONE_B_X 44
#define ZONE_B_W 20

#define LABEL_Y 16
#define VALUE_Y 0

void drawText(const char* s, int zX, int zW, int bY, uint16_t col, uint8_t sz) {
    int tw = strlen(s) * 6 * sz;
    int tx = zX + max(0, (zW - tw) / 2);
    display.setTextSize(sz);
    display.setTextColor(col);
    display.setCursor(tx, bY + (sz == 1 ? 4 : 0)); 
    display.print(s);
}

void updateDisplay() {
    display.fillScreen(C_BLACK); // Replaces clearDisplay()
    
    // FA Zone
    drawText("FA", ZONE_A_X, ZONE_A_W, LABEL_Y, C_YELLOW, 1);
    uint16_t colA = (rxBuf.foulsA >= 5) ? C_RED : C_GREEN;
    char fA[4]; sprintf(fA, "%d", rxBuf.foulsA);
    drawText(fA, ZONE_A_X, ZONE_A_W, VALUE_Y, colA, 2);

    // Shot Clock
    drawText("SC", ZONE_SC_X, ZONE_SC_W, LABEL_Y, C_CYAN, 1);
    char sc[4]; 
    if (rxBuf.shotSecs >= 10) sprintf(sc, "%d", rxBuf.shotSecs);
    else sprintf(sc, "%d.%d", rxBuf.shotSecs, rxBuf.shotTenths);
    drawText(sc, ZONE_SC_X, ZONE_SC_W, VALUE_Y, C_CYAN, rxBuf.shotSecs >= 10 ? 2 : 1);

    // FB Zone
    drawText("FB", ZONE_B_X, ZONE_B_W, LABEL_Y, C_YELLOW, 1);
    uint16_t colB = (rxBuf.foulsB >= 5) ? C_RED : C_GREEN;
    char fB[4]; sprintf(fB, "%d", rxBuf.foulsB);
    drawText(fB, ZONE_B_X, ZONE_B_W, VALUE_Y, colB, 2);
}

void setup() {
    Serial.begin(115200);
    
    display.begin(); 
    
    // Changed color565 to Color888 for RGBmatrixPanel compatibility
    C_BLACK  = display.Color888(0, 0, 0);
    C_WHITE  = display.Color888(255, 255, 255);
    C_GREEN  = display.Color888(0, 255, 0);
    C_RED    = display.Color888(255, 0, 0); 
    C_YELLOW = display.Color888(255, 255, 0);
    C_CYAN   = display.Color888(0, 255, 255);

    WiFi.mode(WIFI_STA);
    if (esp_now_init() != ESP_OK) {
        Serial.println("Error initializing ESP-NOW");
        return;
    }
    esp_now_register_recv_cb(onReceive);
}

void loop() {
    if (newData) {
        newData = false;
        connected = true;
        updateDisplay();
    }

    if (!connected) {
        display.fillScreen(C_BLACK);
        drawText("WAIT", 0, 64, 8, C_CYAN, 2);
        waitMs(100);
    }
}