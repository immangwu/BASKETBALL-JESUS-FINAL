#include <PxMatrix.h>
#include <Ticker.h>

// --- PIN DEFINITIONS ---
// Adjust these based on your ESP8266/ESP32 wiring
#define P_LAT 16 // D0
#define P_A 5    // D1
#define P_B 4    // D2
#define P_C 15   // D8
#define P_OE 2   // D4

#define DISPLAY_WIDTH 32
#define DISPLAY_HEIGHT 16

Ticker display_ticker;
// Pin config: (Width, Height, LAT, OE, A, B, C)
PxMATRIX display(DISPLAY_WIDTH, DISPLAY_HEIGHT, P_LAT, P_OE, P_A, P_B, P_C);

// Refresh mechanism
void display_updater() {
  display.display(70); // 70-100 recommended
}

void setup() {
  // --- Initialize Matrix ---
  display.begin(8); // P10 32x16 often uses 1/8 scan
  display.setBrightness(128); // 0-255
  display.flushDisplay();
  
  // Start the refresh ticker
  display_ticker.attach(0.002, display_updater); 

  // --- Draw Content ---
  display.setTextColor(display.color565(255, 0, 0)); // Red
  display.setCursor(2, 0);
  display.print("P10");
}

void loop() {
  // Library handles drawing in background via ticker
}
