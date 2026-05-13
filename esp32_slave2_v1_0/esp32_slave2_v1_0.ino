/*
  Slave 2 v1.0 — TOL + Possession Display
  ─────────────────────────────────────────────────────────────────────────
  Hardware : 4 panels wide × 2 tall  (128 × 32 px)

  Column layout (32 px each):
    x=  0.. 31 : "TOL" label (top) + 5 timeout dots (bottom) — Team A
    x= 32.. 63 : ◄ left-pointing arrow if possession == 'A'
    x= 64.. 95 : ► right-pointing arrow if possession == 'B'
    x= 96..127 : "TOL" label (top) + 5 timeout dots (bottom) — Team B

  Cross-row remap (32 px visual zone across 2 physical rows):
    VY  0..15 → dmd_y = VY + 16   (physical row 1 — top panel,   y=16..31)
    VY 16..31 → dmd_y = VY - 16   (physical row 2 — bottom panel, y= 0..15)

  TOL label : VY = 0..6   (top panel)
  5 dots    : VY = 18..29 (bottom panel, centred vertically: (16-12)/2=2 → 16+2=18)

  No timer ISR — scanDisplayBySPI() polled via micros() from loop().
*/

#include <DMD32.h>
#include "fonts/SystemFont5x7.h"
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define DISPLAYS_ACROSS  4
#define DISPLAYS_DOWN    2
#define DISPLAY_W        (32 * DISPLAYS_ACROSS)   // 128 px

// Column x-starts
#define TOL_A_X    0
#define TOL_A_W   32
#define ARR_A_X   32   // ◄ possession A
#define ARR_B_X   64   // ► possession B
#define ARR_W     32
#define TOL_B_X   96
#define TOL_B_W   32

// Dot parameters — 5 × 12 px, 1 px gap, max 5 (total 29 px wide fits 32-px col)
#define DOT_W      5
#define DOT_H     12
#define DOT_GAP    1
#define DOT_MAX    5
#define DOT_ROW_W  (DOT_MAX * DOT_W + (DOT_MAX - 1) * DOT_GAP)   // 29 px
#define DOT_VY    18   // bottom panel, vertically centred

DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);

// ── Scan polling (no timer ISR) ───────────────────────────────────────────────
unsigned long lastScan = 0;
void scanIfNeeded() {
    if ((long)(micros() - lastScan) >= 300) {
        dmd.scanDisplayBySPI();
        lastScan = micros();
    }
}
void waitMs(long ms) {
    long end = millis() + ms;
    while ((long)(millis() - end) < 0) scanIfNeeded();
}

// ── Data struct (must match master exactly) ───────────────────────────────────
typedef struct __attribute__((packed)) {
    char eventName[32];
    char teamA[16];
    char teamB[16];
    int  scoreA,    scoreB;
    int  clockSecs, clockTenths;
    int  quarter;
    char possession;
    int  foulsA,    foulsB;
    int  timeoutsA, timeoutsB;
    int  screenMask;
    int  clockRunning;
    int  shotSecs,  shotTenths;
    int  shotRunning;
    int  eventScroll;
} BoardData;

BoardData     rxBuf;
volatile bool newData      = false;
int           lastTimeoutsA = -1;
int           lastTimeoutsB = -1;
char          lastPossession = ' ';

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == sizeof(BoardData)) {
        memcpy(&rxBuf, data, sizeof(rxBuf));
        newData = true;
    }
}

// ── String pixel-width helper ─────────────────────────────────────────────────
int strPixelWidth(const char* s, int len) {
    int w = 0;
    for (int i = 0; i < len; i++) {
        int cw = dmd.charWidth((unsigned char)s[i]);
        if (cw > 0) w += cw + 1;
    }
    return w;
}

// ── Cross-row pixel write ─────────────────────────────────────────────────────
static void writePixelRemapped(int x, int VY, uint8_t val) {
    int dmd_y = (VY < 16) ? (VY + 16) : (VY - 16);
    if (dmd_y < 0 || dmd_y >= DISPLAYS_DOWN * 16) return;
    dmd.writePixel(x, dmd_y, GRAPHICS_NORMAL, val);
}

// ── Cross-row character draw ──────────────────────────────────────────────────
int drawCharRemapped(int bX, int VY_start, unsigned char letter, const uint8_t* fnt) {
    uint8_t height    = pgm_read_byte(fnt + FONT_HEIGHT);
    uint8_t firstChar = pgm_read_byte(fnt + FONT_FIRST_CHAR);
    uint8_t charCount = pgm_read_byte(fnt + FONT_CHAR_COUNT);

    if (letter == ' ') {
        unsigned char n = 'n';
        if (n >= firstChar && n < (firstChar + charCount))
            return pgm_read_byte(fnt + FONT_WIDTH_TABLE + (n - firstChar)) + 1;
        return 5;
    }
    if (letter < firstChar || letter >= (firstChar + charCount)) return 0;

    unsigned char c     = letter - firstChar;
    uint8_t       bytes = (height + 7) / 8;
    uint16_t      index = 0;
    uint8_t       width;

    bool fixedW = (pgm_read_byte(fnt + FONT_LENGTH)     == 0 &&
                   pgm_read_byte(fnt + FONT_LENGTH + 1) == 0);
    if (fixedW) {
        width = pgm_read_byte(fnt + FONT_FIXED_WIDTH);
        index = (uint16_t)c * bytes * width + FONT_WIDTH_TABLE;
    } else {
        for (uint8_t i = 0; i < c; i++)
            index += pgm_read_byte(fnt + FONT_WIDTH_TABLE + i);
        index = index * bytes + charCount + FONT_WIDTH_TABLE;
        width = pgm_read_byte(fnt + FONT_WIDTH_TABLE + c);
    }

    for (uint8_t j = 0; j < width; j++) {
        for (uint8_t bi = bytes - 1; bi < 254; bi--) {
            uint8_t data   = pgm_read_byte(fnt + index + j + (bi * width));
            int     offset = (int)bi * 8;
            if (bi == bytes - 1 && bytes > 1) offset = height - 8;
            for (uint8_t k = 0; k < 8; k++) {
                int row = offset + k;
                if (row < (int)bi * 8 || row > (int)height) continue;
                int VY = VY_start + row;
                writePixelRemapped(bX + j, VY, (data & (1 << k)) ? 1 : 0);
            }
        }
    }
    return width;
}

void drawStringRemapped(int x, int VY_start, const char* s, int len, const uint8_t* fnt) {
    int cx = x;
    for (int i = 0; i < len; i++) {
        int w = drawCharRemapped(cx, VY_start, (unsigned char)s[i], fnt);
        if (w > 0) cx += w + 1;
    }
}

// ── Draw one timeout dot ──────────────────────────────────────────────────────
static void drawDot(int x, int VY, bool filled) {
    for (int ry = 0; ry < DOT_H; ry++) {
        for (int rx = 0; rx < DOT_W; rx++) {
            uint8_t val;
            if (filled) {
                val = 1;
            } else {
                val = (ry == 0 || ry == DOT_H - 1 ||
                       rx == 0 || rx == DOT_W - 1) ? 1 : 0;
            }
            writePixelRemapped(x + rx, VY + ry, val);
        }
    }
}

// ── Draw full TOL + possession zone ──────────────────────────────────────────
void drawTolZone(int timeoutsA, char possession, int timeoutsB) {
    dmd.clearScreen(true);   // GRAPHICS_INVERSE = clear (all OFF)

    dmd.selectFont(SystemFont5x7);

    // Team A — TOL label (VY=0) + 5 dots (VY=DOT_VY)
    {
        const char* lbl = "TOL";
        int w  = strPixelWidth(lbl, 3);
        int x  = TOL_A_X + max(0, (TOL_A_W - w) / 2);
        drawStringRemapped(x, 0, lbl, 3, SystemFont5x7);
        int x0 = TOL_A_X + max(0, (TOL_A_W - DOT_ROW_W) / 2);
        for (int d = 0; d < DOT_MAX; d++)
            drawDot(x0 + d * (DOT_W + DOT_GAP), DOT_VY, d < timeoutsA);
    }

    // Team B — TOL label (VY=0) + 5 dots (VY=DOT_VY)
    {
        const char* lbl = "TOL";
        int w  = strPixelWidth(lbl, 3);
        int x  = TOL_B_X + max(0, (TOL_B_W - w) / 2);
        drawStringRemapped(x, 0, lbl, 3, SystemFont5x7);
        int x0 = TOL_B_X + max(0, (TOL_B_W - DOT_ROW_W) / 2);
        for (int d = 0; d < DOT_MAX; d++)
            drawDot(x0 + d * (DOT_W + DOT_GAP), DOT_VY, d < timeoutsB);
    }

    // Possession arrows span both rows (VY=0..31 visual = 32 px)
    const int triW = 28, triH = 26;
    const int vy0  = (32 - triH) / 2;   // = 3, centres arrow in 32 px zone
    const int half = triH / 2;

    // ◄ Team A — right-justified → apex at LEFT
    if (possession == 'A') {
        int x0 = ARR_A_X + (ARR_W - triW) / 2;
        for (int r = 0; r < triH; r++) {
            int rowW   = triW - (abs(r - half) * triW / half);
            if (rowW < 1) rowW = 1;
            int startX = x0 + triW - rowW;
            for (int c = 0; c < rowW; c++)
                writePixelRemapped(startX + c, vy0 + r, 1);
        }
    }

    // ► Team B — left-justified → apex at RIGHT
    if (possession == 'B') {
        int x0 = ARR_B_X + (ARR_W - triW) / 2;
        for (int r = 0; r < triH; r++) {
            int rowW = triW - (abs(r - half) * triW / half);
            if (rowW < 1) rowW = 1;
            for (int c = 0; c < rowW; c++)
                writePixelRemapped(x0 + c, vy0 + r, 1);
        }
    }
}

// ── Check for new data ────────────────────────────────────────────────────────
void checkNewData() {
    if (!newData) return;
    newData = false;

    bool tolChanged = (rxBuf.timeoutsA  != lastTimeoutsA)  ||
                      (rxBuf.timeoutsB  != lastTimeoutsB)  ||
                      (rxBuf.possession != lastPossession);

    if (tolChanged) {
        lastTimeoutsA  = rxBuf.timeoutsA;
        lastTimeoutsB  = rxBuf.timeoutsB;
        lastPossession = rxBuf.possession;
        drawTolZone(rxBuf.timeoutsA, rxBuf.possession, rxBuf.timeoutsB);
        Serial.printf("TOL A=%d B=%d  Poss=%c\n",
            rxBuf.timeoutsA, rxBuf.timeoutsB, rxBuf.possession);
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    waitMs(500);

    // Startup splash
    dmd.clearScreen(true);
    dmd.selectFont(SystemFont5x7);
    dmd.drawString(0, 0, "SLAVE2", 6, GRAPHICS_NORMAL);
    waitMs(1200);

    // Default state: 0 timeouts, no possession
    drawTolZone(0, ' ', 0);

    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("MAC: ");
    Serial.println(WiFi.macAddress());

    memset(&rxBuf, 0, sizeof(rxBuf));

    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init FAILED");
        dmd.clearScreen(true);
        dmd.drawString(0, 0, "ERR", 3, GRAPHICS_NORMAL);
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Slave 2 v1.0 ready");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();
    checkNewData();
}
