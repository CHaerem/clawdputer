#!/usr/bin/env bash
# Remove the clawd-bridge launchd agent.

set -euo pipefail

label="com.clawdputer.bridge"
plist_dst="$HOME/Library/LaunchAgents/$label.plist"

if launchctl print "gui/$UID/$label" >/dev/null 2>&1; then
    echo "[uninstall] bootout"
    launchctl bootout "gui/$UID/$label"
fi

if [[ -f "$plist_dst" ]]; then
    echo "[uninstall] rm $plist_dst"
    rm "$plist_dst"
fi

echo "[uninstall] done"
