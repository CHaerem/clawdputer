# clawd-bridge

Mac-side daemon that pairs with the Cardputer over BLE and drives host-side
processes on its behalf — `claude` CLI sessions for `apps/chat`, an ssh
client for `apps/ssh`, etc. The Cardputer is I/O; the bridge owns the
processes and the host context.

See [`protocol/WIRE.md`](../protocol/WIRE.md) for the wire protocol.

## Status

Connects to the Cardputer over BLE (clawd-bridge service), and bridges the
`apps/chat` firmware app to a host-side `claude` CLI session: each
`chat.send` event from the device spawns `claude --print [--continue]`
in the configured working directory, streams stdout back as `chat.chunk`
events, and emits `chat.end` when the process exits.

`claude` must be on the bridge's `PATH`. Override the working directory
the CLI runs in by setting `CLAWD_CHAT_CWD` (defaults to `$HOME`).

## Build & run

```bash
cd host
swift build
.build/debug/clawd-bridge
```

Override the device name prefix (default `Claude-Cardputer`):

```bash
CLAWD_NAME_PREFIX=Claude-Cardputer-AB12 .build/debug/clawd-bridge
```

### Bluetooth permission

CoreBluetooth from a CLI inherits its Bluetooth permission from the
terminal that launched it. On first run macOS will prompt — grant it to
your terminal (Terminal.app, iTerm, VS Code, etc). If you previously
denied, re-enable under **System Settings → Privacy & Security →
Bluetooth**.

## Run with a specific working directory

```bash
CLAWD_CHAT_CWD=~/Projects/some-repo .build/debug/clawd-bridge
```

The `claude` CLI runs in that directory, so it picks up the right repo
context and any `CLAUDE.md` files at the root.

## Auto-start at login

```bash
CLAWD_CHAT_CWD=~/Projects/some-repo host/install/install.sh
host/install/uninstall.sh                # remove
```

The script builds a release binary, writes
`~/Library/LaunchAgents/com.clawdputer.bridge.plist` with absolute paths,
and loads it via `launchctl`. Logs end up in
`~/Library/Logs/clawd-bridge/`.

## Plans (in order)

1. Multi-session: more than one `claude` process behind named sessions
   the Cardputer can switch between.
2. Tool-use status feedback (currently only the tool name is forwarded
   when a tool block starts — could include the args / summary too).
