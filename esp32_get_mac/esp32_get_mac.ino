/*
  esp32_get_mac
  Upload this to any ESP32 to read its MAC address.
  Open Serial Monitor at 115200 baud and copy the address
  shown — that is the slave's MAC you need for the master.
*/
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  delay(1000);
  WiFi.mode(WIFI_STA);
  Serial.println();
  Serial.println("=== ESP32 MAC ADDRESS ===");
  Serial.print("MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.println("=========================");
  Serial.println("Copy the MAC above and add it to esp32_master.ino");
}

void loop() {}
