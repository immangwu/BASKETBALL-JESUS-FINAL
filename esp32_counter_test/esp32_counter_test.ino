/*
  Single row × 2 columns P10 RGB = 64 × 16 px
  Just prints JESUS centred.
*/

#define PxMATRIX_SPI_FREQUENCY 20000000
#include <PxMatrix.h>

#define P_LAT  5
#define P_A   19
#define P_B   23
#define P_C   18
#define P_OE   4

// 1 row × 3 cols = 96 wide × 16 tall
PxMATRIX display(96, 16, P_LAT, P_OE, P_A, P_B, P_C);

static unsigned long lastScan = 0;
void scanIfNeeded() {
    unsigned long now = micros();
    if ((long)(now - lastScan) >= 2000) {
        display.display(40);
        lastScan = now;
    }
}

void setup() {
    Serial.begin(115200);
    display.begin(8);
    delay(100);
    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);

    // "JESUS" size 2 = 60px wide × 16px tall — centred in 96px
    display.setTextSize(2);
    display.setTextColor(display.color565(255, 255, 255));  // WHITE
    display.setCursor(18, 0);
    display.print("JESUS");

    Serial.println("ready");
}

void loop() {
    scanIfNeeded();
}
