#define PxMATRIX_SPI_FREQUENCY 2000000
#include <PxMatrix.h>

#define P_LAT  5
#define P_A   19
#define P_B   23
#define P_C   18
#define P_OE   4

PxMATRIX display(64, 32, P_LAT, P_OE, P_A, P_B, P_C);
uint8_t display_draw_time = 30;

void displayTask(void* pvParameters) {
    for (;;) { display.display(display_draw_time); vTaskDelay(1); }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    display.begin(4);
    delay(100);

    xTaskCreatePinnedToCore(displayTask, "disp", 2048, NULL, 2, NULL, 0);
    delay(100);

    display.clearDisplay();
    display.setBrightness(150);
    display.setTextWrap(false);
    display.setRotation(0);

    uint16_t C_YELLOW = display.color565(0, 255, 255);  // R<->B swap

    // "JESUS" textSize=2 → 60px wide, 16px tall
    // centred in 64px: x = (64-60)/2 = 2
    // drawn in both rows so all 4 panels show it
    display.setTextSize(2);
    display.setTextColor(C_YELLOW);
    display.setCursor(2, 0);   // buffer y=0..15
    display.print("JESUS");
    display.setCursor(2, 16);  // buffer y=16..31
    display.print("JESUS");
}

void loop() {}
