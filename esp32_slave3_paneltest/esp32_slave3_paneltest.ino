/*
  Slave 3 — Panel Test (Solid Color)
  ─────────────────────────────────────────────────────
  Step 1: Fill entire 192×32 screen GREEN to confirm display init works.
  If you see solid green on all panels → init is OK.
  Then we can add numbered panels.

  If nothing shows:
    - Try changing display.begin(8) to display.begin(4)
    - Try changing timerAlarmWrite(timer, 2000, true) to 4000
*/

#define PxMATRIX_SPI_FREQUENCY 10000000
#include <PxMatrix.h>

#define P_LAT 5
#define P_A   19
#define P_B   23
#define P_C   18
#define P_OE  4

hw_timer_t * timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t display_draw_time = 50;   // increased from 30 for wider display

// 6 panels wide x 2 panels tall = 192 x 32
PxMATRIX display(192, 32, P_LAT, P_OE, P_A, P_B, P_C);

uint16_t C_BLACK = display.color565(  0,   0,   0);
uint16_t C_GREEN = display.color565(  0, 255,   0);
uint16_t C_RED   = display.color565(  0,   0, 255);  // R<->B swap

void IRAM_ATTR display_updater() {
  portENTER_CRITICAL_ISR(&timerMux);
  display.display(display_draw_time);
  portEXIT_CRITICAL_ISR(&timerMux);
}

void setup() {
  delay(1000);
  Serial.begin(115200);
  Serial.println("Slave3 paneltest starting...");

  display.begin(4);
  delay(100);

  timer = timerBegin(0, 80, true);
  timerAttachInterrupt(timer, &display_updater, true);
  timerAlarmWrite(timer, 4000, true);   // slightly longer than single-panel 1500
  timerAlarmEnable(timer);
  delay(100);

  display.clearDisplay();
  display.setBrightness(200);
  display.setTextWrap(false);
  display.setRotation(0);
  delay(100);

  // Simplest possible test: fill entire screen GREEN
  display.fillRect(0, 0, 192, 32, C_GREEN);

  Serial.println("Done — should see solid green on all 12 panels");
}

void loop() {}
