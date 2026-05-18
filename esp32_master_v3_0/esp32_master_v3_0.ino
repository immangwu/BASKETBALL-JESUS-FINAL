/*
  Master ESP32  v3.0  —  Serial → ESP-NOW bridge
  ─────────────────────────────────────────────────────────────────────────
  Packet formats received from scoreboard_v6.py:
    N,<eventName>,<teamA>,<teamB>,<eventScroll>,<marketingText>\n
    S,<scoreA>,<scoreB>,<clockSecs>,<clockTenths>,<quarter>,<possession>,
      <foulsA>,<foulsB>,<timeoutsA>,<timeoutsB>,<clockRunning>,
      <shotSecs>,<shotTenths>,<shotRunning>,<fontScore>,<fontClock>,
      <fontFoul>,<fontShot>\n
    M,<marketingText>\n   (panel 10 marketing display)
    R                     (broadcast reset to all slaves)
*/

#include <esp_now.h>
#include <WiFi.h>

#define SERIAL_BAUD 115200

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
  int  eventScroll;
  char marketingText[32];   // v3.0 — Panel 10 marketing display
} BoardData;

BoardData txData;

int splitCSV(const String& s, String tokens[], int maxTokens) {
  int count = 0, start = 0;
  for (int i = 0; i <= (int)s.length() && count < maxTokens; i++) {
    if (i == (int)s.length() || s[i] == ',') {
      tokens[count++] = s.substring(start, i);
      start = i + 1;
    }
  }
  return count;
}

void parseN(const String& line) {
  String t[6];
  int n = splitCSV(line, t, 6);
  if (n < 2) return;
  t[1].toCharArray(txData.eventName, sizeof(txData.eventName));
  if (n > 2) t[2].toCharArray(txData.teamA, sizeof(txData.teamA));
  if (n > 3) t[3].toCharArray(txData.teamB, sizeof(txData.teamB));
  txData.eventScroll = (n > 4) ? t[4].toInt() : 0;
  if (n > 5) t[5].toCharArray(txData.marketingText, sizeof(txData.marketingText));
}

void parseS(const String& line) {
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
  txData.screenMask   = (n > 15) ? t[15].toInt() : 0;
}

void parseM(const String& line) {
  // M,<marketingText>
  int comma = line.indexOf(',');
  if (comma < 0) return;
  String txt = line.substring(comma + 1);
  txt.toCharArray(txData.marketingText, sizeof(txData.marketingText));
}

void onSent(const uint8_t* mac, esp_now_send_status_t status) {}

void setup() {
  Serial.begin(SERIAL_BAUD);
  WiFi.mode(WIFI_STA);
  Serial.print("Master v3.0 MAC: ");
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

  memset(&txData, 0, sizeof(txData));
  strncpy(txData.eventName,     "READY",  31);
  strncpy(txData.teamA,         "TEAM A", 15);
  strncpy(txData.teamB,         "TEAM B", 15);
  strncpy(txData.marketingText, "",       31);
  esp_now_send(broadcastAddr, (uint8_t*)&txData, sizeof(txData));

  Serial.println("Master v3.0 ready");
}

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
    } else if (line.startsWith("M,")) {
      parseM(line);
      esp_now_send(broadcastAddr, (uint8_t*)&txData, sizeof(txData));
      Serial.print("TX M → "); Serial.println(txData.marketingText);
    } else if (line.startsWith("R")) {
      uint8_t magic = 0xAA;
      esp_now_send(broadcastAddr, &magic, 1);
      Serial.println("RESET broadcast sent");
    }
  }
}
