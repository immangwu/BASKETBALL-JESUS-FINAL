/*
  Main Board  v4.0  —  Panel 1  (192 × 96 px)
  ─────────────────────────────────────────────────────────────────────────
  Hardware : 6 panels wide × 6 tall  (192 × 96 px)

  Row layout (y=0 is bottom):
    y=80..95  Row 1 : Event name  (SystemFont5x7, scroll or static)
    y=64..79  Row 2 : Combined team names  (SystemFont5x7, full width)
    y=32..63  Rows 3+4 (32px zone) : ScoreA | [GAME CLK label + Clock] | ScoreB
    y=0..31   Rows 5+6 (32px zone) : TOL-A | Possession Arrows | TOL-B

  Changes vs v2.0:
    • BoardData: added char marketingText[32] — matches master v3_0.
    • Event name: always SystemFont5x7 (thin single-pixel font).
      Scrolls right-to-left when eventScroll==1; static when ==0.
    • Team names: display combined teamA+teamB (32 chars total)
      in SystemFont5x7 across full 192px width. No split zones.
    • GAME CLK label: VY=0..6 above clock digits in clock column.
      Clock digits shifted to VY=8 (seconds) + VY=20 (tenths).
    • Independent clock: ESP32 runs its own 100ms countdown and
      only syncs to master on running-state change (stop→start or
      start→stop). While running, master values are IGNORED — no jitter.
*/

#include <DMD32.h>
#include "fonts/SystemFont5x7.h"
#include "fonts/Comic24.h"
#include <esp_now.h>
#include <WiFi.h>
#include "esp_wifi.h"

#define DISPLAYS_ACROSS 6
#define DISPLAYS_DOWN   6
#define DISPLAY_W       (32 * DISPLAYS_ACROSS)   // 192 px

// Virtual Y-starts (y=0 = bottom-most physical row)
#define Y_EVENT  80
#define Y_TEAMS  64
#define Y_ROW3   48
#define Y_ROW4   32
#define Y_ROW5   16
#define Y_ROW6    0

// Score zone — three 64-px columns
#define ZONE_W   64
#define ZONE_CX  64
#define ZONE_BX 128

// TOL zone layout (x=64..191 mapped into the 4 middle+right panels)
#define TOL_A_X   64
#define TOL_A_W   32
#define ARR_A_X   96
#define ARR_B_X  128
#define ARR_W     32
#define TOL_B_X  160
#define TOL_B_W   32

// Timeout dots — 7×7 px, 2 px gap, max 3
#define DOT_W      7
#define DOT_H      7
#define DOT_GAP    2
#define DOT_MAX    3
#define DOT_ROW_W  (DOT_MAX * DOT_W + (DOT_MAX - 1) * DOT_GAP)   // 25 px
#define DOT_VY    20

// Event scroll
#define SCROLL_SPEED_MS 40

DMD dmd(DISPLAYS_ACROSS, DISPLAYS_DOWN);

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
    char marketingText[32];
} BoardData;

BoardData     rxBuf;
volatile bool newData = false;

void onReceive(const uint8_t* mac, const uint8_t* data, int len) {
    if (len == 1 && data[0] == 0xAA) { ESP.restart(); return; }
    if (len == sizeof(BoardData)) { memcpy(&rxBuf, data, sizeof(rxBuf)); newData = true; }
}

// ── String pixel-width (uses currently selected font) ─────────────────────────
int strPixelWidth(const char* s, int len) {
    int w = 0;
    for (int i = 0; i < len; i++) {
        int cw = dmd.charWidth((unsigned char)s[i]);
        if (cw > 0) w += cw + 1;
    }
    return w;
}

// ── Cross-row pixel helper (for 32px score/TOL zones) ─────────────────────────
static void writePixelRemapped(int x, int VY, int baseY, uint8_t val) {
    int dmd_y = (VY < 16) ? (baseY + VY + 16) : (baseY + VY - 16);
    int dmd_x = x;
    if (baseY == Y_ROW6 && VY >= 16) dmd_x = x - 64;
    if (dmd_y < 0 || dmd_y >= DISPLAYS_DOWN * 16) return;
    if (dmd_x < 0 || dmd_x >= DISPLAY_W) return;
    dmd.writePixel(dmd_x, dmd_y, GRAPHICS_NORMAL, val);
}

// ── Cross-row character draw ───────────────────────────────────────────────────
int drawCharRemapped(int bX, int VY_start, unsigned char letter,
                     const uint8_t* fnt, int baseY) {
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
    bool fixedW = (pgm_read_byte(fnt + FONT_LENGTH) == 0 &&
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
                writePixelRemapped(bX + j, VY, baseY, (data & (1 << k)) ? 1 : 0);
            }
        }
    }
    return width;
}

void drawStringRemapped(int x, int VY_start, const char* s, int len,
                        const uint8_t* fnt, int baseY) {
    int cx = x;
    for (int i = 0; i < len; i++) {
        int w = drawCharRemapped(cx, VY_start, (unsigned char)s[i], fnt, baseY);
        if (w > 0) cx += w + 1;
    }
}

// ── Vertically-scaled character draw (nearest-neighbour) ──────────────────────
int drawCharScaledRemapped(int bX, int VY_start, unsigned char letter,
                           const uint8_t* fnt, int baseY, int srcH, int dstH) {
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
    bool fixedW = (pgm_read_byte(fnt + FONT_LENGTH) == 0 &&
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
        for (int sy = 0; sy < srcH; sy++) {
            uint8_t data = pgm_read_byte(fnt + index + j + ((sy / 8) * width));
            uint8_t pix  = (data & (1 << (sy % 8))) ? 1 : 0;
            int dy0 = sy * dstH / srcH;
            int dy1 = (sy + 1) * dstH / srcH;
            if (dy1 <= dy0) dy1 = dy0 + 1;
            for (int dy = dy0; dy < dy1; dy++)
                writePixelRemapped(bX + j, VY_start + dy, baseY, pix);
        }
    }
    return width;
}

void drawStringScaledRemapped(int x, int VY_start, const char* s, int len,
                               const uint8_t* fnt, int baseY, int srcH, int dstH) {
    int cx = x;
    for (int i = 0; i < len; i++) {
        int w = drawCharScaledRemapped(cx, VY_start, (unsigned char)s[i], fnt, baseY, srcH, dstH);
        if (w > 0) cx += w + 1;
    }
}

// ── Event name row (Row 1) ────────────────────────────────────────────────────
char  currentEventText[33]  = "WAITING";
bool  currentEventScroll    = false;
int   eventScrollX          = DISPLAY_W;
unsigned long lastScrollMs  = 0;

// Draw static event name centered in SystemFont5x7
void drawEventStatic(const char* text) {
    int len = strnlen(text, 32);
    dmd.selectFont(SystemFont5x7);
    int textW = strPixelWidth(text, len);
    int drawX = max(0, (DISPLAY_W - textW) / 2);
    dmd.drawFilledBox(0, Y_EVENT, DISPLAY_W - 1, Y_EVENT + 15, GRAPHICS_INVERSE);
    dmd.drawString(drawX, Y_EVENT + 4, text, len, GRAPHICS_NORMAL);
}

// Advance scroll by one pixel (call from loop when scrolling)
void stepEventScroll() {
    unsigned long now = millis();
    if ((long)(now - lastScrollMs) < SCROLL_SPEED_MS) return;
    lastScrollMs = now;
    int len = strnlen(currentEventText, 32);
    dmd.selectFont(SystemFont5x7);
    int textW = strPixelWidth(currentEventText, len);
    dmd.drawFilledBox(0, Y_EVENT, DISPLAY_W - 1, Y_EVENT + 15, GRAPHICS_INVERSE);
    dmd.drawString(eventScrollX, Y_EVENT + 4, currentEventText, len, GRAPHICS_NORMAL);
    eventScrollX--;
    if (eventScrollX < -(textW + 4)) eventScrollX = DISPLAY_W;
}

// ── Team names row (Row 2) ────────────────────────────────────────────────────
// teamA (16 chars) + teamB (16 chars) → 32-char combined display in SystemFont5x7
char teamDisplay[33] = "TEAM A          TEAM B          ";

void drawTeamNamesRow(const char* combined) {
    // Trim trailing spaces for pixel-width calc, but draw full buffer
    int len = 32;
    while (len > 0 && combined[len - 1] == ' ') len--;
    dmd.selectFont(SystemFont5x7);
    int textW = strPixelWidth(combined, len);
    int drawX = max(0, (DISPLAY_W - textW) / 2);
    dmd.drawFilledBox(0, Y_TEAMS, DISPLAY_W - 1, Y_TEAMS + 15, GRAPHICS_INVERSE);
    if (len > 0)
        dmd.drawString(drawX, Y_TEAMS + 4, combined, len, GRAPHICS_NORMAL);
}

// ── Score zone (Rows 3+4, 32px virtual) ───────────────────────────────────────
//  Score A/B : Comic24 stretched to 32px (full zone height).
//  Clock column:
//    VY=0..6  : "GAME CLK" label (SystemFont5x7, centred)
//    VY=8..31 : Clock digits (Comic24, 24px)
//               ≥60s  → "M:SS"
//               <60s  → seconds (24px, VY=8) + .tenths (12px, VY=20)
int lastDrawScoreA    = -1;
int lastDrawScoreB    = -1;
int lastDrawClockSecs = -1;
int lastDrawClockT    = -1;

void drawScoreZone(int scoreA, int clockSecs, int clockTenths, int scoreB) {
    lastDrawScoreA    = scoreA;
    lastDrawScoreB    = scoreB;
    lastDrawClockSecs = clockSecs;
    lastDrawClockT    = clockTenths;

    dmd.drawFilledBox(0, Y_ROW4, DISPLAY_W - 1, Y_ROW3 + 15, GRAPHICS_INVERSE);

    // ── Score A ──
    dmd.selectFont(Comic24);
    char sa[6]; snprintf(sa, sizeof(sa), "%d", scoreA);
    int wA = strPixelWidth(sa, strlen(sa));
    drawStringScaledRemapped(max(0, (ZONE_W - wA) / 2), 0, sa, strlen(sa),
                             Comic24, Y_ROW4, 24, 32);

    // ── Score B ──
    dmd.selectFont(Comic24);
    char sb[6]; snprintf(sb, sizeof(sb), "%d", scoreB);
    int wB = strPixelWidth(sb, strlen(sb));
    drawStringScaledRemapped(ZONE_BX + max(0, (ZONE_W - wB) / 2), 0, sb, strlen(sb),
                             Comic24, Y_ROW4, 24, 32);

    // ── GAME CLK label (VY=0..6) ──
    dmd.selectFont(SystemFont5x7);
    const char* gcl = "GAME CLK";
    int gclen = 8;
    int gcw   = strPixelWidth(gcl, gclen);
    drawStringRemapped(ZONE_CX + max(0, (ZONE_W - gcw) / 2), 0,
                       gcl, gclen, SystemFont5x7, Y_ROW4);

    // ── Clock digits (VY=8..31) ──
    dmd.selectFont(Comic24);
    if (clockSecs >= 60) {
        int m = clockSecs / 60, s = clockSecs % 60;
        char cl[7]; snprintf(cl, sizeof(cl), "%d:%02d", m, s);
        int wC = strPixelWidth(cl, strlen(cl));
        int xC = ZONE_CX + max(0, (ZONE_W - wC) / 2);
        drawStringRemapped(xC, 8, cl, strlen(cl), Comic24, Y_ROW4);
    } else {
        // Seconds full-size (24px, VY=8..31), tenths half-size (12px, VY=20..31)
        char ss[4]; snprintf(ss, sizeof(ss), "%d", clockSecs);
        char tt[4]; snprintf(tt, sizeof(tt), ".%d", clockTenths);
        int wSS = strPixelWidth(ss, strlen(ss));
        int wTT = strPixelWidth(tt, strlen(tt));
        int totalW = wSS + wTT;
        int xC = ZONE_CX + max(0, (ZONE_W - totalW) / 2);
        drawStringRemapped(xC,        8,  ss, strlen(ss), Comic24, Y_ROW4);
        drawStringScaledRemapped(xC + wSS, 20, tt, strlen(tt), Comic24, Y_ROW4, 24, 12);
    }
}

// ── Timeout dot ───────────────────────────────────────────────────────────────
static void drawDot(int x, int VY) {
    for (int ry = 0; ry < DOT_H; ry++) {
        for (int rx = 0; rx < DOT_W; rx++) {
            if ((ry == 0 || ry == DOT_H - 1) && (rx == 0 || rx == DOT_W - 1)) continue;
            writePixelRemapped(x + rx, VY + ry, Y_ROW6, 1);
        }
    }
}

// ── TOL zone (Rows 5+6) ───────────────────────────────────────────────────────
void drawTolZone(int timeoutsA, char possession, int timeoutsB) {
    dmd.drawFilledBox(0, Y_ROW6, DISPLAY_W - 1, Y_ROW5 + 15, GRAPHICS_INVERSE);

    // Timeout dots A
    {
        int x0 = TOL_A_X + max(0, (TOL_A_W - DOT_ROW_W) / 2);
        for (int d = 0; d < timeoutsA && d < DOT_MAX; d++)
            drawDot(x0 + d * (DOT_W + DOT_GAP), DOT_VY);
    }
    // Timeout dots B
    {
        int x0 = TOL_B_X + max(0, (TOL_B_W - DOT_ROW_W) / 2);
        for (int d = 0; d < timeoutsB && d < DOT_MAX; d++)
            drawDot(x0 + d * (DOT_W + DOT_GAP), DOT_VY);
    }
    // Possession arrow A (left-pointing)
    if (possession == 'A') {
        for (int vy = 3; vy <= 27; vy++) {
            int rowW  = 24 - (abs(vy - 15) * 2);
            if (rowW < 1) rowW = 1;
            int startX = 124 - rowW;
            for (int c = 0; c < rowW; c++) writePixelRemapped(startX + c, vy, Y_ROW6, 1);
        }
    }
    // Possession arrow B (right-pointing)
    if (possession == 'B') {
        for (int vy = 3; vy <= 27; vy++) {
            int rowW  = 24 - (abs(vy - 15) * 2);
            if (rowW < 1) rowW = 1;
            for (int c = 0; c < rowW; c++) writePixelRemapped(132 + c, vy, Y_ROW6, 1);
        }
    }
}

// ── Independent game clock ────────────────────────────────────────────────────
int  localClockSecs    = 600;
int  localClockTenths  = 0;
bool localClockRunning = false;
int  prevClockRunning  = -1;   // -1 = first packet not yet received
unsigned long lastClockTickMs = 0;

void syncClock(int ms, int mt, int mr) {
    if (mr && !localClockRunning) {
        // START event: snap to master and begin counting
        localClockSecs    = ms;
        localClockTenths  = mt;
        localClockRunning = true;
        lastClockTickMs   = millis();
    } else if (!mr && localClockRunning) {
        // STOP event: stop and snap to master's final value
        localClockRunning = false;
        localClockSecs    = ms;
        localClockTenths  = mt;
    } else if (!mr) {
        // Clock not running: always accept master value (handles reset/set)
        localClockSecs   = ms;
        localClockTenths = mt;
    }
    // If running: DO NOTHING — run autonomously, master clock ignored
}

void tickClock() {
    if (!localClockRunning) return;
    unsigned long now = millis();
    if ((long)(now - lastClockTickMs) < 100) return;
    lastClockTickMs = now;
    localClockTenths--;
    if (localClockTenths < 0) {
        localClockTenths = 9;
        localClockSecs--;
        if (localClockSecs < 0) {
            localClockSecs    = 0;
            localClockTenths  = 0;
            localClockRunning = false;
        }
    }
}

// ── Cache for last drawn clock (avoid full redraw when nothing changed) ────────
int  lastTickSecs   = -1;
int  lastTickTenths = -1;

// ── Cached state ─────────────────────────────────────────────────────────────
char eventText[33]    = "WAITING";
char teamAText[17]    = "TEAM A          ";
char teamBText[17]    = "TEAM B          ";
int  lastScoreA       = -1;
int  lastScoreB       = -1;
int  lastTimeoutsA    = -1;
int  lastTimeoutsB    = -1;
char lastPossession   = ' ';

// ── Process received data ─────────────────────────────────────────────────────
void checkNewData() {
    if (!newData) return;
    newData = false;

    // Event name
    bool eventChanged = (strncmp(eventText, rxBuf.eventName, 32) != 0) ||
                        (currentEventScroll != (bool)rxBuf.eventScroll);
    if (eventChanged) {
        strncpy(eventText, rxBuf.eventName, 32); eventText[32] = '\0';
        currentEventScroll = (rxBuf.eventScroll != 0);
        strncpy(currentEventText, eventText, 32); currentEventText[32] = '\0';
        eventScrollX = DISPLAY_W;
        if (!currentEventScroll) drawEventStatic(eventText);
        Serial.printf("Event: %s  scroll=%d\n", eventText, currentEventScroll ? 1 : 0);
    }

    // Team names — combine teamA + teamB into 32-char display string
    bool teamsChanged = (strncmp(teamAText, rxBuf.teamA, 16) != 0) ||
                        (strncmp(teamBText, rxBuf.teamB, 16) != 0);
    if (teamsChanged) {
        strncpy(teamAText, rxBuf.teamA, 16); teamAText[16] = '\0';
        strncpy(teamBText, rxBuf.teamB, 16); teamBText[16] = '\0';
        // Build 32-char combined buffer (teamA left-padded 16 chars, teamB next 16)
        memset(teamDisplay, ' ', 32);
        teamDisplay[32] = '\0';
        int la = strnlen(teamAText, 16);
        int lb = strnlen(teamBText, 16);
        memcpy(teamDisplay,      teamAText, la);
        memcpy(teamDisplay + 16, teamBText, lb);
        drawTeamNamesRow(teamDisplay);
        Serial.printf("Teams: [%s][%s]\n", teamAText, teamBText);
    }

    // Score
    bool scoreChanged = (rxBuf.scoreA != lastScoreA) || (rxBuf.scoreB != lastScoreB);
    if (scoreChanged) {
        lastScoreA = rxBuf.scoreA;
        lastScoreB = rxBuf.scoreB;
        drawScoreZone(lastScoreA, localClockSecs, localClockTenths, lastScoreB);
    }

    // TOL / possession
    bool tolChanged = (rxBuf.timeoutsA != lastTimeoutsA) ||
                      (rxBuf.timeoutsB != lastTimeoutsB) ||
                      (rxBuf.possession != lastPossession);
    if (tolChanged) {
        lastTimeoutsA  = rxBuf.timeoutsA;
        lastTimeoutsB  = rxBuf.timeoutsB;
        lastPossession = rxBuf.possession;
        drawTolZone(rxBuf.timeoutsA, rxBuf.possession, rxBuf.timeoutsB);
    }

    // Clock sync — only acts on running-state changes
    syncClock(rxBuf.clockSecs, rxBuf.clockTenths, rxBuf.clockRunning);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    waitMs(500);
    dmd.clearScreen(true);
    dmd.selectFont(SystemFont5x7);
    dmd.drawString(72, Y_EVENT + 4, "WAIT", 4, GRAPHICS_NORMAL);
    waitMs(1200);
    dmd.clearScreen(true);
    drawEventStatic(eventText);
    drawTeamNamesRow(teamDisplay);
    drawScoreZone(0, 600, 0, 0);
    drawTolZone(0, ' ', 0);
    WiFi.mode(WIFI_STA);
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.print("Main board MAC: "); Serial.println(WiFi.macAddress());
    memset(&rxBuf, 0, sizeof(rxBuf));
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW FAILED");
        dmd.clearScreen(true);
        dmd.selectFont(SystemFont5x7);
        dmd.drawString(80, Y_EVENT + 4, "ERR", 3, GRAPHICS_NORMAL);
    } else {
        esp_now_register_recv_cb(onReceive);
        Serial.println("Main board v4.0 ready");
    }
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    scanIfNeeded();

    // Tick local clock
    tickClock();

    // Redraw score zone only when clock value changes
    bool clockValChanged = (localClockSecs != lastTickSecs) ||
                           (localClockSecs < 60 && localClockTenths != lastTickTenths);
    if (clockValChanged) {
        lastTickSecs   = localClockSecs;
        lastTickTenths = localClockTenths;
        drawScoreZone(lastScoreA, localClockSecs, localClockTenths, lastScoreB);
    }

    // Scroll event name if active
    if (currentEventScroll) stepEventScroll();

    // Process incoming ESP-NOW data
    checkNewData();
}
