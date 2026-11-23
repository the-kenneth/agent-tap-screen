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
// The backdrop each card shows; the bridge picks one per card each day. Numbers are part of the serial protocol.
enum SceneId { SUNSET, TEAL_NIGHT, AURORA, SNOWFALL, MOONLIT_SEA, MISTY_DAWN, MOUNT_FUJI, MILKY_WAY, FIREFLIES, BLOSSOM,
               SCENE_COUNT };

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
  int scene;
  int windowCount;
  int waiting;  // conversations waiting for the user
  int working;  // conversations with a turn in progress
  uint32_t workEdgeMs;  // when working last switched between none and some
  int top, height;  // where the card was last drawn
  int horizon;      // card row where the topmost panel starts; the scene's ground begins here
  int textRight;    // right edge of the name and percentage labels
  Window windows[MAX_WINDOWS];
};

static Account accounts[MAX_ACCOUNTS];
static int accountCount = 0;
static int dailyScene[MAX_ACCOUNTS] = {-1, -1};   // the scene the bridge last sent for each card
static int pickedScene[MAX_ACCOUNTS] = {-1, -1};  // a double-tapped scene, until the bridge's pick changes
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
    int working = a["working"] | 0;
    if ((working > 0) != (acc.working > 0)) acc.workEdgeMs = receivedAtMs;
    acc.working = working;
    acc.provider = strcasecmp(acc.name, "claude") == 0 ? CLAUDE : strcasecmp(acc.name, "codex") == 0 ? CODEX : OTHER;
    int daily = constrain(a["scene"] | (acc.provider == CODEX ? TEAL_NIGHT : SUNSET), 0, SCENE_COUNT - 1);
    int slot = accountCount - 1;
    if (dailyScene[slot] != daily) pickedScene[slot] = -1;  // a new daily pick replaces a tapped one
    dailyScene[slot] = daily;
    acc.scene = pickedScene[slot] >= 0 ? pickedScene[slot] : daily;
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

// Positions and sizes are in 2px units, so everything sits on the same chunky grid. Scenes draw into the card canvas;
// only star twinkles go straight to the panel.
static lgfx::LovyanGFX *sceneTarget = &canvas;
static int sceneX = 0, sceneY = 0;  // card position of the target's top-left corner

// Drifting wisps as (dx, dy, length) streaks in scene units, at rest; the last two units at each end are fainter.
struct Streak { int8_t dx, dy, len; };
struct Cloud {
  Streak streaks[3];
  int width;        // widest extent at rest, for wrapping
  float level;      // height in the sky, as a share from the top
  uint32_t stepMs;  // time to drift one unit
};
static const Cloud CIRRUS[2] = {{{{9, 0, 12}, {0, 1, 30}, {17, 2, 11}}, 30, 0.3f, 1200},
                                {{{5, 0, 15}, {0, 1, 9}, {12, 1, 8}}, 20, 0.62f, 1900}};
static const Cloud HAZE[2] = {{{{0, 0, 26}, {10, 1, 40}, {34, 2, 18}}, 52, 0.6f, 2600},
                              {{{8, 0, 20}, {0, 1, 34}, {18, 2, 26}}, 44, 0.82f, 3600}};
static const Cloud MIST[2] = {{{{0, 0, 22}, {8, 1, 34}, {30, 2, 14}}, 44, 0.8f, 2600},
                              {{{6, 0, 16}, {0, 1, 26}, {14, 2, 20}}, 34, 0.9f, 3400}};
static const int CLOUD_ROWS = 3;
static const int CLOUD_SURGE = 8;  // furthest a streak's edge strays from its resting place

// One cloud at one moment: each streak's row and [x0, x1) span.
struct Wisp { int y[3], x0[3], x1[3]; };

struct StarField {
  int count;
  uint32_t colors[2];
  float dim, bright;  // twinkle range, as a blend from sky to star colour
  float reach;        // share of the sky, from the top, the stars keep to
};

struct Scene {
  uint32_t sky[6];  // bands from the top down to the horizon
  uint32_t ground;
  StarField stars;
  const Cloud *clouds;  // two drifting wisps, or none
  uint32_t cloudTint;
  float cloudAlpha;
  int orbX, orbY, orbR;  // sun or moon the stars keep clear of; orbY <= 0 counts up from the horizon, orbR 0 is none
  uint32_t stepMs;       // tick of the scene's own animation; 0 when only stars and clouds move
  void (*draw)(const Account &acc, uint32_t t, const Wisp *wisps);  // everything over the sky
  void (*rows)(const Account &acc, uint32_t t, int &y0, int &y1);   // card rows (px) its animation touches at t
};
extern const Scene SCENES[SCENE_COUNT];

static void px(int x, int y, uint32_t color, int w = 1, int h = 1) {
  sceneTarget->fillRect(x * 2 - sceneX, y * 2 - sceneY, w * 2, h * 2, rgb(color));
}

// Blends over what is already drawn, so it reads the target back (always the card canvas).
static void glaze(int x, int y, uint32_t color, float alpha) {
  if (x < 2 || x >= W / 2 - 2 || y < 2 || alpha <= 0) return;
  auto under = sceneTarget->readPixelRGB(x * 2 - sceneX, y * 2 - sceneY);
  px(x, y, blend(under.r << 16 | under.g << 8 | under.b, color, alpha));
}

// Half-width of row dy of a pixel circle; the extra half unit stops one-pixel nubs at the top and bottom.
static int span(int r, int dy) { return (int)sqrtf(max(0.0f, (r + 0.5f) * (r + 0.5f) - dy * dy)); }

static void disc(int cx, int cy, int r, uint32_t color) {
  for (int dy = -r; dy <= r; dy++) {
    int w = span(r, dy);
    px(cx - w, cy + dy, color, 2 * w + 1);
  }
}

// Integer hash, so stars keep their places and rhythms across redraws.
static uint32_t noise(uint32_t n) {
  n ^= n >> 16;
  n *= 0x7feb352d;
  n ^= n >> 15;
  n *= 0x846ca68b;
  return n ^ (n >> 16);
}

static int orbRow(const Account &acc) {
  const Scene &s = SCENES[acc.scene];
  return s.orbY > 0 ? s.orbY : acc.horizon / 2 + s.orbY;
}

// The six sky bands spread over what shows above the panels; below the horizon is ground.
static uint32_t bandAt(const Account &acc, int y) {
  int horizon = acc.horizon / 2;
  if (y >= horizon) return SCENES[acc.scene].ground;
  return SCENES[acc.scene].sky[constrain((y - 2) * 6 / max(1, horizon - 2), 0, 5)];
}

static const uint32_t MILKY_TINT = 0x6f5fb0;

// Signed distance, in units, from the Milky Way's spine: a line slanting up to the right across the card.
static float milkyOffset(const Account &acc, float x, float y) {
  float rise = acc.horizon / 2 + 10.0f, run = 130;
  return ((x - 20) * rise + (y - acc.horizon / 2 - 4) * run) / sqrtf(run * run + rise * rise);
}

// The Milky Way's glow in a few flat steps, split by a darker dust lane.
static float milkyGlow(const Account &acc, int x, int y) {
  float d = milkyOffset(acc, x, y), g = max(0.0f, 1 - fabsf(d) / 8);
  if (fabsf(d - 0.8f) < 1.1f) g *= 0.55f;
  return roundf(g * g * 4) / 4 * 0.28f;
}

// Plain sky at a point, before anything is drawn over it.
static uint32_t skyAt(const Account &acc, int x, int y) {
  uint32_t c = bandAt(acc, y);
  return acc.scene == MILKY_WAY && y < acc.horizon / 2 ? blend(c, MILKY_TINT, milkyGlow(acc, x, y)) : c;
}

static int cloudY(const Account &acc, int k) {
  return 2 + (int)(SCENES[acc.scene].clouds[k].level * (acc.horizon / 2 - 2));
}

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

// The cloud drifts left to right and starts again from the left once it has fully left the card. Each streak's
// front and back move on their own rhythms, so streaks stretch, shrink and slide past each other as it goes.
static Wisp wispAt(const Account &acc, int k, uint32_t t) {
  Wisp w = {};
  if (!SCENES[acc.scene].clouds) return w;
  const Cloud &c = SCENES[acc.scene].clouds[k];
  int track = W / 2 - 4 + c.width + 2 * CLOUD_SURGE;
  int base = 2 - c.width - CLOUD_SURGE + (int)((t / c.stepMs + acc.scene * 57 + k * 83) % track);
  for (int j = 0; j < 3; j++) {
    const Streak &s = c.streaks[j];
    uint32_t seed = ((acc.scene * 2 + k) * 3 + j) * 2;
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
  const Scene &s = SCENES[acc.scene];
  if (!s.orbR) return true;
  int dx = (x + w / 2) / 2 - s.orbX, dy = (y + h / 2) / 2 - orbRow(acc), clear = s.orbR + 4;
  return dx * dx + dy * dy > clear * clear;
}

// Each star slowly brightens and fades on its own rhythm; the first two are sparkles whose arms come and go.
// A star behind a cloud is left out, so the live twinkle never paints over the cloud, and so is one in rows
// [skip0, skip1), which are redrawn whole anyway.
static void stars(const Account &acc, uint32_t t, const Wisp *wisps, int skip0 = 0, int skip1 = 0) {
  const StarField &f = SCENES[acc.scene].stars;
  int rows = max(1, (int)(f.reach * (acc.horizon / 2 - 3)));
  for (int i = 0; i < f.count; i++) {
    uint32_t n = noise(acc.scene * 131 + i), m = noise(n);
    int x = 3 + n % 154, y = 3 + (n >> 8) % rows, arm = i < 2;
    if (y + arm >= skip0 && y - arm < skip1) continue;
    if (!openSky(acc, (x - arm) * 2, (y - arm) * 2, (2 * arm + 1) * 2, (2 * arm + 1) * 2)) continue;
    if (covered(wisps, x, y) || (arm && (covered(wisps, x - 1, y) || covered(wisps, x + 1, y) ||
                                         covered(wisps, x, y - 1) || covered(wisps, x, y + 1))))
      continue;
    uint32_t period = 3000 + m % 4000;
    float level = 0.5f - 0.5f * cosf(2 * PI * ((t % period + (m >> 12) % period) % period) / period);
    uint32_t c = f.colors[(m >> 24) & 1];
    if (arm) {
      px(x, y, blend(skyAt(acc, x, y), c, f.bright));
      for (int d : {-1, 1}) {
        px(x + d, y, blend(skyAt(acc, x + d, y), c, 0.6f * f.bright * level));
        px(x, y + d, blend(skyAt(acc, x, y + d), c, 0.6f * f.bright * level));
      }
    } else {
      px(x, y, blend(skyAt(acc, x, y), c, f.dim + (f.bright - f.dim) * level));
    }
  }
}

// Thin wisps blended over whatever is already drawn.
static void clouds(const Account &acc, const Wisp *wisps) {
  const Scene &s = SCENES[acc.scene];
  for (int k = 0; k < 2; k++)
    for (int j = 0; j < 3; j++) {
      const Wisp &w = wisps[k];
      for (int x = max(2, w.x0[j]); x < min(W / 2 - 2, w.x1[j]); x++)
        glaze(x, w.y[j], s.cloudTint, s.cloudAlpha * (x < w.x0[j] + 2 || x >= w.x1[j] - 2 ? 0.5f : 1.0f));
    }
}

// ---------- scene pieces ----------

static const uint32_t STRIPE_MS = 5000;

// A banded sunset sun with a soft glow; its stripes drift slowly down.
static void sun(const Account &acc, uint32_t t) {
  const int cx = SCENES[acc.scene].orbX, cy = orbRow(acc), r = SCENES[acc.scene].orbR;
  for (int dy = -r - 2; dy <= r + 2; dy++) {
    int w = span(r + 2, dy);
    px(cx - w, cy + dy, blend(skyAt(acc, cx, cy + dy), 0xff8a4f, 0.12f), 2 * w + 1);
  }
  int shift = t / STRIPE_MS % 3;
  for (int dy = -r; dy <= r; dy++) {
    if (dy > 0 && (dy + 3 - shift) % 3 == 0) continue;
    int w = span(r, dy);
    px(cx - w, cy + dy, blend(0xffd36e, 0xff6f4a, (dy + r) / (2.0f * r)), 2 * w + 1);
  }
}

// A pale sun low in the haze.
static void paleSun(const Account &acc) {
  const int cx = SCENES[acc.scene].orbX, cy = orbRow(acc), r = SCENES[acc.scene].orbR;
  for (int ring = 2; ring >= 1; ring--)
    for (int dy = -r - ring; dy <= r + ring; dy++) {
      int w = span(r + ring, dy);
      px(cx - w, cy + dy, blend(skyAt(acc, cx, cy + dy), 0xffd9b0, 0.12f * (3 - ring)), 2 * w + 1);
    }
  for (int dy = -r; dy <= r; dy++) {
    int w = span(r, dy);
    px(cx - w, cy + dy, blend(0xfff2d8, 0xffc49a, (dy + r) / (2.0f * r)), 2 * w + 1);
  }
}

// A crescent with a faint glow; the shadowed side is just visible against the sky.
static void crescent(const Account &acc) {
  const uint32_t LIT = 0xd9ffe9, SHADE = 0x9fdcc4;
  const int cx = SCENES[acc.scene].orbX, cy = orbRow(acc), r = SCENES[acc.scene].orbR;
  for (int dy = -r - 2; dy <= r + 2; dy++) {
    int w = span(r + 2, dy);
    px(cx - w, cy + dy, blend(skyAt(acc, cx, cy + dy), LIT, 0.04f), 2 * w + 1);
  }
  for (int dy = -r; dy <= r; dy++) {
    int w = span(r, dy);
    for (int dx = -w; dx <= w; dx++) {
      float bite = (dx - 4) * (dx - 4) + (dy + 2) * (dy + 2);
      uint32_t shadow = blend(skyAt(acc, cx + dx, cy + dy), LIT, 0.08f);
      px(cx + dx, cy + dy, bite <= 5.5f * 5.5f ? shadow : bite <= 6.5f * 6.5f ? SHADE : LIT);
    }
  }
}

// A full moon with faint seas and a wide soft glow.
static void fullMoon(const Account &acc) {
  const int cx = SCENES[acc.scene].orbX, cy = orbRow(acc), r = SCENES[acc.scene].orbR;
  for (int ring = 3; ring >= 1; ring--)
    for (int dy = -r - ring; dy <= r + ring; dy++) {
      int w = span(r + ring, dy);
      px(cx - w, cy + dy, blend(skyAt(acc, cx, cy + dy), 0xfff0c8, 0.05f * (4 - ring)), 2 * w + 1);
    }
  disc(cx, cy, r, 0xfff4da);
  static const int8_t SEAS[][2] = {{-3, -2}, {-2, -2}, {-3, -1}, {-2, -1}, {1, 0}, {2, 0}, {2, 1},
                                   {-1, 2}, {0, 2}, {0, 3}, {1, -3}, {2, -3}, {3, -2}};
  for (auto &p : SEAS) px(cx + p[0], cy + p[1], 0xe6d8b6);
}

// Two ridges of low hills on the horizon; the near one runs into the ground under the panels.
static void hills(const Account &acc, uint32_t back, uint32_t front) {
  int horizon = acc.horizon / 2;
  for (int x = 2; x < W / 2 - 2; x++) {
    int top = horizon - 5 + (int)roundf(1.6f * sinf(x / 9.0f + 1) + 0.9f * sinf(x / 4.7f + 2));
    int low = horizon - 2 + (int)roundf(1.2f * sinf(x / 13.0f + 4));
    px(x, top, back, 1, horizon - top);
    px(x, low, front, 1, horizon - low);
  }
}

// A jagged ridge's height above the horizon at column x: straight slopes between seeded peaks and saddles.
static int ridge(int x, uint32_t seed, int spacing, int low, int high) {
  auto height = [&](int k) {
    int half = (high - low) / 2 + 1;
    uint32_t n = noise(seed * 7919 + k);
    return k % 2 ? low + (int)(n % half) : high - (int)(n % half);
  };
  int k = (x + spacing) / spacing;
  float f = (float)((x + spacing) % spacing) / spacing;
  return (int)roundf(height(k) + (height(k + 1) - height(k)) * f);
}

// A mountain range on the horizon; with a cap colour, snow lies on the highest slopes.
static void mountains(const Account &acc, uint32_t seed, int spacing, int low, int high, uint32_t color,
                      uint32_t cap = 0) {
  int horizon = acc.horizon / 2;
  for (int x = 2; x < W / 2 - 2; x++) {
    int tall = ridge(x, seed, spacing, low, high);
    px(x, horizon - tall, color, 1, tall);
    if (cap && tall > high - 3) px(x, horizon - tall, cap, 1, min(2, tall - (high - 3)));
  }
}

// Curtains of light along a wavering line, brightest at their lower edge; ripples and bright patches drift along.
static float auroraMid(const Account &acc) { return acc.horizon / 2 * 0.5f; }

static void aurora(const Account &acc, uint32_t t) {
  float s = t / 1000.0f, mid = auroraMid(acc);
  for (int x = 2; x < W / 2 - 2; x++) {
    float edge = mid + 2.0f * sinf(x / 13.0f + s * 0.21f) + 1.0f * sinf(x / 5.0f - s * 0.37f);
    float tall = 6 + 2.5f * sinf(x / 19.0f - s * 0.13f) + 1.5f * sinf(x / 7.0f + s * 0.29f);
    float bright = 0.55f + 0.45f * sinf(x / 9.0f - s * 0.5f);
    int bottom = (int)roundf(edge);
    for (int y = bottom - (int)tall; y <= bottom; y++) {
      float k = 1 - (bottom - y) / tall;  // 1 at the lower edge
      glaze(x, y, blend(0x7b5cff, 0x3dffa8, k), roundf(0.7f * bright * k * k * 8) / 8);
    }
  }
}

// A line `thick` units wide, for wires and branches.
static void stroke(float x0, float y0, float x1, float y1, int thick, uint32_t color) {
  int n = max(1, (int)ceilf(max(fabsf(x1 - x0), fabsf(y1 - y0))));
  for (int i = 0; i <= n; i++) {
    float f = (float)i / n;
    px((int)roundf(x0 + (x1 - x0) * f), (int)roundf(y0 + (y1 - y0) * f), color, thick, thick);
  }
}

// Light glowing through from far off: concentric steps brightening the sky towards their centre.
static void glow(const Account &acc, int cx, int cy, int r, uint32_t color, float most) {
  for (int k = 1; k <= 5; k++) {
    int ring = r * (6 - k) / 5;
    float a = most * k * k / 25;
    for (int dy = -ring; dy <= ring; dy++) {
      int y = cy + dy, w = span(ring, dy);
      if (y >= 2 && y < acc.horizon / 2) px(cx - w, y, blend(bandAt(acc, y), color, a), 2 * w + 1);
    }
  }
}

// A pine silhouette: tiers that each widen then step back in, with snow lying along the top of each tier, lit from
// the left. Tall ones taper more slowly, like spruces.
static void pine(int cx, int base, int tall, uint32_t color, uint32_t snow) {
  int tier = tall < 14 ? 3 : 4;
  for (int r = 0; r < tall; r++) {
    int half = r / tier + r % tier, y = base - tall + r;
    px(cx - half, y, color, 2 * half + 1);
    if (r % tier == 0 && half >= 2) px(cx - half, y, snow, half);
  }
  px(cx, base - tall, snow);
}

// One depth of a forest. Gaps and sizes are seeded, so every redraw plants the trees in the same places.
struct Grove {
  uint32_t seed;
  int gapLo, gapHi;    // between neighbouring trees
  int sizeLo, sizeHi;  // tree height
  int lift;            // rows above the horizon the trees stand on; negative sinks their feet under the panels
};

static int groveSize(const Grove &g, int cx) { return g.sizeLo + noise(g.seed * 3 + cx + 99) % (g.sizeHi - g.sizeLo + 1); }

static int groveNext(const Grove &g, int cx) { return cx + g.gapLo + noise(g.seed + cx + 99) % (g.gapHi - g.gapLo + 1); }

static void pines(const Account &acc, const Grove &g, uint32_t color, uint32_t snow) {
  for (int cx = (int)(noise(g.seed) % g.gapHi) - 4; cx < W / 2 + 8; cx = groveNext(g, cx))
    pine(cx, acc.horizon / 2 - g.lift, groveSize(g, cx), color, snow);
}

static const int FLAKES = 40;

// Flakes fall at three speeds on a gusting wind that swings them all the same way. The fast, bright ones are near and
// fall in front of the nearest trees; the rest fall behind them.
static void snow(const Account &acc, uint32_t t, bool near) {
  int horizon = acc.horizon / 2;
  uint32_t step = t / SCENES[acc.scene].stepMs;
  float gust = 4 * sinf(step / 37.0f) + 2 * sinf(step / 11.0f);
  for (int i = 0; i < FLAKES; i++) {
    uint32_t n = noise(900 + i), m = noise(n);
    int slow = 1 + n % 3, fall = horizon + 2;
    if ((slow == 1) != near) continue;
    uint32_t s = step / slow + m % 997;
    int y = (int)(s % fall);
    int x = (int)(((n >> 8) % 160 + s / 3 + 400 + (int)roundf(gust * (2.0f - 0.4f * slow))) % 160);
    if (x < 2 || x >= W / 2 - 2 || y < 2 || y >= horizon - 1) continue;
    px(x, y, slow == 1 ? 0xf2f5ff : slow == 2 ? 0xc5cdf0 : 0x8f9ad0);
  }
}

static const int SEA_ROWS = 6;

// Calm water under the moon: faint swells drift along each row and short glints come and go along the moon's path.
static void sea(const Account &acc, uint32_t t) {
  const Scene &s = SCENES[acc.scene];
  int horizon = acc.horizon / 2, top = horizon - SEA_ROWS;
  uint32_t step = t / s.stepMs;
  for (int r = 0; r < SEA_ROWS; r++) {
    int y = top + r;
    uint32_t water = r == 0 ? 0x24366c : blend(0x142048, 0x060c22, (r - 1) / (SEA_ROWS - 2.0f));
    px(2, y, water, W / 2 - 4);
    uint32_t drift = t / (1500 + r * 500);
    for (int x = 2; x < W / 2 - 2; x++)
      if (noise(r * 1013 + ((uint32_t)x - drift) / 4) % 6 == 0) px(x, y, blend(water, 0x5a78c0, 0.25f));
    int reach = 2 + r;
    for (int x = s.orbX - reach; x <= s.orbX + reach; x += 2) {
      float middle = 1 - fabsf(x - s.orbX) / (reach + 1.0f);
      uint32_t n = noise(x * 31 + y * 977 + step * 7);
      if (n % 100 < 70 * middle) px(x - n / 100 % 2, y, blend(water, 0xffe6a8, 0.3f + 0.4f * middle), 2);
    }
  }
}

static const uint32_t FLOCK_LAP_MS = 90000, FLOCK_STEP_MS = 350;

static int flockRow(const Account &acc, uint32_t t) { return acc.horizon / 5 + noise(t / FLOCK_LAP_MS) % 4; }

// Three birds glide across once a lap, beating their wings now and then.
static void birds(const Account &acc, uint32_t t) {
  static const int8_t AT[3][2] = {{0, 0}, {-4, -2}, {-7, 1}};
  int lead = (int)((t % FLOCK_LAP_MS) / FLOCK_STEP_MS) - 4, row = flockRow(acc, t);
  for (int b = 0; b < 3; b++) {
    int x = lead + AT[b][0], y = row + AT[b][1], tips = (t / 700 + b) % 3 ? -1 : 1;
    if (x < 3 || x >= W / 2 - 3) continue;
    px(x, y, 0x2e2440);
    px(x - 1, y + tips, 0x2e2440);
    px(x + 1, y + tips, 0x2e2440);
  }
}

// A soft band of glow and a crowd of faint stars along the Milky Way.
static void milkyWay(const Account &acc) {
  int horizon = acc.horizon / 2;
  for (int y = 2; y < horizon; y++)
    for (int x = 2; x < W / 2 - 2; x++)
      if (milkyGlow(acc, x, y) > 0) px(x, y, skyAt(acc, x, y));
  float rise = horizon + 10.0f, run = 130, norm = sqrtf(run * run + rise * rise);
  for (int i = 0; i < 90; i++) {
    uint32_t n = noise(i + 3131), m = noise(n);
    float along = n % 1000 / 1000.0f, off = (int)(m % 100 + (m >> 8) % 100 - 100) / 20.0f;
    int x = (int)roundf(20 + along * run + off * rise / norm), y = (int)roundf(horizon + 4 - along * rise + off * run / norm);
    if (x < 2 || x >= W / 2 - 2 || y < 2 || y >= horizon) continue;
    px(x, y, blend(skyAt(acc, x, y), (m >> 16) & 1 ? 0xe0d8ff : 0xfff0dc, 0.25f + (m >> 20) % 4 * 0.12f));
  }
}

static const uint32_t METEOR_EVERY_MS = 20000, METEOR_MS = 1200;
static const int METEOR_RUN = 24;  // units the head travels

// One meteor in every METEOR_EVERY_MS, at a seeded moment and place.
static uint32_t meteorStart(uint32_t t, int &x, int &y) {
  uint32_t k = t / METEOR_EVERY_MS, n = noise(k + 4242);
  x = 60 + n % 80;
  y = 3 + (n >> 8) % 5;
  return k * METEOR_EVERY_MS + (n >> 16) % (METEOR_EVERY_MS - 3000);
}

// A shooting star slides down to the left, its tail fading, brightening and dimming as it goes.
static void meteor(const Account &acc, uint32_t t) {
  int sx, sy;
  uint32_t start = meteorStart(t, sx, sy);
  if (t < start || t >= start + METEOR_MS) return;
  float p = (float)(t - start) / METEOR_MS, head = p * METEOR_RUN;
  for (int i = 0; i < 7 && head - i >= 0; i++) {
    int x = sx - (int)roundf(head - i), y = sy + (int)roundf((head - i) / 2);
    if (x >= 2 && y < acc.horizon / 2) px(x, y, blend(skyAt(acc, x, y), 0xffffff, sinf(PI * p) * (1 - i / 7.0f)));
  }
}

// Round-topped trees along the horizon: a trunk under two overlapping crowns.
static void treeline(const Account &acc, uint32_t seed, int spacing, int low, int high, uint32_t color) {
  int horizon = acc.horizon / 2;
  for (int cx = (int)(noise(seed) % spacing); cx < W / 2 + 6; cx += spacing + noise(seed + cx) % 4) {
    int r = low + noise(seed * 3 + cx) % (high - low + 1), cy = horizon - r - 2 - noise(seed * 5 + cx) % 3;
    px(cx, cy, color, 1, horizon - cy);
    disc(cx, cy, r, color);
    disc(cx + r / 2 + 1, cy + r / 2, r * 2 / 3, color);
  }
}

static void meadow(const Account &acc, uint32_t color) {
  int horizon = acc.horizon / 2;
  px(2, horizon - 1, color, W / 2 - 4);
  for (int x = 2; x < W / 2 - 2; x++) {
    uint32_t n = noise(x + 6060);
    if (n % 3 == 0) px(x, horizon - 2 - (n >> 8) % 2, color, 1, 1 + (n >> 8) % 2);
  }
}

static const int FIREFLY_COUNT = 18, FIREFLY_ROWS = 14;

// Fireflies wander over the grass, each glowing up and fading on its own slow rhythm.
static void fireflies(const Account &acc, uint32_t t) {
  int horizon = acc.horizon / 2;
  for (int i = 0; i < FIREFLY_COUNT; i++) {
    uint32_t n = noise(i + 777), m = noise(n), period = 3500 + n % 3000;
    float glow = max(0.0f, sinf(2 * PI * ((t + m % period) % period) / period));
    glow = roundf(glow * glow * 4) / 4;
    if (glow <= 0) continue;
    int x = 4 + (int)((n >> 8) % 152) + (int)roundf(2.0f * sinf(t / (5000.0f + m % 3000) + n % 7));
    int y = horizon - 3 - (int)((m >> 8) % 9) + (int)roundf(1.2f * sinf(t / (4000.0f + n % 2000) + m % 7));
    glaze(x, y, 0xeaff8a, glow);
    if (glow < 0.5f) continue;
    for (int d : {-1, 1}) {
      glaze(x + d, y, 0xc8ff60, 0.35f * glow);
      glaze(x, y + d, 0xc8ff60, 0.35f * glow);
    }
  }
}

static const int SAKURA_X = 88;
static const uint32_t BARK = 0x4a2c3c, FOREST = 0x3c5a52, FAR_HILLS = 0x9aaccc, GRASS = 0x5b8a56;
// Blossom from the side catching the sun to the deepest shade.
static const uint32_t BLOOM[4] = {0xffe4ee, 0xf5b8d0, 0xdb8fb2, 0xb06a92};
// The crown's clumps as (dx, dy, half-width, half-height) from the fork, back to front.
static const int8_t CLUMPS[][4] = {{0, -15, 7, 4}, {-9, -13, 6, 4}, {9, -13, 6, 4}, {-16, -9, 5, 3},
                                   {16, -9, 5, 3}, {-4, -10, 5, 3}, {5, -10, 5, 3}};
// Drapes of blossom hanging from under the crown as (dx, dy, width, length) from the fork; the first three hang behind.
static const int8_t DRAPES[][4] = {{-13, -8, 2, 11}, {13, -8, 2, 12},  {-1, -9, 2, 5},   {-20, -8, 2, 12}, {-16, -7, 3, 13},
                                   {-10, -8, 3, 10}, {-6, -7, 2, 6},   {7, -7, 2, 7},    {11, -8, 3, 11},  {16, -7, 3, 13},
                                   {20, -8, 2, 11}};
static const int BACK_DRAPES = 3;

// The tree is drawn for the taller card and shrinks to fit a shorter one.
static float sakuraScale(const Account &acc) { return min(1.0f, acc.horizon / 2 / 36.0f); }

// Top of the low mound the tree stands on.
static int hillTop(const Account &acc, int x) {
  float d = (x - SAKURA_X) / 34.0f;
  return acc.horizon / 2 - (int)roundf(3.5f * max(0.0f, 1 - d * d));
}

static int sakuraFork(const Account &acc) { return hillTop(acc, SAKURA_X) - (int)roundf(8 * sakuraScale(acc)); }

static int crownHalf(const Account &acc) { return (int)roundf(21 * sakuraScale(acc)); }

static int scaled(const Account &acc, int v) { return (int)roundf(v * sakuraScale(acc)); }

// Wooded hills behind the tree, rising highest to the right, with hazy ridges further off.
static int forestTop(const Account &acc, int x) {
  float a = (x - 118) / 32.0f, b = (x - 58) / 20.0f;
  return acc.horizon / 2 - scaled(acc, (int)roundf(3 + 13 * expf(-a * a) + 6 * expf(-b * b)));
}

static void forest(const Account &acc) {
  mountains(acc, 71, 12, scaled(acc, 8), scaled(acc, 16), FAR_HILLS);
  for (int x = 2; x < W / 2 - 2; x++) {
    int top = forestTop(acc, x);
    px(x, top, FOREST, 1, acc.horizon / 2 - top);
  }
  for (int cx = 0; cx < W / 2 + 4; cx += 2 + noise(cx + 5150) % 3)
    pine(cx, forestTop(acc, cx) + 2, 4 + noise(cx + 5151) % 4, FOREST, FOREST);
}

// Whether (dx, dy) from a clump's centre is inside it; a few seeded bites keep the edge from looking traced.
static bool inClump(int i, int rx, int ry, int dx, int dy) {
  float e = dx * dx / ((rx + 0.5f) * (rx + 0.5f)) + dy * dy / ((ry + 0.5f) * (ry + 0.5f));
  return e <= (noise(i * 7919 + dx * 31 + dy * 997) % 5 ? 1.0f : 0.7f);
}

// A clump of blossom, shaded below and to the right and lit along its upper left edge.
static void clump(const Account &acc, int i) {
  int cx = SAKURA_X + scaled(acc, CLUMPS[i][0]), cy = sakuraFork(acc) + scaled(acc, CLUMPS[i][1]);
  int rx = max(2, scaled(acc, CLUMPS[i][2])), ry = max(2, scaled(acc, CLUMPS[i][3]));
  for (int dy = -ry; dy <= ry + 1; dy++)
    for (int dx = -rx; dx <= rx + 1; dx++)
      if (inClump(i, rx, ry, dx - 1, dy - 1)) px(cx + dx, cy + dy, BLOOM[2]);
  for (int dy = -ry; dy <= ry; dy++)
    for (int dx = -rx; dx <= rx; dx++) {
      if (!inClump(i, rx, ry, dx, dy)) continue;
      bool lit = dx <= rx / 3 && (!inClump(i, rx, ry, dx, dy - 1) || noise(i * 131 + dx * 17 + dy) % 11 == 0);
      px(cx + dx, cy + dy, BLOOM[lit ? 0 : 1]);
    }
}

// A drape of blossom narrowing to its tip, shaded down its right side; the lower half sways on the breeze.
static void drape(const Account &acc, int i, uint32_t t) {
  int x0 = SAKURA_X + scaled(acc, DRAPES[i][0]), top = sakuraFork(acc) + scaled(acc, DRAPES[i][1]);
  int width = DRAPES[i][2], len = max(2, scaled(acc, DRAPES[i][3])), back = i < BACK_DRAPES;
  for (int r = 0; r <= len; r++) {
    int w = r >= len ? 1 : r >= len - 2 ? max(1, width - 1) : width;
    int sway = r > len / 2 ? (int)roundf(sinf(t / 1800.0f + x0 / 5.0f) * (r - len / 2) / (len / 2.0f)) : 0;
    for (int c = 0; c < w; c++) {
      int shade = r >= len - 1 ? 3 : c == w - 1 && w > 1 ? 2 : 1;
      px(x0 - w / 2 + c + sway, top + r, BLOOM[min(3, shade + back)]);
    }
  }
}

// A weeping cherry on a grassy mound in the morning: a dark trunk under a dome of blossom whose drapes hang almost to
// the ground, with fallen petals in the grass.
static void sakura(const Account &acc, uint32_t t) {
  int horizon = acc.horizon / 2, base = hillTop(acc, SAKURA_X), fork = sakuraFork(acc), half = crownHalf(acc);
  for (int x = 2; x < W / 2 - 2; x++) {
    int top = hillTop(acc, x);
    if (top >= horizon) continue;
    px(x, top, GRASS, 1, horizon - top);
    px(x, top, blend(GRASS, 0xd8f0a0, 0.35f));
  }
  for (int i = 0; i < BACK_DRAPES; i++) drape(acc, i, t);
  for (int y = fork - 2; y <= base; y++) px(SAKURA_X - 1 - (y > (fork + base) / 2), y, BARK, 2 + (y > base - 3));
  px(SAKURA_X - 3, base, BARK, 7);
  for (int d : {-1, 1}) stroke(SAKURA_X, fork, SAKURA_X + d * scaled(acc, 10), fork - scaled(acc, 7), 2, BARK);
  for (int i = BACK_DRAPES; i < (int)(sizeof(DRAPES) / sizeof(DRAPES[0])); i++) drape(acc, i, t);
  for (int i = 0; i < (int)(sizeof(CLUMPS) / sizeof(CLUMPS[0])); i++) clump(acc, i);
  for (int x = SAKURA_X - half - 2; x <= SAKURA_X + half + 2; x++)
    if (noise(x + 7070) % 3 == 0) px(x, hillTop(acc, x) + 1 + (int)(noise(x) % 2), BLOOM[1 + (noise(x) & 1)]);
}

static const int PETALS = 10;

// Petals let go of the blossom one at a time and flutter down and to the right until they reach the ground.
static void petals(const Account &acc, uint32_t t) {
  uint32_t step = t / SCENES[acc.scene].stepMs;
  int half = crownHalf(acc), from = sakuraFork(acc) - (int)roundf(8 * sakuraScale(acc));
  for (int i = 0; i < PETALS; i++) {
    uint32_t n = noise(i + 555), m = noise(n);
    int cx = SAKURA_X + (int)(n % (2 * half + 1)) - half, cy = from + (int)(m % 6);
    int slow = 2 + m % 2, fall = acc.horizon / 2 - cy, period = fall * slow + 20 + (m >> 16) % 40;
    uint32_t s = (step + (n >> 8)) % period;
    if (s >= (uint32_t)(fall * slow)) continue;
    float f = (float)s / slow;
    int x = cx + (int)roundf(0.7f * f + 1.5f * sinf(f / 2.5f + n % 7)), y = cy + (int)f;
    if (x < 2 || x >= W / 2 - 2 || y >= hillTop(acc, x)) continue;
    px(x, y, BLOOM[(m >> 20) & 1]);
  }
}

static const int FUJI_X = 92, FUJI_PEAK = 7;  // centre column and summit row
static const uint32_t FUJI_HAZE = 0xdde6f2;

// Height of Mount Fuji above the horizon at column x: a broad, slightly dipped summit, then slopes that flare out.
static int fujiHeight(const Account &acc, int x) {
  int tall = acc.horizon / 2 - FUJI_PEAK, d = abs(x - FUJI_X);
  if (d <= 3) return tall - (d <= 1);
  return (int)roundf(tall * powf(max(0.0f, 1 - (d - 3) / 72.0f), 2.2f));
}

// The haze thickens towards the foot of the mountain, in a few flat steps.
static float fujiHaze(const Account &acc, int y) {
  float f = (float)(y - (acc.horizon / 2 - 16)) / 16;
  return f <= 0 ? 0 : roundf(min(1.0f, f * f) * 5) / 5 * 0.8f;
}

// Snow on the upper slopes reaches further down in streaks; the left side is in sun, the right in shade, and the foot
// fades into the haze.
static void fuji(const Account &acc) {
  int horizon = acc.horizon / 2, summit = horizon - FUJI_PEAK;
  for (int x = 2; x < W / 2 - 2; x++) {
    int tall = fujiHeight(acc, x);
    bool lit = x <= FUJI_X;
    uint32_t n = noise(x * 13 + 7);
    int snowLine = summit * 11 / 20 - (int)(n % 3) - ((n >> 8) % 4 == 0 ? 3 + (int)((n >> 12) % 3) : 0);
    for (int y = horizon - tall; y < horizon;) {
      bool snowy = horizon - y > snowLine;
      float haze = fujiHaze(acc, y);
      int end = y + 1;
      while (end < horizon && (horizon - end > snowLine) == snowy && fujiHaze(acc, end) == haze) end++;
      uint32_t c = snowy ? (lit ? 0xf4f6ff : 0xc9d1ee) : (lit ? 0x6a7ab4 : 0x56649c);
      px(x, y, blend(c, FUJI_HAZE, haze), 1, end - y);
      y = end;
    }
  }
}

// A bank of mist lying along the ground: solid below, thinning to a dither along its top.
static void mistBank(const Account &acc, int top, uint32_t color) {
  for (int y = top; y < acc.horizon / 2; y++) {
    if (y - top >= 3) {
      px(2, y, color, W / 2 - 4);
      continue;
    }
    for (int x = 2; x < W / 2 - 2; x++)
      if ((x + y * 3) % 4 <= y - top) px(x, y, color);
  }
}

// Thin mist over everything in the bottom rows, thicker towards the ground.
static void mistVeil(const Account &acc, int rows, uint32_t color) {
  int horizon = acc.horizon / 2;
  for (int y = horizon - rows; y < horizon; y++)
    for (int x = 2; x < W / 2 - 2; x++) glaze(x, y, color, 0.15f + 0.4f * (y - horizon + rows) / rows);
}

// A cedar: narrow drooping tiers, lit down the left edge and shaded down the right.
static void cedar(int cx, int base, int tall, uint32_t body, uint32_t lit, uint32_t shade) {
  for (int r = 0; r < tall; r++) {
    int half = r / 4 + min(r % 4, 2), y = base - tall + r;
    px(cx - half, y, body, 2 * half + 1);
    if (half) {
      px(cx - half, y, lit);
      px(cx + half, y, shade);
    }
  }
}

static void cedars(const Account &acc, const Grove &g, uint32_t body, uint32_t lit, uint32_t shade) {
  for (int cx = (int)(noise(g.seed) % g.gapHi) - 4; cx < W / 2 + 8; cx = groveNext(g, cx))
    cedar(cx, acc.horizon / 2 - g.lift, groveSize(g, cx), body, lit, shade);
}

// ---------- the scenes ----------

static void sunsetScene(const Account &acc, uint32_t t, const Wisp *wisps) {
  stars(acc, t, wisps);
  sun(acc, t);
  clouds(acc, wisps);
  hills(acc, 0x55203a, SCENES[SUNSET].ground);
}

static void tealNightScene(const Account &acc, uint32_t t, const Wisp *wisps) {
  stars(acc, t, wisps);
  crescent(acc);
  clouds(acc, wisps);
}

static void auroraScene(const Account &acc, uint32_t t, const Wisp *wisps) {
  stars(acc, t, wisps);
  aurora(acc, t);
  mountains(acc, 5, 9, 2, 9, 0x0e1a3a, 0x7d9cc0);
}

static const Grove PINE_FAR = {21, 2, 5, 3, 6, 4}, PINE_MID = {22, 3, 7, 6, 11, 2}, PINE_NEAR = {23, 7, 13, 11, 18, -1};
static const int16_t PINE_FRONT[][2] = {{-1, 44}, {80, 38}, {114, 46}, {161, 42}};  // (column, height) right by us

// Deeper into the woods, each row of pines is darker, bigger and further apart. Light glows through from far off, fog
// drifts between the rows and snow blows through on a gusting wind.
static void snowScene(const Account &acc, uint32_t t, const Wisp *wisps) {
  int horizon = acc.horizon / 2;
  glow(acc, 92, horizon - 9, 24, 0xdfe6ff, 0.4f);
  px(2, horizon - 4, 0x5c6aa6, W / 2 - 4, 4);
  pines(acc, PINE_FAR, 0x5c6aa6, 0x8e9ad2);
  clouds(acc, wisps);
  pines(acc, PINE_MID, 0x34427a, 0x7d8cc0);
  snow(acc, t, false);
  pines(acc, PINE_NEAR, 0x1a2348, 0xa8b4e4);
  px(2, horizon - 1, 0x9aa6d8, W / 2 - 4);
  for (auto &p : PINE_FRONT) pine(p[0], horizon + 8, p[1], 0x0a0f24, 0x7c89bc);
  snow(acc, t, true);
}

static void seaScene(const Account &acc, uint32_t t, const Wisp *wisps) {
  stars(acc, t, wisps);
  fullMoon(acc);
  clouds(acc, wisps);
  sea(acc, t);
}

static void dawnScene(const Account &acc, uint32_t t, const Wisp *wisps) {
  stars(acc, t, wisps);
  paleSun(acc);
  mountains(acc, 11, 9, 5, 11, 0xb07888);
  mountains(acc, 12, 7, 3, 7, 0x7a5478);
  clouds(acc, wisps);
  mountains(acc, 13, 6, 1, 4, 0x3a2c54);
  birds(acc, t);
}

// Fuji over a misty wood: the far cedars stand in a bank of mist, the near ones sharp and dark.
static void fujiScene(const Account &acc, uint32_t t, const Wisp *wisps) {
  fuji(acc);
  clouds(acc, wisps);
  hills(acc, blend(0x4f7f62, FUJI_HAZE, 0.6f), blend(0x2f5a3e, FUJI_HAZE, 0.4f));
  cedars(acc, {61, 2, 5, 4, 8, 4}, blend(0x46645c, FUJI_HAZE, 0.5f), blend(0x6a8a7e, FUJI_HAZE, 0.5f),
         blend(0x34504a, FUJI_HAZE, 0.5f));
  mistBank(acc, acc.horizon / 2 - 4, blend(FUJI_HAZE, 0xffffff, 0.2f));
  cedars(acc, {62, 6, 14, 6, 16, -1}, 0x22402e, 0x3f6a4a, 0x16281e);
  mistVeil(acc, 4, 0xf0f4fa);
  birds(acc, t);
}

static void milkyScene(const Account &acc, uint32_t t, const Wisp *wisps) {
  milkyWay(acc);
  stars(acc, t, wisps);
  meteor(acc, t);
  mountains(acc, 31, 8, 2, 8, 0x0c0a1c);
}

static void fireflyScene(const Account &acc, uint32_t t, const Wisp *wisps) {
  stars(acc, t, wisps);
  clouds(acc, wisps);
  treeline(acc, 41, 9, 3, 5, 0x152a33);
  treeline(acc, 42, 13, 4, 6, 0x08141a);
  meadow(acc, 0x08141a);
  fireflies(acc, t);
}

// A weeping cherry in front of wooded hills on a spring morning, its petals falling.
static void sakuraScene(const Account &acc, uint32_t t, const Wisp *wisps) {
  clouds(acc, wisps);
  forest(acc);
  sakura(acc, t);
  petals(acc, t);
  mistVeil(acc, 3, 0xfff6f0);
}

static void wholeSky(const Account &acc, uint32_t, int &y0, int &y1) {
  y0 = 4;
  y1 = acc.horizon;
}

// The stripes are all in the sun's lower half.
static void sunRows(const Account &acc, uint32_t, int &y0, int &y1) {
  y0 = orbRow(acc) * 2;
  y1 = acc.horizon;
}

static void auroraRows(const Account &acc, uint32_t, int &y0, int &y1) {
  y0 = (int)(auroraMid(acc) - 14) * 2;
  y1 = (int)(auroraMid(acc) + 4) * 2;
}

static void seaRows(const Account &acc, uint32_t, int &y0, int &y1) {
  y0 = acc.horizon - SEA_ROWS * 2;
  y1 = acc.horizon;
}

static void flockRows(const Account &acc, uint32_t t, int &y0, int &y1) {
  y0 = (flockRow(acc, t) - 4) * 2;
  y1 = (flockRow(acc, t) + 3) * 2;
}

// Empty except while a meteor is crossing, and for a moment after so its last frame is cleared.
static void meteorRows(const Account &acc, uint32_t t, int &y0, int &y1) {
  int sx, sy;
  uint32_t start = meteorStart(t, sx, sy);
  y0 = y1 = 0;
  if (t < start || t > start + METEOR_MS + 300) return;
  y0 = sy * 2;
  y1 = (sy + METEOR_RUN / 2 + 2) * 2;
}

static void fireflyRows(const Account &acc, uint32_t, int &y0, int &y1) {
  y0 = acc.horizon - FIREFLY_ROWS * 2;
  y1 = acc.horizon;
}

const Scene SCENES[SCENE_COUNT] = {
    {{0x2a1330, 0x3a1834, 0x4d1f36, 0x652836, 0x7f3335, 0x9a4133}, 0x2c0f27, {16, {0xffe2c4, 0xffc9a0}, 0.15f, 0.6f, 0.45f},
     CIRRUS, 0xffb08c, 0.35f, 99, -12, 7, STRIPE_MS, sunsetScene, sunRows},
    {{0x06171d, 0x082027, 0x0b2a2e, 0x0e3434, 0x113f39, 0x15493e}, 0x04121a, {28, {0x9bffd2, 0xe8fff4}, 0.3f, 1.0f, 1.0f},
     CIRRUS, 0xbfe8dc, 0.25f, 100, 15, 6, 0, tealNightScene, nullptr},
    {{0x050a1f, 0x07102a, 0x0a1734, 0x0c1e3c, 0x0f2643, 0x122e4a}, 0x060b18, {36, {0xe6f0ff, 0xc8f5e6}, 0.25f, 0.9f, 0.6f},
     nullptr, 0, 0, 0, 0, 0, 200, auroraScene, auroraRows},
    {{0x1a2350, 0x27346a, 0x384a86, 0x4f619e, 0x6c78b2, 0x8a8ec0}, 0x1b2346, {0, {0, 0}, 0, 0, 0},
     MIST, 0xc8d2f4, 0.3f, 0, 0, 0, 200, snowScene, wholeSky},
    {{0x050b24, 0x0a1433, 0x0f1c44, 0x152553, 0x1b2f62, 0x22396f}, 0x0a1430, {30, {0xf4f0ff, 0xc8d8ff}, 0.25f, 0.9f, 0.55f},
     CIRRUS, 0xc8d0ff, 0.18f, 100, -15, 6, 300, seaScene, seaRows},
    {{0x232a55, 0x3b3a6c, 0x5c4a7c, 0x86597f, 0xb06c7d, 0xd5877c}, 0x2a2246, {8, {0xffe8f0, 0xffd8e0}, 0.1f, 0.45f, 0.3f},
     MIST, 0xffe0d8, 0.35f, 99, -8, 5, 200, dawnScene, flockRows},
    {{0x5a86c8, 0x6c94ce, 0x82a4d6, 0x98b4dc, 0xaec4e2, 0xc4d4e8}, 0x2f5a3e, {0, {0, 0}, 0, 0, 0},
     HAZE, 0xf4f8fc, 0.6f, 0, 0, 0, 200, fujiScene, flockRows},
    {{0x06051a, 0x090722, 0x0d0a2a, 0x110d33, 0x15103b, 0x1a1444}, 0x07060f, {26, {0xffffff, 0xd8ccff}, 0.25f, 1.0f, 0.6f},
     nullptr, 0, 0, 0, 0, 0, 40, milkyScene, meteorRows},
    {{0x0b0f2e, 0x121a40, 0x1a2650, 0x22335a, 0x2c4162, 0x375066}, 0x07120f, {18, {0xfff4d0, 0xd0e0ff}, 0.15f, 0.7f, 0.45f},
     CIRRUS, 0x9fb8d8, 0.15f, 0, 0, 0, 120, fireflyScene, fireflyRows},
    {{0x6f9ad6, 0x8aaedd, 0xa8c0e2, 0xc8d0e2, 0xe6d8da, 0xf8e2d2}, 0x3e6a48, {0, {0, 0}, 0, 0, 0},
     CIRRUS, 0xffffff, 0.4f, 0, 0, 0, 200, sakuraScene, wholeSky},
};

static void scene(const Account &acc, uint32_t t) {
  const Wisp wisps[2] = {wispAt(acc, 0, t), wispAt(acc, 1, t)};
  for (int y = 2; y < acc.height / 2 - 2; y++) px(2, y, bandAt(acc, y), W / 2 - 4);
  SCENES[acc.scene].draw(acc, t, wisps);
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
static const uint32_t KNOT_STEP_MS = 30;
static LGFX_Sprite logoPatch(&tft);

// -1 is the resting logo. Clawd takes his laptop out, types, and puts it away again, as in the desktop app.
static int logoFrame(const Account &acc, uint32_t t) {
  uint32_t since = t - acc.workEdgeMs;
  if (acc.provider == CLAUDE) {
    if (acc.working <= 0) {
      if (!acc.workEdgeMs) return -1;  // never worked since boot
      int frame = CLAWD_OUT_START + since * CLAWD_OUT_FPS / 1000;
      return frame <= CLAWD_OUT_END ? frame : -1;
    }
    uint32_t step = since * CLAWD_FPS / 1000;
    int intro = CLAWD_LOOP_START - CLAWD_INTRO;
    if (step < intro) return CLAWD_INTRO + step;
    return CLAWD_LOOP_START + (step - intro) % (CLAWD_LOOP_END - CLAWD_LOOP_START + 1);
  }
  if (acc.working <= 0) return -1;
  if (acc.provider == CODEX) return t / KNOT_STEP_MS % KNOT_FRAMES;
  return -1;
}

static void drawLogo(lgfx::LovyanGFX *target, int x, int y, const Account &acc, int frame) {
  target->fillRect(x, y, LOGO_SIZE, LOGO_SIZE, rgb(INK));
  if (acc.provider == CLAUDE) {
    const char *pixels = CLAWD_ANIM[max(frame, 0)];
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
  // Inside a write block the push returns while DMA is still reading the canvas, and the next card draws into it.
  tft.waitDMA();
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

// Moving parts pass behind the labels, so the card is redrawn clipped to the rows that changed and just those rows are
// sent: a cloud's rows when its edge moves, and the scene's own rows on each of its ticks. Stars elsewhere twinkle in
// place on the panel.
static void redrawRows(Account &acc, uint32_t t, int y0, int y1) {
  y0 = max(y0, 4);
  y1 = min(y1, acc.height - 4);
  if (y1 > y0) drawCard(acc, acc.top, acc.height, t, y0, y1);
}

static void animateScene() {
  static uint32_t lastFrame = 0, lastStep[MAX_ACCOUNTS];
  static Wisp shown[MAX_ACCOUNTS][2];
  if (millis() - lastFrame < 80) return;
  uint32_t t = lastFrame = millis();
  tft.startWrite();
  for (int i = 0; i < accountCount; i++) {
    Account &acc = accounts[i];
    if (acc.height == 0) continue;
    const Scene &s = SCENES[acc.scene];
    const Wisp wisps[2] = {wispAt(acc, 0, t), wispAt(acc, 1, t)};
    for (int k = 0; k < 2 && s.clouds; k++) {
      if (!memcmp(&wisps[k], &shown[i][k], sizeof(Wisp))) continue;
      shown[i][k] = wisps[k];
      redrawRows(acc, t, cloudY(acc, k) * 2, (cloudY(acc, k) + CLOUD_ROWS) * 2);
    }
    int y0 = 0, y1 = 0;
    if (s.stepMs) {
      s.rows(acc, t, y0, y1);
      if (t / s.stepMs != lastStep[i]) redrawRows(acc, t, y0, y1);
      lastStep[i] = t / s.stepMs;
    }
    sceneTarget = &tft;
    sceneX = 0;
    sceneY = -acc.top;
    stars(acc, t, wisps, y0 / 2, (y1 + 1) / 2);
    sceneTarget = &canvas;
    sceneY = 0;
  }
  tft.endWrite();
}

static int cardAt(int32_t y) {
  for (int i = 0; i < accountCount; i++)
    if (y >= accounts[i].top && y < accounts[i].top + accounts[i].height) return i;
  return -1;
}

// Next scene for a card, skipping the one on the other card so the two always differ.
static void cycleScene(int card) {
  int next = accounts[card].scene;
  do next = (next + 1) % SCENE_COUNT;
  while (accountCount > 1 && next == accounts[1 - card].scene);
  pickedScene[card] = accounts[card].scene = next;
  dirty = true;
}

// A single tap opens the card's conversation once the double-tap window has passed; a second tap on the same card
// within it cycles the scene instead.
static const uint32_t DOUBLE_TAP_MS = 300;
static int pendingCard = -1;
static uint32_t pendingSince = 0;

static void handleTap(int32_t x, int32_t y) {
  Serial.printf("TOUCH %ld %ld\n", (long)x, (long)y);
  int card = cardAt(y);
  if (card < 0) return;
  if (pendingCard == card) {
    pendingCard = -1;
    cycleScene(card);
  } else {
    pendingCard = card;
    pendingSince = millis();
  }
}

static void flushPendingTap() {
  if (pendingCard < 0 || millis() - pendingSince < DOUBLE_TAP_MS) return;
  if (pendingCard < accountCount) Serial.printf("OPEN %s\n", accounts[pendingCard].name);
  pendingCard = -1;
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
  tft.setRotation(3);
  tft.setBrightness(255);
  line.reserve(1024);
  render();
  Serial.println("{\"hello\":\"agent-tap-screen\"}");
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
  flushPendingTap();

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
