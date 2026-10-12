#!/usr/bin/env python3
"""Claude Code / Codex hook: marks a conversation as waiting for the user, or clears it.

Usage: hook.py <claude|codex> <wait|clear>, with the hook's JSON on stdin.
Prints nothing and always exits 0 so it can never block the agent.
"""
import json
import os
import re
import sys
import time
from pathlib import Path

WAITING_DIR = Path("~/.cache/usage-screen/waiting").expanduser()


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("claude", "codex") or sys.argv[2] not in ("wait", "clear"):
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
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", session):
        return
    path = WAITING_DIR / f"{provider}-{session}.json"
    if action == "clear":
        path.unlink(missing_ok=True)
        return
    WAITING_DIR.mkdir(parents=True, exist_ok=True)
    record = {"provider": provider, "session": session, "since": int(time.time()),
              "event": data.get("hook_event_name"), "cwd": data.get("cwd")}
    tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(record))
    tmp.replace(path)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
