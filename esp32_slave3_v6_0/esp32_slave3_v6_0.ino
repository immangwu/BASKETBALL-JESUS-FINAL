/*
  Slave 3  v6.0  —  Shot Clock TOP panel  (32 × 32 px)
  ─────────────────────────────────────────────────────
  Each panel : 32×16 px  →  2 rows × 1 col = 32×32 px per slave
  Combined   : Slave 3 (top) + Slave 4 (bottom) = 32×64 px

  Combined display layout:
    ≥ 10 s  →  Slave 3 = TENS digit    Slave 4 = UNITS digit
    <  10 s  →  Slave 3 = main digit    Slave 4 = ".tenths"
*/

#define PxMATRIX_SPI_FREQUENCY 20000000
#include <PxMatrix.h>
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define P_LAT  5
#define P_A   19
#define P_B   23
#define P_C   18
#define P_OE   4

// 2 rows × 1 col of 32×16 panels = 32×32 px
PxMATRIX display(32, 32, P_LAT, P_OE, P_A, P_B, P_C);

uint16_t C_BLACK, C_CYAN;

// ── Polled scan ───────────────────────────────────────────────────────────────
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

// ── BoardData (must match master exactly) ────────────────────────────────────
typedef struct __attribute__((packed)) {
    char eventName[32]; char teamA[16]; char teamB[16];
    int  scoreA, scoreB, clockSecs, clockTenths, quarter;
    char possession;
    int  foulsA, foulsB, timeoutsA, timeoutsB, screenMask;
    int  clockRunning, shotSecs, shotTenths, shotRunning, eventScroll;
} BoardData;

BoardData rxBuf; volatile bool newData = false; bool connected = false;

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) { memcpy(&rxBuf, data, sizeof(rxBuf)); newData = true; }
}

// ── Draw (Slave 3 = TOP half of combined display) ────────────────────────────
//   ≥10 s : show TENS digit  (e.g. "2" from "24")
//   < 10 s : show main DIGIT (e.g. "9" from "9.5")
//
//   size 4 = 24×32 px → fills the full 32×32 panel height, centred horizontally
void drawTop(int secs, int tenths) {
    display.fillRect(0, 0, 32, 32, C_BLACK);
    display.setTextWrap(false);
    display.setTextColor(C_CYAN);
    display.setTextSize(4);   // 24×32 px per char

    char buf[3];
    if (secs >= 10) {
        snprintf(buf, sizeof(buf), "%d", secs / 10);   // tens digit only
    } else {
        snprintf(buf, sizeof(buf), "%d", secs);         // single digit
    }

    // centre 24 px wide char in 32 px → x = (32-24)/2 = 4
    display.setCursor(4, 0);
    display.print(buf);
}

// ── WAIT screen ───────────────────────────────────────────────────────────────
void showWait() {
    display.fillRect(0, 0, 32, 32, C_BLACK);
    display.setTextSize(1);
    display.setTextWrap(false);
    display.setTextColor(C_CYAN);
    display.setCursor(2, 0);   display.print("WAIT");
    display.setCursor(2, 16);  display.print("WAIT");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    delay(500);
    Serial.begin(115200);
    display.begin(8);
    delay(100);

    C_BLACK = display.color565(  0,   0,   0);
    C_CYAN  = display.color565(255, 255,   0);  // R↔B swap

    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);

    showWait();

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("Slave3 MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 3 v6.0 ready — 32x32 top panel");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();
    if (!newData) return;
    newData = false;
    if (!connected) { connected = true; Serial.println("Master connected"); }
    drawTop(rxBuf.shotSecs, rxBuf.shotTenths);
}
