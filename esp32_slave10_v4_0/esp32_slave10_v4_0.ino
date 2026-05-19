/*
  Marketing Display  v4.0  —  Panel 10  (64 × 16 px)
  ─────────────────────────────────────────────────────────────────────────
  Changes vs v3.0:
    • eventName[32] → eventName[64] to match updated master BoardData struct.
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

PxMATRIX display(64, 16, P_LAT, P_OE, P_A, P_B, P_C);

uint8_t  display_draw_time = 30;
uint16_t C_BLACK, C_WHITE;

static unsigned long lastScan = 0;
void scanIfNeeded() {
    unsigned long now = micros();
    if ((long)(now - lastScan) >= 2000) { display.display(display_draw_time); lastScan = now; }
}

typedef struct __attribute__((packed)) {
    char eventName[64]; char teamA[16]; char teamB[16];
    int  scoreA, scoreB, clockSecs, clockTenths, quarter;
    char possession;
    int  foulsA, foulsB, timeoutsA, timeoutsB, screenMask;
    int  clockRunning, shotSecs, shotTenths, shotRunning, eventScroll;
    char marketingText[32];
} BoardData;

BoardData     rxBuf;
volatile bool newData = false;

char  currentText[33] = "";
bool  scrollMode      = false;
int   scrollX         = 64;
int   textPixelW      = 0;
unsigned long lastScrollMs = 0;
#define SCROLL_SPEED_MS 40

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == 1 && data[0] == 0xAA) { ESP.restart(); return; }
    if (len == sizeof(BoardData)) { memcpy(&rxBuf, data, sizeof(rxBuf)); newData = true; }
}

void applyText(const char* txt) {
    strncpy(currentText, txt, 32); currentText[32] = '\0';
    int len = strlen(currentText);
    textPixelW = len * 6;
    scrollMode = (textPixelW > 64);
    scrollX    = 64;
}

void drawStatic() {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    if (strlen(currentText) == 0) return;
    display.setTextWrap(false);
    display.setTextSize(1);
    display.setTextColor(C_WHITE);
    int x = max(0, (64 - textPixelW) / 2);
    display.setCursor(x, 4);
    display.print(currentText);
}

void stepScroll() {
    unsigned long now = millis();
    if ((long)(now - lastScrollMs) < SCROLL_SPEED_MS) return;
    lastScrollMs = now;
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);
    display.setTextSize(1);
    display.setTextColor(C_WHITE);
    display.setCursor(scrollX, 4);
    display.print(currentText);
    scrollX--;
    if (scrollX < -textPixelW) scrollX = 64;
}

void showWait() {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);
    display.setTextSize(1);
    display.setTextColor(C_WHITE);
    display.setCursor(20, 4);
    display.print("WAIT");
}

void setup() {
    delay(1000);
    Serial.begin(115200);
    display.begin(8);
    delay(100);
    C_BLACK = display.color565(  0,   0,   0);
    C_WHITE = display.color565(255, 255, 255);
    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);
    showWait();
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("Marketing panel MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Marketing panel v4.0 ready");
    }
}

void loop() {
    scanIfNeeded();
    if (newData) {
        newData = false;
        if (strncmp(currentText, rxBuf.marketingText, 32) != 0) {
            applyText(rxBuf.marketingText);
            if (!scrollMode) drawStatic();
        }
    }
    if (scrollMode) stepScroll();
}
