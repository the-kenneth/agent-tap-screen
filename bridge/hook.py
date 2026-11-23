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

WAITING_DIR = Path("~/.cache/agent-tap-screen/waiting").expanduser()
WORKING_DIR = Path("~/.cache/agent-tap-screen/working").expanduser()
APP_NAMES = {"claude": "Claude", "codex": "ChatGPT"}
CLAUDE_LOGS = [Path(f"~/Library/Logs/Claude/{name}.log").expanduser() for name in ("main", "main1")]
FOCUS_RE = re.compile(rb"setFocusedSession: sessionId=(\S+)")
CHUNK = 262144


def last_focus(path):
    # Reads backwards in chunks, overlapping so a line split across chunks is still found.
    with path.open("rb") as f:
        end = f.seek(0, os.SEEK_END)
        while end > 0:
            start = max(0, end - CHUNK)
            f.seek(start)
            found = FOCUS_RE.findall(f.read(end - start + 200))
            if found:
                return found[-1].decode()
            end = start
    return ""


def claude_focused_session():
    # The desktop app logs every chat switch; the last entry is the open chat. A quiet chat can be open for hours
    # and the log rotates, so the entry may be far back or in the previous file.
    for path in CLAUDE_LOGS:
        try:
            session = last_focus(path)
        except OSError:
            continue
        if session:
            return session
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
    # The idle reminder fires a minute after Stop, which already decided; it would re-flag a chat you just read.
    if data.get("notification_type") == "idle_prompt" or "waiting for your input" in str(data.get("message", "")):
        return
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
