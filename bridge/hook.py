#!/usr/bin/env python3
"""Claude Code / Codex hook: marks a conversation as working or waiting for the user, or clears it.

Usage: hook.py <claude|codex> <work|wait|clear>, with the hook's JSON on stdin.
Prints nothing and always exits 0 so it can never block the agent.
"""
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

WAITING_DIR = Path("~/.cache/usage-screen/waiting").expanduser()
WORKING_DIR = Path("~/.cache/usage-screen/working").expanduser()
APP_NAMES = {"claude": "Claude", "codex": "ChatGPT"}
CLAUDE_LOG = Path("~/Library/Logs/Claude/main.log").expanduser()
FOCUS_RE = re.compile(r"setFocusedSession: sessionId=(\S+)")


def claude_focused_session():
    # The desktop app logs every chat switch; the last entry is the open chat.
    try:
        with CLAUDE_LOG.open("rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - 262144))
            found = FOCUS_RE.findall(f.read().decode("utf-8", "replace"))
        return found[-1] if found else ""
    except Exception:
        return ""


def frontmost_app():
    # Fails open (returns "") so a lookup problem never hides a real waiting conversation.
    try:
        out = subprocess.run(
            ["osascript", "-e", 'tell application "System Events" to get name of first process whose frontmost is true'],
            capture_output=True, text=True, timeout=2)
        return out.stdout.strip()
    except Exception:
        return ""


def write_record(directory, provider, session, data):
    directory.mkdir(parents=True, exist_ok=True)
    record = {"provider": provider, "session": session, "since": int(time.time()),
              "event": data.get("hook_event_name"), "cwd": data.get("cwd")}
    path = directory / f"{provider}-{session}.json"
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(record))
    tmp.replace(path)


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("claude", "codex") or sys.argv[2] not in ("work", "wait", "clear"):
        return
    provider, action = sys.argv[1], sys.argv[2]
    data = json.loads(sys.stdin.read() or "{}")
    if provider == "claude":
        # Only Claude desktop sessions can be reopened, and its links take the desktop's own session id.
        session = os.environ.get("CLAUDE_CODE_HOST_SESSION_ID", "")
        if not re.fullmatch(r"local_[A-Za-z0-9-]{1,64}", session):
            return
    else:
        session = str(data.get("session_id") or data.get("thread_id") or data.get("conversation_id") or "")
        # The desktop app runs hidden ephemeral forks (e.g. next-message suggestions) that have no transcript
        # and can't be opened.
        transcript = data.get("transcript_path")
        if action != "clear" and not (transcript and Path(transcript).is_file()):
            return
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", session):
        return
    name = f"{provider}-{session}.json"
    # A conversation is working or waiting, never both.
    if action != "work":
        (WORKING_DIR / name).unlink(missing_ok=True)
    if action != "wait":
        (WAITING_DIR / name).unlink(missing_ok=True)
    if action == "work":
        write_record(WORKING_DIR, provider, session, data)
    if action != "wait":
        return
    if frontmost_app() == APP_NAMES[provider] and (provider == "codex" or claude_focused_session() == session):
        return
    write_record(WAITING_DIR, provider, session, data)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
