#!/bin/sh
# Installs (or reinstalls) the bridge as a LaunchAgent running from this checkout.
set -e
repo=$(cd "$(dirname "$0")/.." && pwd)
label=local.usage-screen
dest="$HOME/Library/LaunchAgents/$label.plist"
sed -e "s|__REPO__|$repo|g" -e "s|__HOME__|$HOME|g" -e "s|__PYTHON__|$(command -v python3)|g" \
  "$repo/launchd/usage-screen.plist" > "$dest"
if launchctl bootout "gui/$(id -u)/$label" 2>/dev/null; then sleep 1; fi  # bootout finishes asynchronously
launchctl bootstrap "gui/$(id -u)" "$dest"
launchctl kickstart "gui/$(id -u)/$label"  # launchd can leave a fresh load unstarted
echo "Installed $dest"
