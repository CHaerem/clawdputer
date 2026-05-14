#!/usr/bin/env bash
# Online installer for clawd-bridge — downloads the latest pre-built
# binary from GitHub Releases and registers it as a per-user launchd agent.
#
# Run on any Mac that wants to be a host for a Cardputer:
#
#   curl -fsSL https://github.com/CHaerem/clawdputer/raw/main/host/install/online.sh | bash
#
# Configure where chat spawns claude (defaults to $HOME) by exporting
# CLAWD_CHAT_CWD before running.
#
# Re-running the script picks up the newest published bridge build —
# this is also how you update.

set -euo pipefail

REPO="CHaerem/clawdputer"
BINARY_URL="https://github.com/${REPO}/releases/latest/download/clawd-bridge"
LABEL="com.clawdputer.bridge"
INSTALL_DIR="$HOME/Library/Application Support/clawd-bridge"
BIN_PATH="$INSTALL_DIR/clawd-bridge"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG_DIR="$HOME/Library/Logs/clawd-bridge"
CHAT_CWD="${CLAWD_CHAT_CWD:-$HOME}"

say() { printf "[install] %s\n" "$*"; }

say "destination: $BIN_PATH"
say "log dir:     $LOG_DIR"
say "chat cwd:    $CHAT_CWD"

mkdir -p "$INSTALL_DIR" "$LOG_DIR" "$(dirname "$PLIST")"

say "downloading latest bridge binary…"
curl -fsSL --retry 3 "$BINARY_URL" -o "$BIN_PATH.new"
chmod +x "$BIN_PATH.new"

# Downloaded binaries get a quarantine flag that prompts Gatekeeper. Strip
# it so launchd can spawn the agent silently. Same effect as the user
# right-clicking → Open the first time.
xattr -d com.apple.quarantine "$BIN_PATH.new" 2>/dev/null || true

# Atomic swap so launchd doesn't see a half-written file.
mv -f "$BIN_PATH.new" "$BIN_PATH"

# Stop the previous instance (if any) before we rewrite the plist.
if launchctl print "gui/$UID/$LABEL" >/dev/null 2>&1; then
    say "stopping previous agent"
    launchctl bootout "gui/$UID/$LABEL" 2>/dev/null || true
fi

say "writing $PLIST"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>

    <key>ProgramArguments</key>
    <array>
        <string>$BIN_PATH</string>
    </array>

    <key>EnvironmentVariables</key>
    <dict>
        <key>PATH</key>
        <string>$PATH</string>
        <key>CLAWD_CHAT_CWD</key>
        <string>$CHAT_CWD</string>
        <key>HOME</key>
        <string>$HOME</string>
    </dict>

    <key>RunAtLoad</key>
    <true/>

    <key>KeepAlive</key>
    <dict>
        <key>SuccessfulExit</key>
        <false/>
        <key>Crashed</key>
        <true/>
    </dict>

    <key>ThrottleInterval</key>
    <integer>10</integer>

    <key>StandardOutPath</key>
    <string>$LOG_DIR/clawd-bridge.out.log</string>
    <key>StandardErrorPath</key>
    <string>$LOG_DIR/clawd-bridge.err.log</string>

    <key>ProcessType</key>
    <string>Background</string>
</dict>
</plist>
EOF

say "bootstrap launchd agent"
launchctl bootstrap "gui/$UID" "$PLIST"
launchctl enable    "gui/$UID/$LABEL"
launchctl kickstart "gui/$UID/$LABEL" || true

cat <<EOF

[install] done.

First time? macOS may prompt for Bluetooth permission — grant it to the
clawd-bridge process when the dialog appears. After that, your Cardputer
pairs automatically whenever both this Mac and the device are awake.

Logs:
  tail -f $LOG_DIR/clawd-bridge.out.log
  tail -f $LOG_DIR/clawd-bridge.err.log

Re-run this script anytime to update to the latest published build.
EOF
