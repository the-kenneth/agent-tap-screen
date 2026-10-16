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

// More saturated than the brand #D97757 so it holds up on the dim panel.
static const uint32_t CLAUDE_ORANGE = 0xF5622D;
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
  int working;  // conversations with a turn in progress
  int top, height;  // where the card was last drawn
  int horizon;      // card row where the topmost panel starts; the scene's ground begins here
  int textRight;    // right edge of the name and percentage labels
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
static uint32_t shownEpoch = 0;  // the clock as of the last full redraw; partial redraws reuse it

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
    acc.working = a["working"] | 0;
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
  uint32_t now = shownEpoch;
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

static void pixelText(int x, int y, const char *s, int scale, uint32_t color, int stroke = 0, bool right = false,
                      lgfx::LovyanGFX *target = &canvas) {
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
            target->fillRect(x + (i * 6 + xx) * scale - grow, y + yy * scale - grow, scale + 2 * grow, scale + 2 * grow, c);
    }
  }
}

static uint32_t blend(uint32_t a, uint32_t b, float t) {
  t = constrain(t, 0.0f, 1.0f);
  uint32_t out = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    int from = (a >> shift) & 0xFF, to = (b >> shift) & 0xFF;
    out |= (uint32_t)(from + (to - from) * t) << shift;
  }
  return out;
}

static uint16_t mix(uint32_t a, uint32_t b, float t) { return rgb(blend(a, b, t)); }

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

// ---------- scenes ----------

// Scenes draw into the card canvas, or straight to the panel or a small patch when only part of one animates.
static lgfx::LovyanGFX *sceneTarget = &canvas;
static int sceneX = 0, sceneY = 0;  // card position of the target's top-left corner
static LGFX_Sprite patch(&tft);

static const uint32_t SKY[3][6] = {
    {0x2a1330, 0x3a1834, 0x4d1f36, 0x652836, 0x7f3335, 0x9a4133},
    {0x06171d, 0x082027, 0x0b2a2e, 0x0e3434, 0x113f39, 0x15493e},
    {0x0d0a28, 0x120d33, 0x170f3e, 0x1c1349, 0x211654, 0x271a5f}};
static const uint32_t GROUND[3] = {0x2c0f27, 0x04121a, 0x0a0820};

// Scene positions and sizes are in 2px units, so everything sits on the same chunky grid.
static const int SUN_X = 99, SUN_R = 7;
static const int MOON_X = 100, MOON_Y = 15, MOON_R = 6;
static const uint32_t STRIPE_MS = 5000;

// Cirrus wisps as (dx, dy, length) streaks in scene units, at rest; the last two units at each end are fainter.
struct Streak { int8_t dx, dy, len; };
struct Cloud {
  Streak streaks[3];
  int width;        // widest extent at rest, for wrapping
  float level;      // height in the sky, as a share from the top
  uint32_t stepMs;  // time to drift one unit
};
static const Cloud CLOUDS[2] = {{{{9, 0, 12}, {0, 1, 30}, {17, 2, 11}}, 30, 0.3f, 1200},
                                {{{5, 0, 15}, {0, 1, 9}, {12, 1, 8}}, 20, 0.62f, 1900}};
static const int CLOUD_ROWS = 3;
static const int CLOUD_SURGE = 8;  // furthest a streak's edge strays from its resting place
static const uint32_t CLOUD_TINT[3] = {0xffb08c, 0xbfe8dc, 0xd8ccff};
static const float CLOUD_ALPHA[3] = {0.35f, 0.25f, 0.25f};

static void px(int x, int y, uint32_t color, int w = 1, int h = 1) {
  sceneTarget->fillRect(x * 2 - sceneX, y * 2 - sceneY, w * 2, h * 2, rgb(color));
}

// Half-width of row dy of a pixel circle; the extra half unit stops one-pixel nubs at the top and bottom.
static int span(int r, int dy) { return (int)sqrtf(max(0.0f, (r + 0.5f) * (r + 0.5f) - dy * dy)); }

// Integer hash, so stars keep their places and rhythms across redraws.
static uint32_t noise(uint32_t n) {
  n ^= n >> 16;
  n *= 0x7feb352d;
  n ^= n >> 15;
  n *= 0x846ca68b;
  return n ^ (n >> 16);
}

// The six sky bands spread over what shows above the panels; below the horizon is ground.
static uint32_t skyAt(const Account &acc, int y) {
  int horizon = acc.horizon / 2;
  if (y >= horizon) return GROUND[acc.provider];
  return SKY[acc.provider][constrain((y - 2) * 6 / max(1, horizon - 2), 0, 5)];
}

static int sunY(const Account &acc) { return acc.horizon / 2 - 12; }

static int cloudY(const Account &acc, int k) { return 2 + (int)(CLOUDS[k].level * (acc.horizon / 2 - 2)); }

// Where an edge is relative to the cloud's steady drift. Two slow rhythms of its own make it surge and stall, but
// together they never outpace the drift, so the edge only ever moves forward.
static int surge(uint32_t seed, uint32_t t, uint32_t stepMs, float most) {
  uint32_t n = noise(seed), m = noise(n);
  uint32_t slow = 40000 + n % 40000, quick = 9000 + m % 9000;
  float a = 0.6f * slow / (2 * PI * stepMs), b = 0.3f * quick / (2 * PI * stepMs);
  float scale = min(1.0f, most / (a + b));
  float f = a * sinf(2 * PI * ((t + (n >> 8)) % slow) / slow) + b * sinf(2 * PI * ((t + (m >> 8)) % quick) / quick);
  return (int)floorf((float)(t % stepMs) / stepMs + scale * f);
}

// One cloud at one moment: each streak's row and [x0, x1) span.
struct Wisp { int y[3], x0[3], x1[3]; };

// The cloud drifts left to right and starts again from the left once it has fully left the card. Each streak's
// front and back move on their own rhythms, so streaks stretch, shrink and slide past each other as it goes.
static Wisp wispAt(const Account &acc, int k, uint32_t t) {
  const Cloud &c = CLOUDS[k];
  int track = W / 2 - 4 + c.width + 2 * CLOUD_SURGE;
  int base = 2 - c.width - CLOUD_SURGE + (int)((t / c.stepMs + acc.provider * 57 + k * 83) % track);
  Wisp w;
  for (int j = 0; j < 3; j++) {
    const Streak &s = c.streaks[j];
    uint32_t seed = ((acc.provider * 2 + k) * 3 + j) * 2;
    float most = min(CLOUD_SURGE - 1.0f, s.len / 4.0f);  // a streak keeps at least half its length
    w.y[j] = cloudY(acc, k) + s.dy;
    w.x0[j] = base + s.dx + surge(seed, t, c.stepMs, most);
    w.x1[j] = base + s.dx + s.len + surge(seed + 1, t, c.stepMs, most);
  }
  return w;
}

static bool covered(const Wisp *wisps, int x, int y) {
  for (int k = 0; k < 2; k++)
    for (int j = 0; j < 3; j++)
      if (y == wisps[k].y[j] && x >= wisps[k].x0[j] && x < wisps[k].x1[j]) return true;
  return false;
}

// Whether a px rect is open sky: above the panels, clear of the header's boxes and labels, away from the sun or moon.
static bool openSky(const Account &acc, int x, int y, int w, int h) {
  if (x < 6 || x + w > W - 6 || y < 6 || y + h > acc.horizon - 4) return false;
  if (y < 58 && (x < acc.textRight || x + w > 218)) return false;
  if (acc.provider == OTHER) return true;
  bool claude = acc.provider == CLAUDE;
  int dx = (x + w / 2) / 2 - (claude ? SUN_X : MOON_X), dy = (y + h / 2) / 2 - (claude ? sunY(acc) : MOON_Y);
  int clear = (claude ? SUN_R : MOON_R) + 4;
  return dx * dx + dy * dy > clear * clear;
}

struct StarField {
  int count;
  uint32_t colors[2];
  float dim, bright;  // twinkle range, as a blend from sky to star colour
  float reach;        // share of the sky, from the top, the stars keep to
};
static const StarField STARS[3] = {{16, {0xffe2c4, 0xffc9a0}, 0.15f, 0.6f, 0.45f},
                                   {28, {0x9bffd2, 0xe8fff4}, 0.3f, 1.0f, 1.0f},
                                   {40, {0xefe6ff, 0xb9a6ff}, 0.3f, 1.0f, 1.0f}};

// Each star slowly brightens and fades on its own rhythm; the first two are sparkles whose arms come and go.
// A star behind a cloud is left out, so the live twinkle never paints over the cloud.
static void stars(const Account &acc, uint32_t t, const Wisp *wisps) {
  const StarField &f = STARS[acc.provider];
  int rows = max(1, (int)(f.reach * (acc.horizon / 2 - 3)));
  for (int i = 0; i < f.count; i++) {
    uint32_t n = noise(acc.provider * 131 + i), m = noise(n);
    int x = 3 + n % 154, y = 3 + (n >> 8) % rows, arm = i < 2;
    if (!openSky(acc, (x - arm) * 2, (y - arm) * 2, (2 * arm + 1) * 2, (2 * arm + 1) * 2)) continue;
    if (covered(wisps, x, y) || (arm && (covered(wisps, x - 1, y) || covered(wisps, x + 1, y) ||
                                         covered(wisps, x, y - 1) || covered(wisps, x, y + 1))))
      continue;
    uint32_t period = 3000 + m % 4000;
    float level = 0.5f - 0.5f * cosf(2 * PI * ((t % period + (m >> 12) % period) % period) / period);
    uint32_t c = f.colors[(m >> 24) & 1];
    if (arm) {
      px(x, y, blend(skyAt(acc, y), c, f.bright));
      for (int d : {-1, 1}) {
        px(x + d, y, blend(skyAt(acc, y), c, 0.6f * f.bright * level));
        px(x, y + d, blend(skyAt(acc, y + d), c, 0.6f * f.bright * level));
      }
    } else {
      px(x, y, blend(skyAt(acc, y), c, f.dim + (f.bright - f.dim) * level));
    }
  }
}

// A banded sunset sun with a soft glow; its stripes drift slowly down.
static void sun(const Account &acc, uint32_t t) {
  const int cy = sunY(acc);
  for (int dy = -SUN_R - 2; dy <= SUN_R + 2; dy++) {
    int w = span(SUN_R + 2, dy);
    px(SUN_X - w, cy + dy, blend(skyAt(acc, cy + dy), 0xff8a4f, 0.12f), 2 * w + 1);
  }
  int shift = t / STRIPE_MS % 3;
  for (int dy = -SUN_R; dy <= SUN_R; dy++) {
    if (dy > 0 && (dy + 3 - shift) % 3 == 0) continue;
    int w = span(SUN_R, dy);
    px(SUN_X - w, cy + dy, blend(0xffd36e, 0xff6f4a, (dy + SUN_R) / (2.0f * SUN_R)), 2 * w + 1);
  }
}

// Two ridges of low hills on the horizon; the near one runs into the ground under the panels.
static void hills(const Account &acc) {
  int horizon = acc.horizon / 2;
  for (int x = 2; x < W / 2 - 2; x++) {
    int far = horizon - 5 + (int)roundf(1.6f * sinf(x / 9.0f + 1) + 0.9f * sinf(x / 4.7f + 2));
    int near = horizon - 2 + (int)roundf(1.2f * sinf(x / 13.0f + 4));
    px(x, far, 0x55203a, 1, horizon - far);
    px(x, near, GROUND[CLAUDE], 1, horizon - near);
  }
}

// A crescent with a faint glow; the shadowed side is just visible against the sky.
static void moon(const Account &acc) {
  const uint32_t LIT = 0xd9ffe9, SHADE = 0x9fdcc4;
  for (int dy = -MOON_R - 2; dy <= MOON_R + 2; dy++) {
    int w = span(MOON_R + 2, dy);
    px(MOON_X - w, MOON_Y + dy, blend(skyAt(acc, MOON_Y + dy), LIT, 0.04f), 2 * w + 1);
  }
  for (int dy = -MOON_R; dy <= MOON_R; dy++) {
    int w = span(MOON_R, dy);
    for (int dx = -w; dx <= w; dx++) {
      float bite = (dx - 4) * (dx - 4) + (dy + 2) * (dy + 2);
      uint32_t shadow = blend(skyAt(acc, MOON_Y + dy), LIT, 0.08f);
      px(MOON_X + dx, MOON_Y + dy, bite <= 5.5f * 5.5f ? shadow : bite <= 6.5f * 6.5f ? SHADE : LIT);
    }
  }
}

// Thin cirrus blended over whatever is already drawn, so it reads back the target (always a sprite here).
static void clouds(const Account &acc, const Wisp *wisps) {
  for (int k = 0; k < 2; k++)
    for (int j = 0; j < 3; j++) {
      const Wisp &w = wisps[k];
      for (int x = max(2, w.x0[j]); x < min(W / 2 - 2, w.x1[j]); x++) {
        auto under = sceneTarget->readPixelRGB(x * 2 - sceneX, w.y[j] * 2 - sceneY);
        float alpha = CLOUD_ALPHA[acc.provider] * (x < w.x0[j] + 2 || x >= w.x1[j] - 2 ? 0.5f : 1.0f);
        px(x, w.y[j], blend(under.r << 16 | under.g << 8 | under.b, CLOUD_TINT[acc.provider], alpha));
      }
    }
}

static void scene(const Account &acc, uint32_t t) {
  const Wisp wisps[2] = {wispAt(acc, 0, t), wispAt(acc, 1, t)};
  for (int y = 2; y < acc.height / 2 - 2; y++) px(2, y, skyAt(acc, y), W / 2 - 4);
  stars(acc, t, wisps);
  if (acc.provider == CLAUDE) {
    sun(acc, t);
    clouds(acc, wisps);
    hills(acc);
  } else {
    if (acc.provider == CODEX) moon(acc);
    clouds(acc, wisps);
  }
}

static uint32_t accentFor(int provider) {
  return provider == CLAUDE ? CLAUDE_ORANGE : provider == CODEX ? 0x4049fe : 0xb388ff;
}

static void frame(uint32_t accent, int h) {
  canvas.drawRect(0, 0, W, h, rgb(INK));
  canvas.drawRect(1, 1, W - 2, h - 2, rgb(INK));
  canvas.drawRect(2, 2, W - 4, h - 4, rgb(accent));
  canvas.drawRect(3, 3, W - 6, h - 6, rgb(accent));
  for (int cx : {0, W - 2})
    for (int cy : {0, h - 2}) canvas.fillRect(cx, cy, 2, 2, rgb(BACKGROUND));
}

// ---------- logo ----------

// Inside of the logo box. While a turn is in progress the logo animates: Clawd types on his laptop as he does in
// the Claude desktop app, and the ChatGPT knot turns.
static const int LOGO_X = 14, LOGO_Y = 12, LOGO_SIZE = 40;
static const uint32_t KNOT_STEP_MS = 83;
static LGFX_Sprite logoPatch(&tft);

// -1 is the resting logo.
static int logoFrame(const Account &acc, uint32_t t) {
  if (acc.working <= 0) return -1;
  if (acc.provider == CLAUDE) return t / (1000 / CLAWD_FPS) % CLAWD_FRAMES;
  if (acc.provider == CODEX) return t / KNOT_STEP_MS % KNOT_FRAMES;
  return -1;
}

static void drawLogo(lgfx::LovyanGFX *target, int x, int y, const Account &acc, int frame) {
  target->fillRect(x, y, LOGO_SIZE, LOGO_SIZE, rgb(INK));
  if (acc.provider == CLAUDE) {
    const char *pixels = frame < 0 ? CLAWD_REST : CLAWD_TYPING[frame];
    x += (LOGO_SIZE - CLAWD_W) / 2;
    y += (LOGO_SIZE - CLAWD_H) / 2;
    for (int i = 0; i < CLAWD_W * CLAWD_H; i++)
      if (pixels[i] != '.') target->drawPixel(x + i % CLAWD_W, y + i / CLAWD_W, rgb(CLAWD_PALETTE[pixels[i] - '0']));
  } else if (acc.provider == CODEX) {
    x += (LOGO_SIZE - KNOT) / 2;
    y += (LOGO_SIZE - KNOT) / 2;
    for (int i = 0; i < KNOT * KNOT; i++) {
      int xx = x + i % KNOT, yy = y + i / KNOT;
      if (frame < 0) {
        if (SPRITE_CHATGPT[i] != SPRITE_KEY) target->drawPixel(xx, yy, SPRITE_CHATGPT[i]);
        continue;
      }
      char c = KNOT_TURN[frame][i];
      if (c != '.') target->drawPixel(xx, yy, mix(INK, CHATGPT_INK, (c <= '9' ? c - '0' : c - 'A' + 10) / 15.0f));
    }
  }
}

// Red badge on the logo box's corner counting waiting conversations; (x, y) is the target's place in the card.
static void badge(lgfx::LovyanGFX *target, int x, int y, int waiting) {
  char count[4];
  if (waiting == 1) strcpy(count, "!");
  else snprintf(count, sizeof(count), "%d", min(waiting, 9));
  target->fillCircle(54 - x, 14 - y, 9, rgb(INK));
  target->fillCircle(54 - x, 14 - y, 7, rgb(0xff5a5f));
  pixelText(54 - x - textWidth(count, 1) / 2 + leadingBlank(count), 11 - y, count, 1, CREAM, 0, false, target);
}

// ---------- cards ----------

// One canvas serves every card and only grows, so cards of different heights don't churn the heap.
static bool canvasFor(int h) {
  if (canvas.width() == W && canvas.height() >= h) return true;
  canvas.deleteSprite();
  canvas.setColorDepth(16);
  return canvas.createSprite(W, h);
}

static void pushCanvas(int x, int y, int w, int h, int top) {
  tft.setClipRect(x, top + y, w, h);
  canvas.pushSprite(0, top);
  tft.clearClipRect();
}

// Rows y0..y1 given: only those rows inside the frame are drawn and sent, for a scene change behind the labels.
static void drawCard(Account &acc, int top, int h, uint32_t t, int y0 = 0, int y1 = 0) {
  if (!canvasFor(h)) return;
  if (y1 > y0) canvas.setClipRect(4, y0, W - 8, y1 - y0);
  uint32_t accent = accentFor(acc.provider);
  bool stale = strcmp(acc.status, "ok") != 0 || acc.age > STALE_SECONDS;

  char name[16];
  strlcpy(name, acc.name, sizeof(name));
  // The weekly window is the headline; anything else (Claude's 5H) sits above its gauge.
  const Window *main = findWindow(acc, "WEEK");
  if (!main && acc.windowCount) main = &acc.windows[0];
  const Window *session = findWindow(acc, "5H");
  if (session == main) session = nullptr;
  float left = remaining(main);
  char number[8];
  if (left < 0) strcpy(number, "--");
  else snprintf(number, sizeof(number), "%d%%", (int)floorf(left));
  int numberRight = 66 - leadingBlank(number) * 3 + textWidth(number, 3);

  // The scene keeps its stars clear of the labels and sets its horizon where the panels begin.
  acc.horizon = session ? h - 64 : h - 34;
  acc.textRight = max(66 - leadingBlank(name) * 2 + textWidth(name, 2), numberRight + (stale ? 8 + textWidth("OLD", 2) : 0)) + 6;

  canvas.fillScreen(rgb(BACKGROUND));
  sceneTarget = &canvas;
  sceneX = sceneY = 0;
  scene(acc, t);
  frame(accent, h);

  canvas.drawRect(12, 10, 44, 44, rgb(accent));
  canvas.drawRect(13, 11, 42, 42, rgb(accent));
  drawLogo(&canvas, LOGO_X, LOGO_Y, acc, logoFrame(acc, t));

  pixelText(66, 10, name, 2, CREAM, 1);
  pixelText(66, 31, number, 3, CREAM, 2);
  if (stale) pixelText(numberRight + 8, 38, "OLD", 2, OLD_INK, 1);

  canvas.fillRect(222, 10, 86, 42, rgb(INK));
  canvas.drawRect(222, 10, 86, 42, rgb(accent));
  canvas.drawRect(223, 11, 84, 40, rgb(accent));
  char when[16], caption[16];
  timeLeft(main, when, sizeof(when));
  snprintf(caption, sizeof(caption), "%s RESET", main && strcmp(main->label, "WEEK") ? main->label : "WK");
  pixelText(230, 18, caption, 1, SOFT);
  pixelText(230, 31, when, 2, CREAM);
  if (left < 0 && strcmp(acc.status, "ok") != 0) pixelText(230, 31, "NO DATA", 2, OLD_INK);

  if (session) {
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

  if (acc.waiting > 0) badge(&canvas, 0, 0, acc.waiting);
  if (y1 > y0) {
    canvas.clearClipRect();
    pushCanvas(4, y0, W - 8, y1 - y0, top);
    return;
  }
  pushCanvas(0, 0, W, h, top);
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
  if (!canvasFor(60)) return;
  canvas.fillScreen(rgb(BACKGROUND));
  frame(CLAUDE_ORANGE, 60);
  pixelText(W / 2 + textWidth("USAGE SCREEN", 2) / 2, 14, "USAGE SCREEN", 2, CREAM, 1, true);
  pixelText(W / 2 + textWidth(message, 1) / 2, 36, message, 1, SOFT, 0, true);
  pushCanvas(0, 0, W, 60, H / 2 - 30);
}

static void render() {
  if (!haveData) {
    drawWaiting("WAITING FOR THE MAC BRIDGE");
    return;
  }
  if (millis() - receivedAtMs > LINK_TIMEOUT_MS)
    for (int i = 0; i < accountCount; i++) accounts[i].age = STALE_SECONDS + 1;
  shownEpoch = nowEpoch();

  // Claude carries the extra 5H row, so it gets the taller card.
  // The gaps are redrawn every time, so a card that changed size leaves nothing behind.
  int top = 4;
  drawBackground(0, top);
  for (int i = 0; i < accountCount; i++) {
    bool tall = findWindow(accounts[i], "5H") && findWindow(accounts[i], "WEEK");
    int h = accountCount == 1 ? H - 8 : tall ? 136 : (accountCount == 2 && i == 1 ? H - 4 - top : 92);
    accounts[i].top = top;
    accounts[i].height = h;
    drawCard(accounts[i], top, h, millis());
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
  float level = 0.55f - 0.45f * cosf(t * 2 * PI);  // 10-100% of the peak
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

// Only a frame change is drawn, into a patch over the inside of the logo box.
static void animateLogos() {
  static int shown[MAX_ACCOUNTS];
  uint32_t t = millis();
  for (int i = 0; i < accountCount; i++) {
    const Account &acc = accounts[i];
    int frame = acc.height ? logoFrame(acc, t) : -1;
    if (frame == shown[i]) continue;
    shown[i] = frame;
    if (frame < 0) continue;  // the redraw that stopped it drew the resting logo
    if (!logoPatch.width()) {
      logoPatch.setColorDepth(16);
      if (!logoPatch.createSprite(LOGO_SIZE, LOGO_SIZE)) return;
    }
    drawLogo(&logoPatch, 0, 0, acc, frame);
    if (acc.waiting > 0) badge(&logoPatch, LOGO_X, LOGO_Y, acc.waiting);
    logoPatch.pushSprite(LOGO_X, acc.top + LOGO_Y);
  }
}

// Stars are repainted in place and the sun's stripes via a small patch. Clouds pass behind the labels, so when a
// cloud's edge moves, its card is redrawn clipped to the cloud's rows and just those rows are sent.
static void animateScene() {
  static uint32_t lastFrame = 0, lastStripe = 0;
  static Wisp shown[MAX_ACCOUNTS][2];
  if (millis() - lastFrame < 80) return;
  uint32_t t = lastFrame = millis();
  bool stripe = t / STRIPE_MS != lastStripe;
  lastStripe = t / STRIPE_MS;
  tft.startWrite();
  for (int i = 0; i < accountCount; i++) {
    Account &acc = accounts[i];
    if (acc.height == 0) continue;
    const Wisp wisps[2] = {wispAt(acc, 0, t), wispAt(acc, 1, t)};
    for (int k = 0; k < 2; k++) {
      if (!memcmp(&wisps[k], &shown[i][k], sizeof(Wisp))) continue;
      shown[i][k] = wisps[k];
      int y = cloudY(acc, k) * 2;
      drawCard(acc, acc.top, acc.height, t, y, y + CLOUD_ROWS * 2);
    }
    sceneTarget = &tft;
    sceneX = 0;
    sceneY = -acc.top;
    stars(acc, t, wisps);
    if (acc.provider != CLAUDE || !stripe) continue;
    const int x0 = (SUN_X - SUN_R - 2) * 2, y0 = (sunY(acc) - SUN_R - 2) * 2, w = (2 * SUN_R + 5) * 2;
    if (patch.width() != w || patch.height() != acc.horizon - y0) {
      patch.deleteSprite();
      patch.setColorDepth(16);
      if (!patch.createSprite(w, acc.horizon - y0)) continue;
    }
    sceneTarget = &patch;
    sceneX = x0;
    sceneY = y0;
    scene(acc, t);
    patch.pushSprite(x0, acc.top + y0);
  }
  tft.endWrite();
  sceneTarget = &canvas;
  sceneX = sceneY = 0;
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
  animateScene();
  animateLogos();
  ledUpdate();
  delay(10);
}
