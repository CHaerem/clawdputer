#!/usr/bin/env bash
# Install clawd-bridge as a per-user launchd agent. Builds a release binary,
# writes the plist with absolute paths substituted in, and loads it via
# launchctl so it starts now and at every login.
#
# Configure the chat working directory by setting CLAWD_CHAT_CWD before
# running this script. Default is $HOME.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
host_dir="$(cd "$here/.." && pwd)"
label="com.clawdputer.bridge"
plist_src="$here/$label.plist.template"
plist_dst="$HOME/Library/LaunchAgents/$label.plist"
log_dir="$HOME/Library/Logs/clawd-bridge"
chat_cwd="${CLAWD_CHAT_CWD:-$HOME}"

echo "[install] building release binary…"
(cd "$host_dir" && swift build -c release)
bin="$host_dir/.build/release/clawd-bridge"
[[ -x "$bin" ]] || { echo "build did not produce $bin" >&2; exit 1; }

echo "[install] log dir: $log_dir"
mkdir -p "$log_dir"
mkdir -p "$(dirname "$plist_dst")"

# Stop any previous instance before rewriting the plist.
if launchctl list | grep -q "$label"; then
    echo "[install] bootout previous agent"
    launchctl bootout "gui/$UID/$label" 2>/dev/null || true
fi

echo "[install] writing $plist_dst"
sed \
    -e "s|{{BIN}}|$bin|g" \
    -e "s|{{PATH}}|$PATH|g" \
    -e "s|{{CWD}}|$chat_cwd|g" \
    -e "s|{{HOME}}|$HOME|g" \
    -e "s|{{LOG}}|$log_dir|g" \
    "$plist_src" > "$plist_dst"

echo "[install] bootstrap"
launchctl bootstrap "gui/$UID" "$plist_dst"
launchctl enable    "gui/$UID/$label"
launchctl kickstart "gui/$UID/$label" || true

echo "[install] done. logs:"
echo "  tail -f $log_dir/clawd-bridge.out.log"
echo "  tail -f $log_dir/clawd-bridge.err.log"
