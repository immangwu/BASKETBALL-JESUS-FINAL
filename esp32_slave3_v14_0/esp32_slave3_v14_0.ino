/*
  Shot Clock TOP  v14.0  —  Panel 3  (64 × 16 px)
  ─────────────────────────────────────────────────────────────────────────
  Combined 32px surface with Slave 4 (bottom panel):
    y=0..7  : "SHOT CLOCK" label — size 1, centred
    y=8..15 : top 8px of size-3 (24px) digit  (cursor y=8)

  When secs > 4 : size-3 digit split across both panels (24px total)
  When secs ≤ 4 : label only — digit + tenths shown entirely on Slave 4

  Color: BLUE normal → RED when secs ≤ 5
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
uint16_t C_BLACK, C_BLUE, C_RED;

static unsigned long lastScan = 0;
void scanIfNeeded() {
    unsigned long now = micros();
    if ((long)(now - lastScan) >= 2000) { display.display(display_draw_time); lastScan = now; }
}
void waitMs(long ms) {
    long end = (long)millis() + ms;
    while ((long)(millis() - end) < 0) scanIfNeeded();
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
volatile bool newData   = false;
bool          connected = false;

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == 1 && data[0] == 0xAA) { ESP.restart(); return; }
    if (len == sizeof(BoardData)) { memcpy(&rxBuf, data, sizeof(rxBuf)); newData = true; }
}

// ── Slave-side shot clock ─────────────────────────────────────────────────────
int  localShotSecs    = 24;
int  localShotTenths  = 0;
bool localShotRunning = false;
unsigned long lastTickMs = 0;

void syncShot(int ms, int mt, int mr) {
    if (mr && !localShotRunning) {
        localShotSecs = ms; localShotTenths = mt;
        localShotRunning = true; lastTickMs = millis();
    } else if (!mr && localShotRunning) {
        localShotRunning = false;
        localShotSecs = ms; localShotTenths = mt;
    } else if (localShotRunning) {
        int masterTotal = ms * 10 + mt;
        int localTotal  = localShotSecs * 10 + localShotTenths;
        if (abs(masterTotal - localTotal) > 1) {
            localShotSecs = ms; localShotTenths = mt;
        }
    } else {
        localShotSecs = ms; localShotTenths = mt;
    }
}

void tickShot() {
    if (!localShotRunning) return;
    unsigned long now = millis();
    if ((long)(now - lastTickMs) < 100) return;
    lastTickMs = now;
    localShotTenths--;
    if (localShotTenths < 0) {
        localShotTenths = 9;
        localShotSecs--;
        if (localShotSecs < 0) {
            localShotSecs = 0; localShotTenths = 0;
            localShotRunning = false;
        }
    }
}

// ── Display ───────────────────────────────────────────────────────────────────
int lastDrawSecs = -1;

void drawTop(int secs) {
    if (secs == lastDrawSecs) return;
    lastDrawSecs = secs;

    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);

    uint16_t col = (secs <= 5) ? C_RED : C_BLUE;

    // y=0..7 : "SHOT CLOCK" label, size 1
    display.setTextSize(1);
    display.setTextColor(col);
    const char* lbl = "SHOT CLOCK";
    int lw = (int)strlen(lbl) * 6;
    display.setCursor((64 - lw) / 2, 0);
    display.print(lbl);

    if (secs > 4) {
        // y=8..15 : top 8px of size-3 (24px) digit — cursor y=8
        display.setTextSize(3);
        display.setTextColor(col);
        char buf[3];
        snprintf(buf, sizeof(buf), "%d", secs);
        int tw = (int)strlen(buf) * 18;
        display.setCursor((64 - tw) / 2, 8);
        display.print(buf);
    }
    // secs ≤ 4: label only — digit shown entirely on Slave 4
}

void showWait() {
    display.fillRect(0, 0, 64, 16, C_BLACK);
    display.setTextWrap(false);
    display.setTextSize(1);
    display.setTextColor(C_BLUE);
    display.setCursor(20, 4);
    display.print("WAIT");
}

void setup() {
    delay(1000);
    Serial.begin(115200);
    display.begin(8);
    delay(100);
    C_BLACK = display.color565(  0,   0,   0);
    C_BLUE  = display.color565(  0,   0, 255);
    C_RED   = display.color565(255,   0,   0);
    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);
    showWait();
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("Shot clock top MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Shot clock top v14.0 ready");
    }
}

void loop() {
    scanIfNeeded();
    tickShot();
    drawTop(localShotSecs);

    if (newData) {
        newData = false;
        if (!connected) { connected = true; Serial.println("Master connected"); }
        syncShot(rxBuf.shotSecs, rxBuf.shotTenths, rxBuf.shotRunning);
    }
}
