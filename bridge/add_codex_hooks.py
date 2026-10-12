#!/usr/bin/env python3
"""Add the usage-screen waiting hooks to Codex's hooks.json, keeping every existing hook (backs it up first)."""
import json
import shutil
import sys
from pathlib import Path

HOOK = f"python3 {Path(__file__).resolve().with_name('hook.py')} codex "


def main():
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "~/.codex/hooks.json").expanduser()
    raw = path.read_text() if path.exists() else "{}"
    if path.exists():
        shutil.copy(path, str(path) + ".bak-usage-screen")
    data = json.loads(raw)
    hooks = data.setdefault("hooks", {})
    for event, action in (("Stop", "wait"), ("UserPromptSubmit", "clear"), ("PostToolUse", "clear")):
        command = HOOK + action
        entries = hooks.setdefault(event, [])
        if not any(h.get("command") == command for e in entries for h in e.get("hooks", [])):
            entries.append({"hooks": [{"type": "command", "command": command, "timeout": 5}]})
    path.write_text(json.dumps(data, indent=2) + "\n")
    print(f"updated {path}; approve the new hooks in Codex when it asks")


if __name__ == "__main__":
    main()
