// TokenLimitMonitor — premium TFT dashboard for live token / quota usage.
//
// Data is NOT hardcoded anymore: the board listens on the USB serial port for
// JSON snapshots and re-renders in real time. See token_feed.py for a sample
// feeder. Expected line-delimited JSON (one full snapshot per line):
//
//   {"services":[
//     {"name":"CLAUDE CODE","provider":"Anthropic","reset":"8:00 PM",
//      "metrics":[{"label":"Context Window","pct":35,"value":"75k / 200k"},
//                 {"label":"Monthly Cost","pct":55,"value":"$5.50 / $10"}]},
//     ... up to NUM_SERVICES services (currently 2) ...
//   ]}
//
// Requires the ArduinoJson library (v7.x) — install via Arduino Library Manager.

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <ArduinoJson.h>
#include <math.h>

// ---- Which serial port the PC feeder talks to ----------------------------
// ESP32 DevKit (CP2102/CH340 USB-UART bridge): Serial0 is UART0 over USB.
// Native-USB boards (S2/S3/C3): switch this to `Serial` or `USBSerial`.
#define DATA_SERIAL Serial0

TFT_eSPI tft;
TFT_eSprite rowSprite    = TFT_eSprite(&tft);  // reused strip for flicker-free metric rows
TFT_eSprite peakSprite   = TFT_eSprite(&tft);  // big % number (keeps its smooth font loaded)
TFT_eSprite mascotSprite = TFT_eSprite(&tft);  // little running mascot in the footer lane

// ---- Smooth (anti-aliased VLW) fonts -------------------------------------
// Drop NotoSansBold15.h / NotoSansBold36.h into this sketch folder and they
// are picked up automatically. Without them the build still compiles and
// falls back to the built-in (blocky) GLCD font. See the usage notes.
#if defined(__has_include)
  #if __has_include("NotoSansBold15.h")
    #include "NotoSansBold15.h"
    #define HAVE_FONT_TITLE 1
  #endif
  #if __has_include("NotoSansBold36.h")
    #include "NotoSansBold36.h"
    #define HAVE_FONT_HERO 1
  #endif
#endif

// Service names + header title tier (loaded/unloaded on demand — infrequent).
inline void fontTitle() {
#ifdef HAVE_FONT_TITLE
  tft.loadFont(NotoSansBold15);
#else
  tft.setTextFont(1);
  tft.setTextSize(2);
#endif
}
inline void fontTitleDone() {
#ifdef HAVE_FONT_TITLE
  tft.unloadFont();
#endif
}

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------
constexpr int NUM_SERVICES = 2;                    // Claude Code + Codex

struct Metric {
  char    label[24];
  char    value[24];
  uint8_t pct;
};

struct Service {
  char   name[16];
  char   provider[20];
  char   resetAt[20];
  Metric m[2];
};

Service services[NUM_SERVICES];

// Per-metric animation state (interpolates old -> new on every data update).
struct Anim {
  uint8_t       from;
  uint8_t       cur;
  unsigned long startMs;
};
Anim anim[NUM_SERVICES][2];

bool          hasData = false;
unsigned long bootMs = 0;
unsigned long lastDataMs = 0;
unsigned long lastBlinkMs = 0;
unsigned long lastFooterMs = 0;
unsigned long lastMascotMs = 0;
bool          blinkOn = true;

int  mascotX = 36;       // position within the lane (>= DOG_MARGIN)
int  mascotDir = 1;      // +1 right, -1 left
int  mascotFrame = 0;    // run-cycle frame counter

constexpr uint16_t ANIM_MS = 900;     // bar/number animation duration
constexpr uint16_t STALE_MS = 8000;   // mark data "stale" after this many ms

// ---------------------------------------------------------------------------
// Geometry  (480x320, rotation 3)
// ---------------------------------------------------------------------------
constexpr int SCREEN_W = 480;
constexpr int SCREEN_H = 320;
constexpr int HEADER_H = 40;
constexpr int FOOTER_H = 48;   // taller: gives the dog room to run, bigger
constexpr int FOOTER_Y = SCREEN_H - FOOTER_H;

constexpr int SIDE_PAD = 10;
constexpr int CARD_GAP = 10;
constexpr int CARD_W   = (SCREEN_W - 2 * SIDE_PAD - (NUM_SERVICES - 1) * CARD_GAP) / NUM_SERVICES;  // 225
constexpr int CARD_TOP = HEADER_H + 8;            // 48
constexpr int CARD_BOT = FOOTER_Y - 6;            // 266
constexpr int CARD_H   = CARD_BOT - CARD_TOP;     // 218
constexpr int CARD_R   = 10;                       // corner radius

constexpr int CONTENT_X_OFF = 16;                  // left padding (past accent spine)
constexpr int INNER_W = CARD_W - CONTENT_X_OFF - 14;  // 195

constexpr int ROW_H = 44;                          // metric row sprite height

// Card content layout (offsets from CARD_TOP)
constexpr int PROV_Y    = 14;
constexpr int NAME_Y    = 28;
constexpr int PILL_Y    = 14;   // pill sits top-right, beside the name
constexpr int PEAK_Y    = 46;   // big centred % number
constexpr int DIVIDER_Y = 90;
constexpr int RESET_Y   = 98;
constexpr int ROW0_Y    = 116;
constexpr int ROW_STEP  = 48;
constexpr int BACKLIGHT_PIN = 21;

// Mascot lane: the empty centre strip of the (now taller) footer.
constexpr int LANE_X = 152;
constexpr int LANE_W = 176;
constexpr int LANE_H = 42;
constexpr int MASCOT_Y = FOOTER_Y + 3;

inline int cardX(int col) { return SIDE_PAD + col * (CARD_W + CARD_GAP); }
inline int contentX(int col) { return cardX(col) + CONTENT_X_OFF; }

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

constexpr uint16_t BG_TOP   = rgb565(0x12, 0x1A, 0x2B);
constexpr uint16_t BG_BOT   = rgb565(0x0B, 0x10, 0x1C);
constexpr uint16_t BG_CARD  = rgb565(0x18, 0x22, 0x37);
constexpr uint16_t BORDER   = rgb565(0x2C, 0x3A, 0x52);
constexpr uint16_t TEXT_PRI = rgb565(0xF4, 0xF8, 0xFF);
constexpr uint16_t TEXT_SUB = rgb565(0x86, 0x9B, 0xBA);
constexpr uint16_t TEXT_DIM = rgb565(0x55, 0x66, 0x80);
constexpr uint16_t OK_COL   = rgb565(0x34, 0xD3, 0x99);
constexpr uint16_t WARN_COL = rgb565(0xF5, 0xB7, 0x2E);
constexpr uint16_t CRIT_COL = rgb565(0xF8, 0x6E, 0x7C);
constexpr uint16_t BLUE_COL = rgb565(0x6F, 0xB1, 0xFF);
constexpr uint16_t BAR_BG   = rgb565(0x0E, 0x16, 0x24);
constexpr uint16_t DOG_COL  = rgb565(0xF2, 0x9E, 0x38);   // shiba orange
constexpr uint16_t DOG_DARK = rgb565(0x8A, 0x4B, 0x1E);   // outline / nose
constexpr uint16_t CREAM    = rgb565(0xFD, 0xF3, 0xDC);   // chest / muzzle / paws / tail tip

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
uint16_t lerp565(uint16_t a, uint16_t b, float t) {
  int ar = (a >> 11) & 0x1F, ag = (a >> 5) & 0x3F, ab = a & 0x1F;
  int br = (b >> 11) & 0x1F, bg = (b >> 5) & 0x3F, bb = b & 0x1F;
  int r = ar + static_cast<int>((br - ar) * t);
  int g = ag + static_cast<int>((bg - ag) * t);
  int bl = ab + static_cast<int>((bb - ab) * t);
  return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

uint16_t statusColor(uint8_t pct) {
  if (pct >= 80) return CRIT_COL;
  if (pct >= 55) return WARN_COL;
  return OK_COL;
}

const char* statusLabel(uint8_t pct) {
  if (pct >= 80) return "CRITICAL";
  if (pct >= 55) return "WARNING";
  return "OK";
}

// The card's headline/status follows the PRIMARY metric (index 0) -- e.g. the
// Context gauge for Claude, the 5h limit for Codex. The second metric still
// shows its own coloured bar in its row, but doesn't hijack the big number.
uint8_t peakOf(int col) {
  return anim[col][0].cur;
}

uint8_t targetPeak(int col) {
  return services[col].m[0].pct;
}

void copyStr(char* dst, const char* src, size_t cap) {
  if (!src) src = "--";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

void setFont(uint8_t size, uint16_t fg, uint16_t bg) {
  tft.setTextFont(1);
  tft.setTextSize(size);
  tft.setTextColor(fg, bg);
}

void drawRightText(const char* text, int rightX, int y) {
  tft.drawString(text, rightX - tft.textWidth(text), y);
}

void drawCenterText(const char* text, int centerX, int y) {
  tft.drawString(text, centerX - (tft.textWidth(text) / 2), y);
}

void drawSpacedText(const char* text, int x, int y, uint8_t size, uint8_t spacing,
                    uint16_t color, uint16_t bg) {
  setFont(size, color, bg);
  int cursorX = x;
  const int charW = 6 * size;
  while (*text != '\0') {
    char c[2] = {*text, '\0'};
    tft.drawString(c, cursorX, y);
    cursorX += charW + spacing;
    ++text;
  }
}

void drawWarningIcon(int x, int y, uint16_t color, uint16_t bg) {
  tft.fillTriangle(x + 6, y, x, y + 12, x + 12, y + 12, bg);
  tft.drawTriangle(x + 6, y, x, y + 12, x + 12, y + 12, color);
  tft.drawFastVLine(x + 6, y + 4, 4, color);
  tft.drawPixel(x + 6, y + 10, color);
}

// ---------------------------------------------------------------------------
// Static chrome (drawn once)
// ---------------------------------------------------------------------------
void drawBackground() {
  for (int y = 0; y < SCREEN_H; ++y) {
    float t = static_cast<float>(y) / (SCREEN_H - 1);
    tft.drawFastHLine(0, y, SCREEN_W, lerp565(BG_TOP, BG_BOT, t));
  }
}

void drawCardFrame(int col) {
  const int x = cardX(col);
  tft.fillRoundRect(x, CARD_TOP, CARD_W, CARD_H, CARD_R, BG_CARD);
  tft.drawRoundRect(x, CARD_TOP, CARD_W, CARD_H, CARD_R, BORDER);
}

void drawHeaderChrome() {
  tft.drawRoundRect(SIDE_PAD, 11, 20, 20, 5, BLUE_COL);
  tft.drawRoundRect(SIDE_PAD + 1, 12, 18, 18, 4, BLUE_COL);
  setFont(2, BLUE_COL, BG_TOP);
  drawCenterText("T", SIDE_PAD + 10, 14);

  fontTitle();
  tft.setTextColor(TEXT_PRI, BG_TOP);
  tft.drawString("TOKEN MONITOR", SIDE_PAD + 28, 13);
  fontTitleDone();

  tft.drawFastHLine(0, HEADER_H, SCREEN_W, BORDER);
}

void drawFooterChrome() {
  tft.drawFastHLine(0, FOOTER_Y, SCREEN_W, BORDER);
}

// ---------------------------------------------------------------------------
// Dynamic header: connection pill + critical alert
// ---------------------------------------------------------------------------
void drawHeaderStatus() {
  const unsigned long now = millis();
  const bool stale = hasData && (now - lastDataMs > STALE_MS);

  // critical count + alert (left of the pill)
  int crit = 0;
  if (hasData) {
    for (int i = 0; i < NUM_SERVICES; ++i)
      if (targetPeak(i) >= 80) crit++;
  }

  tft.fillRect(250, 8, SCREEN_W - 250 - SIDE_PAD, HEADER_H - 10, BG_TOP);

  // connection pill (right edge)
  uint16_t col;
  const char* txt;
  if (!hasData)      { col = WARN_COL; txt = "WAITING"; }
  else if (stale)    { col = CRIT_COL; txt = "STALE";   }
  else               { col = OK_COL;   txt = "LIVE";    }

  const int pillW = 74, pillH = 22, pillY = 9;
  const int pillX = SCREEN_W - SIDE_PAD - pillW;
  tft.fillRoundRect(pillX, pillY, pillW, pillH, 11, BG_CARD);
  tft.drawRoundRect(pillX, pillY, pillW, pillH, 11, col);
  const bool dotOn = (col == OK_COL) || blinkOn;  // pulse for waiting/stale
  if (dotOn) tft.fillCircle(pillX + 16, pillY + pillH / 2, 4, col);
  setFont(1, col, BG_CARD);
  tft.drawString(txt, pillX + 28, pillY + 7);

  if (crit > 0 && blinkOn) {
    char alert[20];
    snprintf(alert, sizeof(alert), "%d CRITICAL", crit);
    int aw = 12 + 6 + tft.textWidth(alert);
    int ax = pillX - 14 - aw;
    drawWarningIcon(ax, 14, CRIT_COL, BG_TOP);
    setFont(1, CRIT_COL, BG_TOP);
    tft.drawString(alert, ax + 18, 16);
  }
}

// ---------------------------------------------------------------------------
// Dynamic footer: uptime + freshness
// ---------------------------------------------------------------------------
void fmtUptime(char* buf, size_t cap) {
  unsigned long s = (millis() - bootMs) / 1000;
  unsigned long d = s / 86400; s %= 86400;
  unsigned int  h = s / 3600;  s %= 3600;
  unsigned int  m = s / 60;
  unsigned int  sec = s % 60;
  if (d)      snprintf(buf, cap, "Up %lud %uh %um", d, h, m);
  else if (h) snprintf(buf, cap, "Up %uh %um", h, m);
  else        snprintf(buf, cap, "Up %um %us", m, sec);
}

void drawFooter() {
  const unsigned long now = millis();
  const bool stale = hasData && (now - lastDataMs > STALE_MS);

  // Clear only the text regions; the centre lane belongs to the mascot.
  tft.fillRect(0, FOOTER_Y + 1, LANE_X - 2, FOOTER_H - 1, BG_BOT);
  tft.fillRect(LANE_X + LANE_W + 2, FOOTER_Y + 1,
               SCREEN_W - (LANE_X + LANE_W + 2), FOOTER_H - 1, BG_BOT);

  const int textY = FOOTER_Y + (FOOTER_H - 8) / 2;   // vertically centred in the footer

  uint16_t dot = !hasData ? WARN_COL : (stale ? CRIT_COL : OK_COL);
  tft.fillCircle(SIDE_PAD + 6, textY + 4, 4, dot);

  char left[28];
  fmtUptime(left, sizeof(left));
  setFont(1, TEXT_SUB, BG_BOT);
  tft.drawString(left, SIDE_PAD + 18, textY);

  char right[28];
  uint16_t rcol;
  if (!hasData) {
    snprintf(right, sizeof(right), "WAITING FOR DATA");
    rcol = TEXT_DIM;
  } else {
    float ago = (now - lastDataMs) / 1000.0f;
    if (stale) { snprintf(right, sizeof(right), "STALE %.0fs ago", ago); rcol = CRIT_COL; }
    else       { snprintf(right, sizeof(right), "Updated %.1fs ago", ago); rcol = OK_COL; }
  }
  setFont(1, rcol, BG_BOT);
  drawRightText(right, SCREEN_W - SIDE_PAD, textY);
}

// ---------------------------------------------------------------------------
// Mascot — a little bot trotting along the footer lane (pure decoration)
// ---------------------------------------------------------------------------
bool anyCritical() {
  if (!hasData) return false;
  for (int i = 0; i < NUM_SERVICES; ++i)
    if (targetPeak(i) >= 80) return true;
  return false;
}

constexpr int DOG_MARGIN = 38;   // keep the whole shiba inside the lane

// Behaviour loop: run/walk around for 10 min, then curl up and sleep for 30 min.
constexpr unsigned long MASCOT_ACTIVE_MS   = 10UL * 60 * 1000;
constexpr unsigned long MASCOT_SLEEP_MS    = 30UL * 60 * 1000;
constexpr unsigned long MASCOT_SUBCYCLE_MS = 9000;   // alternate run/walk

// Erect shiba ear (pointy triangle) with a cream inner.
void drawEar(int ex, int topY, uint16_t col, uint16_t inner) {
  mascotSprite.fillTriangle(ex - 4, topY, ex + 4, topY, ex, topY - 9, col);
  mascotSprite.fillTriangle(ex - 2, topY - 1, ex + 2, topY - 1, ex, topY - 6, inner);
}

void drawShibaActive(bool running) {
  const bool crit = anyCritical();
  const uint16_t body = crit ? CRIT_COL : DOG_COL;
  const uint16_t dark = crit ? rgb565(0x7A, 0x22, 0x2A) : DOG_DARK;
  const int f = mascotDir > 0 ? 1 : -1;
  const bool phase = mascotFrame & 1;
  const int bob = (running && phase) ? 2 : 0;

  const int baseY = LANE_H - 4;
  const int cx = mascotX;
  const int by = baseY - 24 - bob;
  const int headCx = cx + f * 17;
  const int headCy = by + 3;
  const int legTop = by + 11;
  const int swing = running ? 3 : 2;

  mascotSprite.fillSprite(BG_BOT);
  mascotSprite.fillRoundRect(cx - 18, baseY + 1, 42, 3, 2, BORDER);   // shadow

  // legs (diagonal trot), cream paws
  const int legX[4]   = { cx + f * 11, cx + f * 5, cx - f * 4, cx - f * 10 };
  const int legGrp[4] = { 0, 1, 0, 1 };
  for (int i = 0; i < 4; ++i) {
    const bool lifted = (legGrp[i] == (phase ? 1 : 0));
    const int sx = legX[i] + (lifted ? f * swing : -f);
    const int bottom = lifted ? baseY - 3 : baseY;
    const uint16_t lc = (i >= 2) ? dark : body;
    mascotSprite.fillRect(sx - 1, legTop, 3, bottom - legTop, lc);
    mascotSprite.fillRect(sx - 1, bottom - 2, 3, 2, CREAM);          // paw
  }

  // curled tail over the back, cream tip
  const int tx = cx - f * 15, ty = by - 2;
  mascotSprite.fillCircle(tx, ty, 6, body);
  mascotSprite.fillCircle(tx, ty, 3, BG_BOT);                        // the curl gap
  mascotSprite.fillCircle(tx - f * 4, ty - 3, 2, CREAM);             // tail tip

  // body + cream chest
  mascotSprite.fillRoundRect(cx - 16, by, 32, 13, 6, body);
  mascotSprite.fillRoundRect(cx + f * 4, by + 5, 12, 8, 3, CREAM);

  // ears, head, cream muzzle, nose
  drawEar(headCx - 5, headCy - 6, body, CREAM);
  drawEar(headCx + 5, headCy - 6, body, CREAM);
  mascotSprite.fillCircle(headCx, headCy, 8, body);
  const int muzW = 10;
  const int muzX = (f > 0) ? headCx + 2 : headCx - 2 - muzW;
  mascotSprite.fillRoundRect(muzX, headCy + 1, muzW, 7, 3, CREAM);
  mascotSprite.fillCircle(headCx + f * 12, headCy + 3, 2, dark);     // nose

  // front legs over the body
  for (int i = 0; i < 2; ++i) {
    const bool lifted = (legGrp[i] == (phase ? 1 : 0));
    const int sx = legX[i] + (lifted ? f * swing : -f);
    const int bottom = lifted ? baseY - 3 : baseY;
    mascotSprite.fillRect(sx - 1, legTop, 3, bottom - legTop, body);
    mascotSprite.fillRect(sx - 1, bottom - 2, 3, 2, CREAM);
  }

  // eye + panting tongue while running
  mascotSprite.fillCircle(headCx + f * 4, headCy - 1, 2, dark);
  if (running && phase && !crit) {
    const int tX = (f > 0) ? headCx + 9 : headCx - 13;
    mascotSprite.fillRoundRect(tX, headCy + 7, 4, 4, 1, CRIT_COL);
  }

  mascotSprite.pushSprite(LANE_X, MASCOT_Y);
}

void drawShibaSleeping() {
  const uint16_t body = DOG_COL, dark = DOG_DARK;
  const int f = mascotDir > 0 ? 1 : -1;
  const int breath = ((mascotFrame / 10) & 1) ? 1 : 0;   // slow rise/fall

  const int baseY = LANE_H - 4;
  const int cx = mascotX;
  const int by = baseY - 13 - breath;                    // low, lying body
  const int headCx = cx + f * 18;
  const int headCy = baseY - 6;

  mascotSprite.fillSprite(BG_BOT);
  mascotSprite.fillRoundRect(cx - 22, baseY + 1, 48, 3, 2, BORDER);  // shadow

  // long resting body + cream belly
  mascotSprite.fillRoundRect(cx - 20, by, 42, 13, 7, body);
  mascotSprite.fillRoundRect(cx - 14, by + 6, 24, 7, 3, CREAM);

  // curled tail resting against the body
  mascotSprite.fillCircle(cx - f * 18, by + 3, 6, body);
  mascotSprite.fillCircle(cx - f * 18, by + 3, 3, BG_BOT);
  mascotSprite.fillCircle(cx - f * 20, by, 2, CREAM);

  // paws stretched forward
  mascotSprite.fillRoundRect(headCx - f * 2 - (f > 0 ? 0 : 10), baseY - 3, 10, 3, 1, CREAM);

  // head resting low, relaxed ears, closed eye
  drawEar(headCx - 4, headCy - 5, body, CREAM);
  drawEar(headCx + 6, headCy - 4, body, CREAM);
  mascotSprite.fillCircle(headCx, headCy, 8, body);
  const int muzX = (f > 0) ? headCx + 2 : headCx - 12;
  mascotSprite.fillRoundRect(muzX, headCy + 2, 10, 6, 3, CREAM);
  mascotSprite.fillCircle(headCx + f * 12, headCy + 4, 2, dark);     // nose on the ground
  mascotSprite.drawFastHLine(headCx + f * 1, headCy - 1, 4, dark);   // closed eye

  // Zzz drifting up and back
  const int n = (mascotFrame / 8) % 3;                   // 0..2 z's twinkle
  mascotSprite.setTextFont(1);
  mascotSprite.setTextColor(TEXT_SUB);
  const int zx = cx - f * 4;
  if (n >= 0) { mascotSprite.setTextSize(1); mascotSprite.drawString("z", zx, by - 6); }
  if (n >= 1) { mascotSprite.setTextSize(1); mascotSprite.drawString("z", zx + f * 6, by - 12); }
  if (n >= 2) { mascotSprite.setTextSize(2); mascotSprite.drawString("z", zx + f * 12, by - 22); }

  mascotSprite.pushSprite(LANE_X, MASCOT_Y);
}

void updateMascot() {
  const unsigned long t = (millis() - bootMs) % (MASCOT_ACTIVE_MS + MASCOT_SLEEP_MS);

  if (t >= MASCOT_ACTIVE_MS) {                 // sleeping
    mascotFrame++;
    drawShibaSleeping();
    return;
  }

  const bool running = ((t / MASCOT_SUBCYCLE_MS) & 1) == 0;
  const int base = running ? 5 : 2;
  const int speed = anyCritical() ? 7 : base;
  mascotX += mascotDir * speed;
  if (mascotX > LANE_W - DOG_MARGIN) { mascotX = LANE_W - DOG_MARGIN; mascotDir = -1; }
  if (mascotX < DOG_MARGIN)          { mascotX = DOG_MARGIN;          mascotDir = 1;  }
  mascotFrame++;
  drawShibaActive(running);
}

// ---------------------------------------------------------------------------
// Card content
// ---------------------------------------------------------------------------
constexpr int PILL_W = 84;
constexpr int PILL_H = 20;

void drawPill(int col, bool visible) {
  const int w = PILL_W, h = PILL_H;
  const int x = cardX(col) + CARD_W - 14 - w;     // top-right corner
  const int y = CARD_TOP + PILL_Y;
  const uint8_t peak = targetPeak(col);
  const uint16_t c = hasData ? statusColor(peak) : TEXT_DIM;

  tft.fillRect(x, y, w, h, BG_CARD);
  if (!visible) return;

  tft.fillRoundRect(x, y, w, h, h / 2, BG_BOT);
  tft.drawRoundRect(x, y, w, h, h / 2, c);
  tft.fillCircle(x + 13, y + h / 2, 3, c);
  setFont(1, c, BG_BOT);
  tft.drawString(hasData ? statusLabel(peak) : "WAITING", x + 24, y + (h - 8) / 2);
}

void drawAccent(int col) {
  const uint16_t c = hasData ? statusColor(targetPeak(col)) : TEXT_DIM;
  tft.fillRect(cardX(col) + 4, CARD_TOP + 12, 4, CARD_H - 24, c);
}

void drawDivider(int col) {
  tft.drawFastHLine(contentX(col), CARD_TOP + DIVIDER_Y, INNER_W, BORDER);
}

void drawPeak(int col) {
  const int x = cardX(col);
  const int y = CARD_TOP + PEAK_Y;
  const uint8_t peak = peakOf(col);
  const uint16_t c = hasData ? statusColor(targetPeak(col)) : TEXT_DIM;

  char txt[8];
  snprintf(txt, sizeof(txt), "%u%%", peak);
  peakSprite.fillSprite(BG_CARD);
  peakSprite.setTextColor(c, BG_CARD);
  const int tw = peakSprite.textWidth(txt);
  const int ty = (peakSprite.height() - peakSprite.fontHeight()) / 2;
  peakSprite.drawString(txt, (peakSprite.width() - tw) / 2, ty);
  peakSprite.pushSprite(x + 8, y);
}

void drawReset(int col) {
  const int x = contentX(col);
  const int y = CARD_TOP + RESET_Y;
  tft.fillRect(x, y, INNER_W, 10, BG_CARD);
  setFont(1, TEXT_DIM, BG_CARD);
  tft.drawString("RESET", x, y);
  setFont(1, TEXT_SUB, BG_CARD);
  tft.drawString(services[col].resetAt, x + 40, y);
}

void drawHeaderText(int col) {
  const int x = contentX(col);
  tft.fillRect(x, CARD_TOP + 10, CARD_W - 118, 38, BG_CARD);   // left zone only (pill keeps the right)
  drawSpacedText(services[col].provider, x, CARD_TOP + PROV_Y, 1, 2, TEXT_SUB, BG_CARD);
  fontTitle();
  tft.setTextColor(TEXT_PRI, BG_CARD);
  tft.drawString(services[col].name, x, CARD_TOP + NAME_Y);
  fontTitleDone();
}

// Metric row rendered into a sprite, then pushed (no flicker).
void drawMetricRow(int col, int row) {
  const Metric& mt = services[col].m[row];
  const uint8_t shownPct = anim[col][row].cur;
  const uint16_t c = hasData ? statusColor(mt.pct) : TEXT_DIM;
  const int x = contentX(col);
  const int y = CARD_TOP + ROW0_Y + row * ROW_STEP;

  rowSprite.fillSprite(BG_CARD);
  rowSprite.setTextFont(1);
  rowSprite.setTextSize(1);

  rowSprite.setTextColor(TEXT_SUB);
  rowSprite.drawString(mt.label, 0, 0);

  char pctText[8];
  snprintf(pctText, sizeof(pctText), "%u%%", shownPct);
  rowSprite.setTextColor(c);
  rowSprite.drawString(pctText, INNER_W - rowSprite.textWidth(pctText), 0);

  // progress track + rounded fill (taller bar for the wider cards)
  const int barY = 16, barH = 14, r = barH / 2;
  rowSprite.fillRoundRect(0, barY, INNER_W, barH, r, BAR_BG);
  rowSprite.drawRoundRect(0, barY, INNER_W, barH, r, BORDER);
  int fw = (INNER_W * shownPct) / 100;
  if (shownPct > 0 && fw < barH) fw = barH;
  if (fw > INNER_W) fw = INNER_W;
  if (fw > 0) rowSprite.fillRoundRect(0, barY, fw, barH, r, c);

  rowSprite.setTextColor(TEXT_PRI);
  rowSprite.drawString(mt.value, 0, barY + barH + 2);

  rowSprite.pushSprite(x, y);
}

void drawCardContent(int col) {
  drawAccent(col);
  drawHeaderText(col);
  drawPill(col, true);
  drawPeak(col);
  drawDivider(col);
  drawReset(col);
  drawMetricRow(col, 0);
  drawMetricRow(col, 1);
}

// ---------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------
void updateAnimation() {
  const unsigned long now = millis();
  for (int col = 0; col < NUM_SERVICES; ++col) {
    bool peakChanged = false;
    for (int row = 0; row < 2; ++row) {
      Anim& a = anim[col][row];
      const uint8_t target = services[col].m[row].pct;
      if (a.cur == target) continue;

      float t = (now - a.startMs) / static_cast<float>(ANIM_MS);
      uint8_t next;
      if (t >= 1.0f) {
        next = target;
        a.from = target;
      } else {
        float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);  // ease-out cubic
        next = static_cast<uint8_t>(lroundf(a.from + (target - a.from) * e));
      }
      if (next != a.cur) {
        a.cur = next;
        drawMetricRow(col, row);
        peakChanged = true;
      }
    }
    if (peakChanged) drawPeak(col);
  }
}

// ---------------------------------------------------------------------------
// Serial intake (non-blocking, line-delimited JSON)
// ---------------------------------------------------------------------------
char  rxBuf[640];
int   rxLen = 0;
bool  rxSkip = false;

void applySnapshot(JsonArrayConst arr) {
  const unsigned long now = millis();
  int n = 0;
  for (JsonObjectConst s : arr) {
    if (n >= NUM_SERVICES) break;
    copyStr(services[n].name,     s["name"]     | "--", sizeof(services[n].name));
    copyStr(services[n].provider, s["provider"] | "--", sizeof(services[n].provider));
    copyStr(services[n].resetAt,  s["reset"]    | "--", sizeof(services[n].resetAt));

    JsonArrayConst ms = s["metrics"].as<JsonArrayConst>();
    int mi = 0;
    for (JsonObjectConst mo : ms) {
      if (mi >= 2) break;
      Metric& mt = services[n].m[mi];
      copyStr(mt.label, mo["label"] | "--",  sizeof(mt.label));
      copyStr(mt.value, mo["value"] | "--",  sizeof(mt.value));
      int p = mo["pct"] | 0;
      if (p < 0) p = 0; if (p > 100) p = 100;
      mt.pct = static_cast<uint8_t>(p);

      // (re)start animation from whatever is currently on screen
      if (anim[n][mi].cur != mt.pct) {
        anim[n][mi].from = anim[n][mi].cur;
        anim[n][mi].startMs = now;
      }
      mi++;
    }
    n++;
  }

  hasData = true;
  lastDataMs = now;

  // repaint data-dependent statics; the animator then takes over any row that
  // is mid-transition. We repaint metric rows unconditionally here so a changed
  // value string (or a metric still sitting at the same pct) refreshes too.
  drawHeaderStatus();
  for (int col = 0; col < NUM_SERVICES; ++col) {
    drawAccent(col);
    drawHeaderText(col);
    drawPill(col, true);
    drawReset(col);
    drawPeak(col);
    drawMetricRow(col, 0);
    drawMetricRow(col, 1);
  }
}

void handleLine(char* line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;     // ignore non-JSON / debug lines
  JsonArrayConst arr = doc["services"].as<JsonArrayConst>();
  if (arr.isNull()) return;
  applySnapshot(arr);
}

void pollSerial() {
  while (DATA_SERIAL.available()) {
    char c = DATA_SERIAL.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (!rxSkip && rxLen > 0) { rxBuf[rxLen] = '\0'; handleLine(rxBuf); }
      rxLen = 0;
      rxSkip = false;
    } else if (rxSkip) {
      continue;
    } else if (rxLen < static_cast<int>(sizeof(rxBuf)) - 1) {
      rxBuf[rxLen++] = c;
    } else {
      rxSkip = true;  // overlong line: drop until newline
      rxLen = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void initServices() {
  for (int col = 0; col < NUM_SERVICES; ++col) {
    copyStr(services[col].name, "--", sizeof(services[col].name));
    copyStr(services[col].provider, "awaiting feed", sizeof(services[col].provider));
    copyStr(services[col].resetAt, "--", sizeof(services[col].resetAt));
    for (int row = 0; row < 2; ++row) {
      copyStr(services[col].m[row].label, row == 0 ? "Metric A" : "Metric B",
              sizeof(services[col].m[row].label));
      copyStr(services[col].m[row].value, "--", sizeof(services[col].m[row].value));
      services[col].m[row].pct = 0;
      anim[col][row] = {0, 0, 0};
    }
  }
}

void setup() {
  DATA_SERIAL.begin(115200);
  delay(200);
  DATA_SERIAL.println("TOKEN MONITOR BOOT");

  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);

  tft.init();
  tft.setRotation(3);

  rowSprite.setColorDepth(16);
  if (!rowSprite.createSprite(INNER_W, ROW_H)) {
    DATA_SERIAL.println("WARN: row sprite alloc failed");
  }

  peakSprite.setColorDepth(16);
  if (!peakSprite.createSprite(CARD_W - 16, 42)) {
    DATA_SERIAL.println("WARN: peak sprite alloc failed");
  }
#ifdef HAVE_FONT_HERO
  peakSprite.loadFont(NotoSansBold36);  // loaded once, kept for the sprite's life
#else
  peakSprite.setTextFont(1);
  peakSprite.setTextSize(4);
#endif

  mascotSprite.setColorDepth(16);
  if (!mascotSprite.createSprite(LANE_W, LANE_H)) {
    DATA_SERIAL.println("WARN: mascot sprite alloc failed");
  }

  initServices();

  bootMs = millis();
  lastBlinkMs = bootMs;
  lastFooterMs = bootMs;

  drawBackground();
  drawHeaderChrome();
  drawFooterChrome();
  for (int col = 0; col < NUM_SERVICES; ++col) {
    drawCardFrame(col);
    drawCardContent(col);
  }
  drawHeaderStatus();
  drawFooter();

  DATA_SERIAL.println("TOKEN MONITOR READY");
}

void loop() {
  const unsigned long now = millis();

  pollSerial();
  updateAnimation();

  if (now - lastBlinkMs >= 600) {
    lastBlinkMs = now;
    blinkOn = !blinkOn;
    drawHeaderStatus();
    for (int col = 0; col < NUM_SERVICES; ++col) {
      if (hasData && targetPeak(col) >= 80) drawPill(col, blinkOn);
    }
  }

  if (now - lastFooterMs >= 250) {
    lastFooterMs = now;
    drawFooter();
  }

  if (now - lastMascotMs >= 70) {
    lastMascotMs = now;
    updateMascot();
  }
}
