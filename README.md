# Usage Screen

A desk display for Claude and Codex usage limits, plus a nudge when a conversation is waiting for you.

It runs on a JC2432W328C (ESP32, 2.8" 320×240 ST7789, capacitive touch) plugged into the Mac over USB.
The look is the "Pixel Retro" face from [token-tv](https://github.com/click6067-ship-it/token-tv) (MIT).

- **Claude card:** weekly limit left as the headline, the 5-hour limit in a thinner bar above it.
- **Codex card:** weekly limit left.
- Bars are full at 100% left and empty as you use them; colour moves green → lime → orange → red.
- A reading older than 15 minutes is marked `OLD`; a missing one shows `--`, never 0%.
- **Waiting conversations:** a light runs round the card's frame and a red badge shows `!` (or the count).
  Tap the card to open the longest-waiting one in the Claude desktop app or Codex.
- **Working conversations:** a spinner beside the name while an agent is mid-turn — Claude's asterisks, Codex's
  turning knot.
- Tapping a card with nothing waiting opens a working conversation; tap again while that app is in front to
  cycle through the others. With nothing working either, it brings the app (Claude or ChatGPT) to the front.

## How it works

```
Claude Code / Codex hooks ──► ~/.cache/usage-screen/{working,waiting}/*.json ─┐
Claude usage API, codex app-server ──────────────────────────────────────────┼─► bridge (LaunchAgent) ──USB serial──► ESP32
                                                  open claude:// / codex:// ◄┘◄── "OPEN <name>" on tap ◄──────────────┘
```

- `bridge/usage_bridge.py` reads usage every 2 minutes, watches the working and waiting folders, sends one JSON line
  to the screen on change (and every 30 s), and opens conversations when the screen asks.
- `bridge/hook.py` is the hook both agents call: `work` on each prompt and tool call, `wait` when a turn
  ends or needs permission, `clear` when the session ends.
- `firmware/` draws everything; the Mac only sends numbers.

## Setup

Requirements: Python 3 (stdlib only), PlatformIO (`brew install platformio`), the ChatGPT app (its bundled
`codex` CLI), and a signed-in Claude Code (`claude` then `/login`).

1. **Flash** (use a USB-A→C cable; C→C may not power the board):
   ```bash
   cd firmware && pio run -t upload --upload-port /dev/cu.usbserial-*
   ```
2. **Check readings:** `python3 bridge/usage_bridge.py --once`
3. **Run at login:**
   ```bash
   cp launchd/local.usage-screen.plist ~/Library/LaunchAgents/
   launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.usage-screen.plist
   launchctl kickstart gui/$(id -u)/local.usage-screen
   ```
   Log: `~/Library/Logs/usage-screen.log`.
4. **Hooks:**
   - Claude: in `~/.claude/settings.json`, `Stop` and `Notification` run `hook.py claude wait`;
     `UserPromptSubmit`, `PreToolUse` and `PostToolUse` run `hook.py claude work`; `SessionEnd` runs `hook.py claude clear`.
   - Codex: `Stop` runs `hook.py codex wait`; `UserPromptSubmit`, `PreToolUse` and `PostToolUse` run `hook.py codex work`.
     Install with `python3 bridge/add_codex_hooks.py`, then approve the new hooks in Codex.

## Everyday commands

The bridge holds the serial port, so stop it before flashing or taking a screenshot:

```bash
launchctl bootout gui/$(id -u)/local.usage-screen                                   # stop
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.usage-screen.plist    # start
launchctl kickstart -k gui/$(id -u)/local.usage-screen                              # restart after bridge edits
python3 bridge/screenshot.py screen.png                                                   # capture the cards as a PNG
python3 bridge/usage_bridge.py --debug-claude                                             # raw Claude usage JSON
```

## Credits

Pixel Retro layout, 5×7 font and the Claude mascot come from token-tv (MIT). The ChatGPT knot is
thresholded from the ChatGPT app's own icon.
