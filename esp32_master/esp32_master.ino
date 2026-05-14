/*
  Master ESP32 — Scoreboard Serial Bridge
  ────────────────────────────────────────
  Reads N / S packets from scoreboard_final.py via USB serial (115200 baud)
  and broadcasts them via ESP-NOW to all slave ESP32s.

  Packet formats (from scoreboard_final.py):
    N,<eventName>,<teamA>,<teamB>\n
    S,<scoreA>,<scoreB>,<clockSecs>,<clockTenths>,<quarter>,<possession>,
      <foulsA>,<foulsB>,<timeoutsA>,<timeoutsB>,<clockRunning>,
      <shotSecs>,<shotTenths>,<shotRunning>,<fontScore>,<fontClock>,
      <fontFoul>,<fontShot>\n

  No display on master — just a serial → ESP-NOW relay.
*/

#include <esp_now.h>
#include <WiFi.h>

#define SERIAL_BAUD 115200

//uint8_t broadcastAddr[] = {0x28, 0x05, 0xA5, 0x34, 0x62, 0x78};
uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

typedef struct __attribute__((packed)) {
  char eventName[32];
  char teamA[16];
  char teamB[16];
  int  scoreA,     scoreB;
  int  clockSecs,  clockTenths;
  int  quarter;
  char possession;
  int  foulsA,     foulsB;
  int  timeoutsA,  timeoutsB;
  int  screenMask;
  int  clockRunning;
  int  shotSecs,   shotTenths;
  int  shotRunning;
  int  eventScroll;   // 0 = static display, 1 = scrolling display
} BoardData;

BoardData txData;

// ── Helpers ───────────────────────────────────────────────────────────────────
// Split a comma-delimited String into tokens[], return count
int splitCSV(const String& s, String tokens[], int maxTokens) {
  int count = 0;
  int start = 0;
  for (int i = 0; i <= (int)s.length() && count < maxTokens; i++) {
    if (i == (int)s.length() || s[i] == ',') {
      tokens[count++] = s.substring(start, i);
      start = i + 1;
    }
  }
  return count;
}

// ── Packet parsers ────────────────────────────────────────────────────────────
void parseN(const String& line) {
  // Tokens: N | eventName | teamA | teamB | eventScroll
  String t[5];
  int n = splitCSV(line, t, 5);
  if (n < 2) return;
  t[1].toCharArray(txData.eventName, sizeof(txData.eventName));
  if (n > 2) t[2].toCharArray(txData.teamA, sizeof(txData.teamA));
  if (n > 3) t[3].toCharArray(txData.teamB, sizeof(txData.teamB));
  txData.eventScroll = (n > 4) ? t[4].toInt() : 0;
}

void parseS(const String& line) {
  // Tokens: S | scoreA | scoreB | clockSecs | clockTenths | quarter |
  //         possession | foulsA | foulsB | timeoutsA | timeoutsB |
  //         clockRunning | shotSecs | shotTenths | shotRunning |
  //         fontScore | fontClock | fontFoul | fontShot
  String t[20];
  int n = splitCSV(line, t, 20);
  if (n < 15) return;

  txData.scoreA       = t[1].toInt();
  txData.scoreB       = t[2].toInt();
  txData.clockSecs    = t[3].toInt();
  txData.clockTenths  = t[4].toInt();
  txData.quarter      = t[5].toInt();
  txData.possession   = t[6].length() > 0 ? t[6][0] : 'N';
  txData.foulsA       = t[7].toInt();
  txData.foulsB       = t[8].toInt();
  txData.timeoutsA    = t[9].toInt();
  txData.timeoutsB    = t[10].toInt();
  txData.clockRunning = t[11].toInt();
  txData.shotSecs     = t[12].toInt();
  txData.shotTenths   = t[13].toInt();
  txData.shotRunning  = t[14].toInt();
  // t[15..18] = font sizes — stored in screenMask for future slave use
  txData.screenMask   = t[15].toInt();
}

// ── ESP-NOW send callback ──────────────────────────────────────────────────────
void onSent(const uint8_t* mac, esp_now_send_status_t status) {
  // Uncomment for debugging:
  // Serial.println(status == ESP_NOW_SEND_SUCCESS ? "ESP-NOW OK" : "ESP-NOW FAIL");
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(SERIAL_BAUD);

  WiFi.mode(WIFI_STA);
  Serial.print("Master MAC: ");
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed — restarting");
    ESP.restart();
  }
  esp_now_register_send_cb(onSent);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddr, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  // Send initial state so slaves show something on power-up
  memset(&txData, 0, sizeof(txData));
  strncpy(txData.eventName, "READY",  31);
  strncpy(txData.teamA,     "TEAM A", 15);
  strncpy(txData.teamB,     "TEAM B", 15);
  esp_now_send(broadcastAddr, (uint8_t*)&txData, sizeof(txData));

  Serial.println("Master ready — listening for N/S packets");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line.startsWith("N,")) {
      parseN(line);
      esp_now_send(broadcastAddr, (uint8_t*)&txData, sizeof(txData));
      Serial.print("TX N → "); Serial.println(txData.eventName);
    } else if (line.startsWith("S,")) {
      parseS(line);
      esp_now_send(broadcastAddr, (uint8_t*)&txData, sizeof(txData));
    }
  }
}
