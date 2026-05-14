#include <DMD32.h>
#include "fonts/SystemFont5x7.h"
#include "fonts/Arial_black_16.h"

#define DISPLAYS_ACROSS 1
#define DISPLAYS_DOWN 1
DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);

hw_timer_t* timer = NULL;

void IRAM_ATTR triggerScan() {
  dmd.scanDisplayBySPI();
}
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("return the clock speed of the CPU.");
  uint8_t cpuClock = ESP.getCpuFreqMHz();
  delay(500);
  Serial.println();
  Serial.println("Timer Begin");
  timer = timerBegin(0, cpuClock, true);
  delay(500);
  Serial.println();
  Serial.println("Attach triggerScan function to our timer.");
  timerAttachInterrupt(timer, &triggerScan, true);
  delay(500);
  Serial.println();
  Serial.println("Set alarm to call triggerScan function.");
  timerAlarmWrite(timer, 300, true);
  delay(500);
  Serial.println();
  Serial.println("Start an alarm.");
  timerAlarmEnable(timer);
  delay(500);
}

void loop() {
  dmd.selectFont(Arial_Black_16);
  String txt_1 = "ESP32 With P10 LED Display";
  char char_array_txt_1[txt_1.length() + 1];
  txt_1.toCharArray(char_array_txt_1, txt_1.length() + 1);
  dmd.clearScreen(true);
  delay(1000);
  dmd.drawMarquee(char_array_txt_1, txt_1.length(), (32 * DISPLAYS_ACROSS) - 1, 0);
  long start_1 = millis();
  long timer_1 = start_1;
  boolean ret = false;
  while (!ret) {
    if ((timer_1 + 30) < millis()) {
      ret = dmd.stepMarquee(-1, 0);
      delay(50);
      timer_1 = millis();
    }
  }
  
}