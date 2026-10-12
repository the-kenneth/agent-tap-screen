#!/bin/sh
# Claude Code status line: caches the input JSON (incl. rate_limits) for usage_bridge.py, then prints a short line.
input=$(cat)
mkdir -p "$HOME/.cache/usage-screen"
printf '%s' "$input" > "$HOME/.cache/usage-screen/claude-statusline.json"
printf '%s' "$input" | python3 -c 'import json,sys; d=json.load(sys.stdin); r=d.get("rate_limits") or {}; print(" · ".join(f"{k}: {round(v.get(\"used_percentage\",0))}%" for k,v in r.items() if isinstance(v,dict)) or d.get("model",{}).get("display_name",""))'
