#include <DMD32.h>
#include "fonts/Arial_black_16.h"

#define DISPLAYS_ACROSS 6
#define DISPLAYS_DOWN   1
DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);

hw_timer_t* timer = NULL;

void IRAM_ATTR triggerScan() {
  dmd.scanDisplayBySPI();
}

void setup() {
  Serial.begin(115200);
  delay(500);

  uint8_t cpuClock = ESP.getCpuFreqMHz();
  timer = timerBegin(0, cpuClock, true);
  timerAttachInterrupt(timer, &triggerScan, true);
  timerAlarmWrite(timer, 2000, true);
  timerAlarmEnable(timer);

  delay(500);

  dmd.clearScreen(true);
  dmd.selectFont(Arial_Black_16);
  dmd.drawString(0, 0, "HI all", 6, GRAPHICS_NORMAL);

  Serial.println("Static display ready.");
}

void loop() {
  // Nothing — timer ISR keeps the display refreshed continuously
}
