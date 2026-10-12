#include <Arduino.h>
#include <ArduinoJson.h>

#include "display.h"
#include "pixelfont.h"
#include "sprites.h"

// "Pixel Retro" face after token-tv: framed cards over pixel scenes, 5x7 type, ten-cell gauges.
static LGFX tft;
static LGFX_Sprite canvas(&tft);

static const int W = 320;
static const int H = 240;
static const int MAX_ACCOUNTS = 2;
static const int MAX_WINDOWS = 4;
static const uint32_t LINK_TIMEOUT_MS = 3 * 60 * 1000;
static const uint32_t STALE_SECONDS = 15 * 60;

static uint16_t rgb(uint32_t hex) { return lgfx::color565(hex >> 16, (hex >> 8) & 0xFF, hex & 0xFF); }

static const uint32_t BACKGROUND = 0x0a0f2c;
static const uint32_t INK = 0x0a0d26;
static const uint32_t CREAM = 0xfff3d6;
static const uint32_t LABEL = 0xb9d2ff;
static const uint32_t SOFT = 0xa9bdf0;
static const uint32_t OLD_INK = 0xff9a9a;
static const uint32_t TRACK = 0x19235f;
static const uint32_t PANEL_RULE = 0x3a4fb0;
// (start, tip) per used-quota band: below 50, 50-79, 80-89, 90-100.
static const uint32_t BANDS[4][2] = {{0x2aa84a, 0x5ee06a}, {0x8fc93a, 0xd3e84a}, {0xff7a1f, 0xffab3d}, {0xd62b3c, 0xff5a5f}};

enum Provider { CLAUDE, CODEX, OTHER };

struct Window {
  char label[8];
  float used;  // < 0 means missing
  uint32_t resets;
};

struct Account {
  char name[16];
  char status[16];
  uint32_t age;
  Provider provider;
  int windowCount;
  int waiting;  // conversations waiting for the user
  int top, height;  // where the card was last drawn
  Window windows[MAX_WINDOWS];
};

static Account accounts[MAX_ACCOUNTS];
static int accountCount = 0;
static uint32_t hostEpoch = 0;
static uint32_t receivedAtMs = 0;
static bool haveData = false;
static bool dirty = true;
static bool capturing = false;
static String line;

// ---------- data ----------

static uint32_t nowEpoch() { return hostEpoch + (millis() - receivedAtMs) / 1000; }

static void handleLine(const String &text) {
  JsonDocument doc;
  if (deserializeJson(doc, text)) return;
  hostEpoch = doc["now"] | 0;
  receivedAtMs = millis();
  accountCount = 0;
  for (JsonObject a : doc["accounts"].as<JsonArray>()) {
    if (accountCount >= MAX_ACCOUNTS) break;
    Account &acc = accounts[accountCount++];
    strlcpy(acc.name, a["name"] | "?", sizeof(acc.name));
    strlcpy(acc.status, a["status"] | "error", sizeof(acc.status));
    acc.age = a["age"] | 0;
    acc.waiting = a["waiting"] | 0;
    acc.provider = strcasecmp(acc.name, "claude") == 0 ? CLAUDE : strcasecmp(acc.name, "codex") == 0 ? CODEX : OTHER;
    acc.windowCount = 0;
    for (JsonObject w : a["windows"].as<JsonArray>()) {
      if (acc.windowCount >= MAX_WINDOWS) break;
      Window &win = acc.windows[acc.windowCount++];
      strlcpy(win.label, w["label"] | "", sizeof(win.label));
      win.used = w["used"].isNull() ? -1 : w["used"].as<float>();
      win.resets = w["resets"] | 0;
    }
  }
  haveData = true;
  dirty = true;
}

static const Window *findWindow(const Account &acc, const char *label) {
  for (int i = 0; i < acc.windowCount; i++)
    if (strcmp(acc.windows[i].label, label) == 0) return &acc.windows[i];
  return nullptr;
}

static float remaining(const Window *w) {
  if (!w || w->used < 0) return -1;
  return constrain(100.0f - w->used, 0.0f, 100.0f);
}

static void timeLeft(const Window *w, char *out, size_t size) {
  if (!w || !w->resets || !hostEpoch) {
    strlcpy(out, "--", size);
    return;
  }
  uint32_t now = nowEpoch();
  uint32_t m = w->resets > now ? (w->resets - now + 59) / 60 : 0;
  if (m >= 1440) snprintf(out, size, "%luD %luH", (unsigned long)(m / 1440), (unsigned long)(m % 1440 / 60));
  else snprintf(out, size, "%luH %luM", (unsigned long)(m / 60), (unsigned long)(m % 60));
}

// ---------- drawing primitives (into canvas; y is relative to the card) ----------

static int textWidth(const char *s, int scale) { return max(0, (int)strlen(s) * 6 - 1) * scale; }

// Leading empty columns of the first glyph (e.g. '1'), so left-aligned text lines up on ink.
static int leadingBlank(const char *s) {
  if (!s[0]) return 0;
  const uint8_t *rows = glyphRows(s[0]);
  uint8_t bits = 0;
  for (int yy = 0; yy < 7; yy++) bits |= rows[yy];
  int blank = 0;
  while (blank < 4 && !(bits & (16 >> blank))) blank++;
  return blank;
}

static void pixelText(int x, int y, const char *s, int scale, uint32_t color, int stroke = 0, bool right = false) {
  if (right) x -= textWidth(s, scale);
  else x -= leadingBlank(s) * scale;
  for (int pass = stroke ? 0 : 1; pass < 2; pass++) {
    uint16_t c = rgb(pass == 0 ? INK : color);
    int grow = pass == 0 ? stroke : 0;
    for (int i = 0; s[i]; i++) {
      const uint8_t *rows = glyphRows(s[i]);
      for (int yy = 0; yy < 7; yy++)
        for (int xx = 0; xx < 5; xx++)
          if (rows[yy] & (16 >> xx))
            canvas.fillRect(x + (i * 6 + xx) * scale - grow, y + yy * scale - grow, scale + 2 * grow, scale + 2 * grow, c);
    }
  }
}

static uint16_t mix(uint32_t a, uint32_t b, float t) {
  t = constrain(t, 0.0f, 1.0f);
  auto ch = [&](int shift) {
    int from = (a >> shift) & 0xFF, to = (b >> shift) & 0xFF;
    return (int)(from + (to - from) * t);
  };
  return lgfx::color565(ch(16), ch(8), ch(0));
}

static const uint32_t *band(float used) { return BANDS[used < 50 ? 0 : used < 80 ? 1 : used < 90 ? 2 : 3]; }

// Ten 10% cells filled by what is left; colour follows how much has been used.
static void gauge(int x0, int y0, int x1, int y1, float left, bool stale) {
  const int gap = 2, width = x1 - x0 + 1, height = y1 - y0 + 1;
  const float cell = (width - 9.0f * gap) / 10.0f;
  if (left < 0) {
    for (int i = 0; i < 10; i++) {
      int a = x0 + (int)(i * (cell + gap));
      for (int y = 0; y < height; y++)
        for (int x = 0; x < (int)cell; x++)
          if ((x + y) % 3 == 0 && (x == 0 || y == 0 || x == (int)cell - 1 || y == height - 1))
            canvas.drawPixel(a + x, y0 + y, rgb(0x3a4ea8));
    }
    return;
  }
  const uint32_t *colors = band(100 - left);
  float end = left >= 100 ? width : floorf(left / 10) * (cell + gap) + (left / 10 - floorf(left / 10)) * cell;
  for (int i = 0; i < 10; i++) {
    int a = x0 + (int)(i * (cell + gap));
    int cw = (int)(x0 + (i + 1) * (cell + gap) - gap) - a;
    canvas.fillRect(a, y0, cw, height, rgb(TRACK));
    float fraction = constrain(left / 10 - i, 0.0f, 1.0f);
    int fw = (int)roundf(cw * fraction);
    if (fw <= 0) continue;
    uint16_t c = stale ? rgb(0x6b7394) : mix(colors[0], colors[1], ((i + 0.5f) * (cell + gap)) / max(1.0f, end));
    canvas.fillRect(a, y0, fw, height, c);
    canvas.drawFastHLine(a, y0, fw, mix(stale ? 0x6b7394 : colors[1], 0xffffff, 0.45f));
    canvas.drawFastHLine(a, y1, fw, mix(stale ? 0x6b7394 : colors[0], 0x000000, 0.35f));
    if (stale)
      for (int k = -height; k < fw; k += 5) canvas.drawLine(a + k, y1, a + k + height, y0, rgb(TRACK));
  }
}

static void gaugePanel(int x0, int y0, int x1, int y1, float left, bool stale) {
  canvas.fillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, rgb(INK));
  canvas.drawRect(x0 + 2, y0 + 2, x1 - x0 - 3, y1 - y0 - 3, rgb(PANEL_RULE));
  gauge(x0 + 4, y0 + 4, x1 - 4, y1 - 4, left, stale);
}

static void sprite(const uint16_t *pixels, int x, int y) {
  for (int yy = 0; yy < 32; yy++)
    for (int xx = 0; xx < 32; xx++) {
      uint16_t p = pixels[yy * 32 + xx];
      if (p != SPRITE_KEY) canvas.drawPixel(x + xx, y + yy, p);
    }
}

// ---------- scenes ----------

static void px(int x, int y, uint32_t color, int w = 1, int h = 1) { canvas.fillRect(x * 2, y * 2, w * 2, h * 2, rgb(color)); }

static void disc(int cx, int cy, int r, uint32_t color) {
  for (int dy = -r; dy <= r; dy++) {
    int w = (int)roundf(sqrtf(r * r - dy * dy));
    px(cx - w, cy + dy, color, 2 * w + 1);
  }
}

static void scene(Provider p, int h) {
  static const uint32_t SKY[3][6] = {
      {0x2a1330, 0x3a1834, 0x4d1f36, 0x652836, 0x7f3335, 0x9a4133},
      {0x06171d, 0x082027, 0x0b2a2e, 0x0e3434, 0x113f39, 0x15493e},
      {0x0d0a28, 0x120d33, 0x170f3e, 0x1c1349, 0x211654, 0x271a5f}};
  const uint32_t *sky = SKY[p];
  int bandH = (h - 8 + 5) / 6;
  for (int i = 0; i < 6; i++) canvas.fillRect(6, 4 + i * bandH, W - 12, min(bandH, h - 8 - i * bandH), rgb(sky[i]));
  int bottom = h / 2 - 3;  // scene ground line, in 2px units
  randomSeed(p == CLAUDE ? 11 : p == CODEX ? 23 : 37);
  if (p == CLAUDE) {
    disc(100, 15, 5, 0xff9e4f);
    disc(100, 15, 3, 0xffc067);
    for (int yy : {17, 19}) px(94, yy, sky[2], 13);
    for (int x = 4; x < 156; x++) {
      int top = bottom - 8 + (int)roundf(5 * max(0.0f, sinf((x - 30) / 11.0f)) + 2 * sinf(x / 3.0f));
      px(x, top, 0x5a2236, 1, bottom - top);
    }
  } else if (p == CODEX) {
    for (int i = 0; i < 18; i++) px(random(30, 156), random(3, 14), 0x9bffd2);
    disc(92, 9, 3, 0xd9ffe9);
    disc(93, 8, 2, sky[0]);
    for (int x = 50; x < 156;) {
      int w = random(3, 6), bh = random(6, 13);
      px(x, bottom - bh, 0x0b3530, w, bh);
      for (int wy = bottom - bh + 1; wy < bottom - 1; wy += 2)
        if (random(2)) px(x + 1, wy, 0x7dffb9);
      x += w + 1;
    }
  } else {
    for (int i = 0; i < 40; i++) px(random(3, 156), random(3, bottom), random(2) ? 0xefe6ff : 0xb9a6ff);
  }
  // Keep the strip under the gauge panel plain sky, so no scenery peeks out below it.
  canvas.fillRect(6, h - 10, W - 12, 6, rgb(sky[5]));
}

static uint32_t accentFor(int provider) {
  return provider == CLAUDE ? 0xD97757 : provider == CODEX ? 0x4049fe : 0xb388ff;
}

static void frame(uint32_t accent, int h) {
  canvas.drawRect(0, 0, W, h, rgb(INK));
  canvas.drawRect(1, 1, W - 2, h - 2, rgb(INK));
  canvas.drawRect(2, 2, W - 4, h - 4, rgb(accent));
  canvas.drawRect(3, 3, W - 6, h - 6, rgb(accent));
  for (int cx : {0, W - 2})
    for (int cy : {0, h - 2}) canvas.fillRect(cx, cy, 2, 2, rgb(BACKGROUND));
}

// ---------- cards ----------

static void drawCard(const Account &acc, int top, int h) {
  if (canvas.width() != W || canvas.height() != h) {
    canvas.deleteSprite();
    canvas.setColorDepth(16);
    if (!canvas.createSprite(W, h)) return;
  }
  uint32_t accent = accentFor(acc.provider);
  bool stale = strcmp(acc.status, "ok") != 0 || acc.age > STALE_SECONDS;

  canvas.fillScreen(rgb(BACKGROUND));
  scene(acc.provider, h);
  frame(accent, h);

  canvas.fillRect(12, 10, 44, 44, rgb(INK));
  canvas.drawRect(12, 10, 44, 44, rgb(accent));
  canvas.drawRect(13, 11, 42, 42, rgb(accent));
  if (acc.provider == CLAUDE) sprite(SPRITE_CLAUDE, 18, 16);
  else if (acc.provider == CODEX) sprite(SPRITE_CHATGPT, 18, 16);

  char name[16];
  strlcpy(name, acc.name, sizeof(name));
  pixelText(66, 10, name, 2, CREAM, 1);

  // The weekly window is the headline; anything else (Claude's 5H) sits above its gauge.
  const Window *main = findWindow(acc, "WEEK");
  if (!main && acc.windowCount) main = &acc.windows[0];
  float left = remaining(main);
  char number[8];
  if (left < 0) strcpy(number, "--");
  else snprintf(number, sizeof(number), "%d%%", (int)floorf(left));
  pixelText(66, 31, number, 3, CREAM, 2);
  if (stale) pixelText(66 + textWidth(number, 3) - leadingBlank(number) * 3 + 8, 38, "OLD", 2, OLD_INK, 1);

  canvas.fillRect(222, 10, 86, 42, rgb(INK));
  canvas.drawRect(222, 10, 86, 42, rgb(accent));
  canvas.drawRect(223, 11, 84, 40, rgb(accent));
  char when[16], caption[16];
  timeLeft(main, when, sizeof(when));
  snprintf(caption, sizeof(caption), "%s RESET", main && strcmp(main->label, "WEEK") ? main->label : "WK");
  pixelText(230, 18, caption, 1, SOFT);
  pixelText(230, 31, when, 2, CREAM);
  if (left < 0 && strcmp(acc.status, "ok") != 0) pixelText(230, 31, "NO DATA", 2, OLD_INK);

  const Window *session = findWindow(acc, "5H");
  if (session && session != main) {
    canvas.fillRect(10, h - 64, W - 20, 26, rgb(INK));
    canvas.drawRect(12, h - 62, W - 24, 22, rgb(PANEL_RULE));
    pixelText(18, h - 58, "5H", 2, LABEL);
    float sLeft = remaining(session);
    char pct[8], sWhen[16];
    if (sLeft < 0) strcpy(pct, "--");
    else snprintf(pct, sizeof(pct), "%d%%", (int)floorf(sLeft));
    timeLeft(session, sWhen, sizeof(sWhen));
    gauge(48, h - 56, 196, h - 47, sLeft, stale);
    pixelText(250, h - 58, pct, 2, CREAM, 0, true);
    pixelText(W - 18, h - 55, sWhen, 1, SOFT, 0, true);
  }

  gaugePanel(10, h - 34, W - 11, h - 10, left, stale);

  if (acc.waiting > 0) {
    char count[4];
    if (acc.waiting == 1) strcpy(count, "!");
    else snprintf(count, sizeof(count), "%d", min(acc.waiting, 9));
    canvas.fillCircle(54, 14, 9, rgb(INK));
    canvas.fillCircle(54, 14, 7, rgb(0xff5a5f));
    pixelText(54 - textWidth(count, 1) / 2 + leadingBlank(count), 11, count, 1, CREAM);
  }
  canvas.pushSprite(0, top);
  if (capturing) {
    Serial.printf("CARD %d %d\n", top, h);
    Serial.write((const uint8_t *)canvas.getBuffer(), W * h * 2);
  }
}

// Night sky for rows [y0, y1); the stars are seeded so any strip can be redrawn to match the rest.
static void drawBackground(int y0 = 0, int y1 = H) {
  if (y1 <= y0) return;
  tft.fillRect(0, y0, W, y1 - y0, rgb(BACKGROUND));
  randomSeed(5);
  for (int i = 0; i < 90; i++) {
    uint32_t c = random(3) == 0 ? 0xffe9a8 : random(2) ? 0xffffff : 0xb9c8ff;
    int x = random(W), y = random(H);
    if (y >= y0 && y < y1) tft.drawPixel(x, y, rgb(c));
  }
}

static void drawWaiting(const char *message) {
  drawBackground();
  if (canvas.width() != W || canvas.height() != 60) {
    canvas.deleteSprite();
    canvas.setColorDepth(16);
    canvas.createSprite(W, 60);
  }
  canvas.fillScreen(rgb(BACKGROUND));
  frame(0xD97757, 60);
  pixelText(W / 2 + textWidth("USAGE SCREEN", 2) / 2, 14, "USAGE SCREEN", 2, CREAM, 1, true);
  pixelText(W / 2 + textWidth(message, 1) / 2, 36, message, 1, SOFT, 0, true);
  canvas.pushSprite(0, H / 2 - 30);
}

static void render() {
  if (!haveData) {
    drawWaiting("WAITING FOR THE MAC BRIDGE");
    return;
  }
  if (millis() - receivedAtMs > LINK_TIMEOUT_MS)
    for (int i = 0; i < accountCount; i++) accounts[i].age = STALE_SECONDS + 1;

  // Claude carries the extra 5H row, so it gets the taller card.
  // The gaps are redrawn every time, so a card that changed size leaves nothing behind.
  int top = 4;
  drawBackground(0, top);
  for (int i = 0; i < accountCount; i++) {
    bool tall = findWindow(accounts[i], "5H") && findWindow(accounts[i], "WEEK");
    int h = accountCount == 1 ? H - 8 : tall ? 136 : (accountCount == 2 && i == 1 ? H - 4 - top : 92);
    accounts[i].top = top;
    accounts[i].height = h;
    drawCard(accounts[i], top, h);
    top += h + 4;
    drawBackground(top - 4, min(top, H));
  }
  drawBackground(top, H);
}

// ---------- status LED ----------

// Active-low RGB LED on LEDC channels 0-2 (the backlight owns channel 7).
static const int LED_PINS[3] = {4, 16, 17};

// The LED is far brighter than its duty suggests, so peak duties (12-bit, per provider) are tiny and tuned by
// eye. Blue must stay 0 for Claude's orange, so colours are set directly rather than derived from the accent.
static const int LED_PEAK[3][3] = {{756, 625, 0}, {0, 0, 150}, {300, 0, 100}};  // CLAUDE, CODEX, OTHER

static void ledSet(int r, int g, int b) {
  const int v[3] = {r, g, b};
  for (int c = 0; c < 3; c++) ledcWrite(c, 4095 - v[c]);
}

static void ledInit() {
  for (int c = 0; c < 3; c++) {  // active-low: hold off before PWM takes over
    pinMode(LED_PINS[c], OUTPUT);
    digitalWrite(LED_PINS[c], HIGH);
  }
  for (int c = 0; c < 3; c++) {
    ledcSetup(c, 5000, 12);
    ledcAttachPin(LED_PINS[c], c);
  }
  ledSet(0, 0, 0);
}

// Breathes in the colour of a waiting account; with several waiting, the colour crossfades between them.
static void ledUpdate() {
  int waiting[MAX_ACCOUNTS], n = 0;
  for (int i = 0; i < accountCount; i++)
    if (accounts[i].waiting > 0) waiting[n++] = i;
  if (n == 0) { ledSet(0, 0, 0); return; }
  const int *a = LED_PEAK[accounts[waiting[0]].provider];
  const int *b = LED_PEAK[accounts[waiting[n - 1]].provider];
  float mixT = (millis() % 4000) / 4000.0f;
  float w = n > 1 ? 0.5f - 0.5f * cosf(mixT * 2 * PI) : 0;  // 0 = first account, 1 = last
  float t = (millis() % 2000) / 2000.0f;
  float level = 0.6f - 0.4f * cosf(t * 2 * PI);  // 20-100% of the peak
  ledSet((a[0] + (b[0] - a[0]) * w) * level, (a[1] + (b[1] - a[1]) * w) * level,
         (a[2] + (b[2] - a[2]) * w) * level);
}

// ---------- waiting animation ----------

// A point `d` pixels clockwise along the card's inner accent ring (2px wide, inset 2px).
static void ringPoint(const Account &acc, int d, int &x, int &y) {
  const int w = W - 6, h = acc.height - 6;
  d = ((d % (2 * (w + h))) + 2 * (w + h)) % (2 * (w + h));
  if (d < w) { x = 2 + d; y = 2; }
  else if (d < w + h) { x = W - 4; y = 2 + d - w; }
  else if (d < 2 * w + h) { x = W - 4 - (d - w - h); y = acc.height - 4; }
  else { x = 2; y = acc.height - 4 - (d - 2 * w - h); }
  y += acc.top;
}

// A short comet of light circles the frame: subtle, but visible from across the desk.
static void animateWaiting() {
  static uint32_t lastFrame = 0;
  static int lastHead[MAX_ACCOUNTS] = {-1, -1};
  const int TAIL = 56;
  if (millis() - lastFrame < 33) return;
  lastFrame = millis();
  for (int i = 0; i < accountCount; i++) {
    const Account &acc = accounts[i];
    if (acc.waiting <= 0 || acc.height == 0) {
      lastHead[i] = -1;
      continue;
    }
    uint32_t accent = accentFor(acc.provider);
    const int perimeter = 2 * (W - 6 + acc.height - 6);
    int head = (int)((millis() % 4000) * perimeter / 4000);
    int x, y;
    // Restore the ring the comet just left, then draw it at its new place, brightest at the head.
    if (lastHead[i] >= 0)
      for (int d = lastHead[i] - TAIL; d <= lastHead[i]; d += 2) {
        ringPoint(acc, d, x, y);
        tft.fillRect(x, y, 2, 2, rgb(accent));
      }
    for (int d = head - TAIL; d <= head; d += 2) {
      ringPoint(acc, d, x, y);
      tft.fillRect(x, y, 2, 2, mix(accent, 0xffffff, (float)(d - (head - TAIL)) / TAIL));
    }
    lastHead[i] = head;
  }
}

static void handleTap(int32_t x, int32_t y) {
  Serial.printf("TOUCH %ld %ld\n", (long)x, (long)y);
  for (int i = 0; i < accountCount; i++) {
    const Account &acc = accounts[i];
    if (y >= acc.top && y < acc.top + acc.height) {
      Serial.printf("OPEN %s\n", acc.name);
      return;
    }
  }
}

// The panel's read line isn't wired, so screenshots stream each card's sprite as it is redrawn.
static void screenshot() {
  Serial.printf("SHOT %d %d\n", W, H);
  capturing = true;
  render();
  capturing = false;
  Serial.println("END");
}

void setup() {
  ledInit();
  Serial.begin(115200);
  Serial.setRxBufferSize(2048);
  tft.init();
  tft.setRotation(1);
  tft.setBrightness(255);
  line.reserve(1024);
  render();
  Serial.println("{\"hello\":\"usage-screen\"}");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (line == "SHOT") screenshot();
      else handleLine(line);
      line = "";
    } else if (line.length() < 2000) {
      line += c;
    }
  }

  static bool wasTouched = false;
  int32_t tx, ty;
  bool touched = tft.getTouch(&tx, &ty);
  if (touched && !wasTouched) handleTap(tx, ty);
  wasTouched = touched;

  // Redraw on new data, and once a minute so countdowns tick.
  static uint32_t lastDraw = 0;
  if (dirty || millis() - lastDraw > 60000) {
    dirty = false;
    lastDraw = millis();
    render();
  }
  animateWaiting();
  ledUpdate();
  delay(10);
}
