#!/usr/bin/env python3
"""Read Claude and Codex usage on the Mac and stream it to the screen over USB serial."""
import argparse
import glob
import hashlib
import json
import os
import select
import subprocess
import sys
import termios
import threading
import time
import unicodedata
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

CODEX_BIN = "/Applications/ChatGPT.app/Contents/Resources/codex-cli/bin/codex"
STATUSLINE_CACHE = Path("~/.cache/usage-screen/claude-statusline.json").expanduser()
WAITING_DIR = Path("~/.cache/usage-screen/waiting").expanduser()
WAIT_EXPIRY = 12 * 3600
CLAUDE_WINDOWS = (("five_hour", "5H"), ("seven_day", "WEEK"))


def epoch(value):
    if isinstance(value, (int, float)) and not isinstance(value, bool) and value > 0:
        return int(value)
    if isinstance(value, str):
        try:
            return int(datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp())
        except ValueError:
            return None
    return None


def window(label, used, resets):
    if isinstance(used, bool) or not isinstance(used, (int, float)):
        return None
    return {"label": label, "used": round(float(used), 1), "resets": epoch(resets)}


def keychain_services():
    # Claude Code names its Keychain item after a hash of the config dir; older versions use the bare name.
    home = os.environ.get("CLAUDE_CONFIG_DIR") or str(Path("~/.claude").expanduser())
    digest = hashlib.sha256(unicodedata.normalize("NFC", home).encode()).hexdigest()[:8]
    return ["Claude Code-credentials-" + digest, "Claude Code-credentials"]


def claude_credentials():
    """Every readable Claude Code login, freshest expiry first."""
    found = []
    for service in keychain_services():
        result = subprocess.run(["security", "find-generic-password", "-w", "-s", service],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=10)
        if result.returncode == 0 and result.stdout.strip():
            found.append((service, json.loads(result.stdout)["claudeAiOauth"]))
    path = Path(os.environ.get("CLAUDE_CONFIG_DIR") or "~/.claude").expanduser() / ".credentials.json"
    if path.is_file():
        found.append((str(path), json.loads(path.read_text())["claudeAiOauth"]))
    found = [item for item in found if (item[1].get("expiresAt") or 0) / 1000 > time.time()] or found
    if not found:
        raise RuntimeError("not signed in to Claude Code")
    return sorted(found, key=lambda item: item[1].get("expiresAt") or 0, reverse=True)


def claude_request(token):
    request = urllib.request.Request("https://api.anthropic.com/api/oauth/usage", headers={
        "Authorization": "Bearer " + token, "Accept": "application/json",
        "anthropic-beta": "oauth-2025-04-20", "User-Agent": "claude-code/2.1.278"})
    with urllib.request.urlopen(request, timeout=20) as response:
        return json.load(response)


def claude_oauth():
    error = None
    for _, credentials in claude_credentials():
        try:
            data = claude_request(credentials["accessToken"])
            break
        except urllib.error.HTTPError as failure:
            error = failure
    else:
        raise error
    windows = [window(limit_label(limit), limit.get("percent"), limit.get("resets_at"))
               for limit in data.get("limits") or [] if isinstance(limit, dict)]
    if not any(windows):
        windows = [window(label, (data.get(key) or {}).get("utilization"), (data.get(key) or {}).get("resets_at"))
                   for key, label in CLAUDE_WINDOWS]
    return [w for w in windows if w], data


def limit_label(limit):
    kind = limit.get("kind")
    if kind == "session":
        return "5H"
    if kind == "weekly_all":
        return "WEEK"
    model = ((limit.get("scope") or {}).get("model") or {}).get("display_name")
    return (model or kind or "LIMIT").upper()[:7]


def renew_claude_login():
    """Claude Code owns token rotation, so a minimal Haiku prompt makes it renew an expired login."""
    subprocess.run(["claude", "-p", "Reply only OK.", "--model", "haiku", "--tools", "",
                    "--no-session-persistence", "--max-turns", "1"],
                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=90)


def claude_statusline():
    """Fallback written by statusline.sh from Claude Code's status line input; stale once sessions stop."""
    data = json.loads(STATUSLINE_CACHE.read_text())
    limits = data.get("rate_limits") or {}
    windows = [window(label, (limits.get(key) or {}).get("used_percentage"), (limits.get(key) or {}).get("resets_at"))
               for key, label in CLAUDE_WINDOWS]
    return [w for w in windows if w], int(STATUSLINE_CACHE.stat().st_mtime)


def codex_usage():
    process = subprocess.Popen([CODEX_BIN, "app-server"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, text=True, bufsize=1)
    try:
        def call(ident, method, params):
            process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": ident, "method": method, "params": params}) + "\n")
            process.stdin.flush()
            deadline = time.monotonic() + 30
            for line in process.stdout:
                try:
                    message = json.loads(line)
                except ValueError:
                    continue
                if message.get("id") == ident:
                    if "error" in message:
                        raise RuntimeError(message["error"].get("message", "rpc error"))
                    return message.get("result") or {}
                if time.monotonic() > deadline:
                    break
            raise RuntimeError("codex timeout")

        call(1, "initialize", {"clientInfo": {"name": "usage_screen", "title": "Usage Screen", "version": "0.1"},
                               "capabilities": {}})
        process.stdin.write('{"method":"initialized","params":{}}\n')
        process.stdin.flush()
        limits = call(2, "account/rateLimits/read", {}).get("rateLimits") or {}
    finally:
        process.kill()
        process.wait()
    windows = []
    for key in ("primary", "secondary"):
        value = limits.get(key) or {}
        label = {300: "5H", 10080: "WEEK"}.get(value.get("windowDurationMins"), "LIMIT")
        item = window(label, value.get("usedPercent"), value.get("resetsAt"))
        if item:
            windows.append(item)
    return windows


class Account:
    def __init__(self, name, color, fetch):
        self.name, self.color, self.fetch = name, color, fetch
        self.windows, self.fetched_at, self.status = [], None, "waiting"

    def refresh(self, verbose):
        try:
            self.windows, self.fetched_at = self.fetch()
            self.status = "ok" if self.windows else "no data"
        except Exception as error:  # keep the last reading; the screen marks it OLD by age
            self.status = "error" if not self.windows else "ok"
            if verbose:
                print(f"{self.name}: {error}", file=sys.stderr)

    def row(self, now):
        age = now - self.fetched_at if self.fetched_at else 0
        return {"name": self.name, "color": self.color, "status": self.status, "age": age, "windows": self.windows}


def fetch_claude(verbose):
    last_renewal = [0.0]

    def fetch():
        try:
            try:
                return claude_oauth()[0], int(time.time())
            except urllib.error.HTTPError as error:
                if error.code != 401 or time.monotonic() - last_renewal[0] < 1800:
                    raise
                last_renewal[0] = time.monotonic()
                if verbose:
                    print("Claude login expired; renewing via claude -p", file=sys.stderr)
                renew_claude_login()
                return claude_oauth()[0], int(time.time())
        except urllib.error.HTTPError as error:
            if verbose:
                print(f"Claude usage API: HTTP {error.code}; using status line cache", file=sys.stderr)
            return claude_statusline()
    return fetch


def fetch_codex():
    return lambda: (codex_usage(), int(time.time()))


def open_serial(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0  # iflag
    attrs[1] = 0  # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0  # lflag
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/cu.wchusbserial*"))
    return ports[0] if ports else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="serial device (default: first /dev/cu.usbserial-*)")
    parser.add_argument("--poll", type=int, default=120, help="seconds between usage reads")
    parser.add_argument("--once", action="store_true", help="print one reading and exit, without the screen")
    parser.add_argument("--debug-claude", action="store_true", help="print Claude's raw usage JSON and exit")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if args.debug_claude:
        # Prints only non-secret metadata about each login, never the token itself.
        for service, credentials in claude_credentials():
            expires = (credentials.get("expiresAt") or 0) / 1000
            print(f"{service}: plan={credentials.get('subscriptionType')} "
                  f"expires_in={round((expires - time.time()) / 60)}min")
            try:
                print(json.dumps(claude_request(credentials["accessToken"]), indent=2))
            except urllib.error.HTTPError as error:
                print(f"  HTTP {error.code}: {error.read().decode(errors='replace')[:300]}")
        return

    accounts = [Account("Claude", "#D97757", fetch_claude(True)), Account("Codex", "#10A37F", fetch_codex())]
    if args.once:
        for account in accounts:
            account.refresh(True)
        now = int(time.time())
        print(json.dumps({"now": now, "accounts": [a.row(now) for a in accounts]}, indent=2))
        return

    # Usage reads take seconds, so they run apart from the loop that serves taps and waiting badges.
    def poll():
        while True:
            for account in accounts:
                account.refresh(args.verbose)
            time.sleep(args.poll)
    threading.Thread(target=poll, daemon=True).start()

    fd, incoming, last_sent, last_payload = None, b"", 0.0, None
    while True:
        waits = waiting()
        now = int(time.time())
        rows = [dict(a.row(now), waiting=len(waits.get(a.name.lower(), []))) for a in accounts]
        payload = (json.dumps({"now": now, "accounts": rows}) + "\n").encode()
        # Ages tick every second, so compare without them to send only on real changes.
        changed = json.dumps([{k: v for k, v in r.items() if k != "age"} for r in rows]) != last_payload
        try:
            if fd is None:
                port = args.port or find_port()
                if not port:
                    raise OSError("screen not connected")
                fd = open_serial(port)
                time.sleep(2)  # opening the port can reset the ESP32
                last_sent = 0.0
                if args.verbose:
                    print(f"connected to {port}", file=sys.stderr)
            if changed or time.monotonic() - last_sent >= 30:
                os.write(fd, payload)
                last_sent = time.monotonic()
                last_payload = json.dumps([{k: v for k, v in r.items() if k != "age"} for r in rows])
            incoming += read_available(fd)
            while b"\n" in incoming:
                line, incoming = incoming.split(b"\n", 1)
                handle_device_line(line.decode(errors="replace").strip(), waits, args.verbose)
        except OSError as error:
            if args.verbose:
                print(f"serial: {error}", file=sys.stderr)
            if fd is not None:
                os.close(fd)
            fd, incoming = None, b""
            time.sleep(5)
        time.sleep(0.25)


def waiting():
    """Conversations waiting for the user, per provider, oldest first; entries past WAIT_EXPIRY are dropped."""
    result = {}
    for path in WAITING_DIR.glob("*.json"):
        try:
            record = json.loads(path.read_text())
            if time.time() - record["since"] > WAIT_EXPIRY:
                path.unlink(missing_ok=True)
                continue
            record["path"] = path
            result.setdefault(record["provider"], []).append(record)
        except (OSError, ValueError, KeyError):
            continue
    for records in result.values():
        records.sort(key=lambda r: r["since"])
    return result


APP_NAMES = {"claude": "Claude", "codex": "ChatGPT"}


def conversation_url(record):
    if record["provider"] == "codex":
        return "codex://threads/" + record["session"]
    return "claude://code/continue?session=" + record["session"]


def handle_device_line(line, waits, verbose):
    if verbose and line.startswith("TOUCH"):
        print(line, file=sys.stderr)
    if not line.startswith("OPEN "):
        return
    provider = line[5:].strip().lower()
    records = waits.get(provider)
    if not records:
        # Nothing waiting: just bring the provider's app to the front.
        app = APP_NAMES.get(provider)
        if app:
            subprocess.run(["open", "-a", app], timeout=10)
        return
    record = records[0]
    subprocess.run(["open", conversation_url(record)], timeout=10)
    # Opened counts as seen, so the next tap moves on to the next waiting conversation.
    record["path"].unlink(missing_ok=True)
    if verbose:
        print(f"opened {conversation_url(record)}", file=sys.stderr)


def read_available(fd):
    data = b""
    while select.select([fd], [], [], 0)[0]:
        chunk = os.read(fd, 1024)
        if not chunk:
            break
        data += chunk
    return data


if __name__ == "__main__":
    main()
