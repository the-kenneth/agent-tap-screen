# Usage Screen — agent notes

Personal project (see `~/Mine/CLAUDE.md`). Read `README.md` for what it does and how to run it; this file
holds what isn't obvious from the code.

## Layout

- `firmware/src/main.cpp` — everything on the device: JSON parsing, card rendering, waiting animation, touch.
- `firmware/src/display.h` — LovyanGFX pin config. `pixelfont.h` — 5×7 glyphs. `sprites.h` — generated
  32×32 RGB565 mascots (`0xF81F` = transparent); the generator scripts aren't in the repo.
- `bridge/usage_bridge.py` — Mac side: usage polling (background thread), waiting folder, serial I/O.
- `bridge/hook.py` — agent hook; must stay silent and always exit 0 so it can never block an agent.
- `bridge/screenshot.py` — sends one live reading, then captures the cards to a PNG.
- `bridge/statusline.sh` — optional fallback source for Claude usage; not wired up by default.

## Hardware facts (verified on this board)

- ST7789 on HSPI: SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, backlight 27. `invert = false`, `rgb_order = false`.
- Touch CST820 at I2C 0x15 (LovyanGFX `Touch_CST816S`): SDA 33, SCL 32, RST 25, INT 21. Rotation 1 = 320×240.
- Serial is a CH340 at `/dev/cu.usbserial-*`. Uploads at 921600 baud fail; 460800 works.
- The panel can't be read back (`readRect` returns black), so screenshots stream each card's sprite instead.
  Gaps between cards and the waiting animation are drawn straight to the panel and aren't captured.
- The ESP32 has no PSRAM: draw one card at a time into a 16-bit sprite; a full-screen sprite won't fit.

## Serial protocol (115200, newline-terminated)

Mac → screen:
- `{"now": epoch, "accounts": [{"name", "color", "status", "age", "waiting", "windows": [{"label", "used", "resets"}]}]}`
  - `used` is percent used; the firmware shows 100 − used. `label` is `5H`, `WEEK` or a model name.
  - `status` other than `ok`, or `age` over 15 min, marks the card OLD.
- `SHOT` → screen replies `SHOT w h`, then `CARD top h` plus raw big-endian RGB565 rows per card, then `END`.

Screen → Mac: `OPEN <name>` (tap on a waiting card), `TOUCH x y` (every tap; logged with `-v`).

## Data sources

- **Claude usage:** OAuth token from Keychain item `Claude Code-credentials` (or the hashed
  `Claude Code-credentials-<sha256(config dir)[:8]>`), sent to `https://api.anthropic.com/api/oauth/usage`.
  Use its `limits` array (`session` → 5H, `weekly_all` → WEEK, `weekly_scoped` → model name).
  - The token lasts about 8 h and only Claude Code refreshes it. On a 401 the bridge runs a one-line Haiku
    `claude -p`, at most every 30 min, to make it renew.
- **Codex usage:** `codex app-server` JSON-RPC `account/rateLimits/read`, using the binary bundled in
  `/Applications/ChatGPT.app/Contents/Resources/codex-cli/bin/codex` (there's no `codex` on PATH).
  This account only reports a weekly window.
- **Waiting conversations:** hooks write `~/.cache/usage-screen/waiting/<provider>-<id>.json`; the bridge
  drops entries older than 12 h and deletes an entry when it opens it.
  - **Claude:** only desktop-app sessions count. The id is `$CLAUDE_CODE_HOST_SESSION_ID` (`local_…`),
    not the hook's `session_id`; `claude://code/continue?session=` rejects anything but `local_…`.
  - **Codex:** id is the hook's `session_id`; opened with `codex://threads/<id>`.
  - Rejected deep links show in `~/Library/Logs/Claude/main.log` as `claudeURLHandler: … invalid`.

## Working on it

- Stop the LaunchAgent before `pio run -t upload` or `screenshot.py`, and start it again afterwards
  (commands in README). Only one process can hold the serial port.
- After a firmware change, run `bridge/screenshot.py` and look at the PNG instead of asking the user.
- Never print, log or copy tokens or credential contents; `--debug-claude` prints only plan and expiry.
- A missing reading stays `--`; never turn unknown into 0%.
- Live config outside the repo: `~/.claude/settings.json` hooks, `~/.codex/hooks.json` hooks,
  `~/Library/LaunchAgents/local.usage-screen.plist` (a copy of `launchd/`).
