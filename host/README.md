# clawd-bridge

Mac-side daemon that pairs with the Cardputer over BLE and drives host-side
processes on its behalf — `claude` CLI sessions for `apps/chat`, an ssh
client for `apps/ssh`, etc. The Cardputer is I/O; the bridge owns the
processes and the host context.

See [`protocol/WIRE.md`](../protocol/WIRE.md) for the wire protocol.

## Status

MVP scaffolding. Connects to a Cardputer advertising the Nordic UART
Service, prints received lines on stdout, and forwards lines typed on
stdin to the device. No `claude` CLI integration yet — that lands once
the dial tone is verified end-to-end.

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

## Plans (in order)

1. Add a dedicated `clawd-bridge` BLE service on the Cardputer (separate
   UUIDs, see `protocol/WIRE.md`) so this daemon can coexist with Claude
   Desktop's buddy connection.
2. Spawn and manage a `claude` CLI process; pipe stdin/stdout and forward
   chunks as `chat.chunk` events.
3. `launchd` plist so the daemon starts at login.
