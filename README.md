# clawdputer

Firmware and Mac-side bridge that turn the [M5Stack Cardputer ADV](https://docs.m5stack.com/en/core/Cardputer-Adv)
into a hardware companion for Claude. Multi-app launcher with passive
status (buddy), active remote (chat over the bridge), live usage
metrics, an SSH client with sealed host presets, OTA + GitOps
self-updates, and more — all on a 240×135 IPS display.

## What it is, in one paragraph

The Cardputer pairs with your Mac over BLE. Claude Desktop talks the
Anthropic-defined buddy protocol so the device can show session status
and approve/deny tool prompts. A second BLE service runs in parallel
where a custom `clawd-bridge` daemon on the Mac spawns `claude` CLI
sessions on demand, streams output back to the device, and reads
`~/.claude/stats-cache.json` for usage figures. Apps on the device are
laid out as a declarative registry; adding a new one is one directory
and one `REGISTER_APP(...)` call.

## Apps shipped today

| App      | Description |
|----------|-------------|
| Home     | Coverflow launcher — `,`/`/` to rotate, Enter to launch, `G0`/`Tab` from anywhere |
| Buddy    | Claude Desktop companion — session counts, tokens, approve/deny prompts |
| Chat     | Type prompts on the Cardputer; bridge spawns `claude --print` and streams text back |
| Usage    | Daily / 7d / 30d activity + cost from bridge (reads stats-cache.json + `/cost`) |
| SSH      | WiFi SSH client with Ed25519 key auth, sealed-secrets host presets, ad-hoc add + NVS save |
| Sysinfo  | Battery, uptime, heap, links, WiFi/IP, build SHA |
| Settings | Device info + actions (show pubkey, show seal key, configure WiFi, check updates, …) |
| WiFi     | Scan + connect (launched from Settings → "configure WiFi") |

## Repo layout

```
firmware/                  ESP32 firmware (PlatformIO)
  src/main.cpp             entrypoint — boots services, runs active app
  src/core/                App struct, registry, event bus, key codes
  src/services/            ble, wifi, ota, updater, identity, sealed
  src/ui/                  canvas (backbuffer sprite), statusbar, chrome,
                           list, toast — shared primitives
  src/apps/<name>/         one directory per app, REGISTER_APP() inside
  src/secrets/             sealed blobs (auto-generated at build, gitignored)
  src/wifi_secrets.h       optional compile-time WiFi creds (gitignored)
  scripts/                 PlatformIO pre-build hooks (version, secrets)
  platformio.ini           envs: cardputer (USB) and cardputer-ota
host/                      Mac-side bridge daemon (Swift)
  Sources/clawd-bridge/    CoreBluetooth central + claude CLI runner + usage
  install/                 launchd plist installer
protocol/WIRE.md           authoritative wire-protocol spec
tools/                     seal-hosts.py + plaintext hosts.json (gitignored)
.github/workflows/         CI build + GitHub release for OTA updates
```

## Hardware

M5Stack Cardputer-Adv: ESP32-S3, 240×135 IPS, 56-key keyboard with
arrow glyphs on `;,./`, G0 side button, BLE, WiFi, IR LED, microphone,
speaker, microSD, 1750 mAh battery.

## Quick start

```bash
# Firmware
cd firmware
pio run -e cardputer -t upload      # USB flash (one time)

# Bridge
cd host
swift build
host/install/install.sh             # install as launchd agent
```

After first flash:

1. Pair Claude Desktop → Developer → Open Hardware Buddy → pick the
   `Claude-Cardputer-<mac>` entry. Enter the 6-digit passkey when prompted.
2. On the device: open the **WiFi** app from Settings → "configure WiFi",
   scan, select, enter password. Credentials persist to NVS.
3. (Optional) Settings → "show SSH pubkey" → copy the `ssh-ed25519 …`
   line into `~/.ssh/authorized_keys` on each target host.
4. (Optional) Settings → "show seal key", then on the Mac:
   ```bash
   export CLAWD_SEAL_KEY=<the base64 line>
   echo '[{"name":"mac","host":"…","user":"…","port":22}]' > tools/hosts.json
   python3 tools/seal-hosts.py
   git add firmware/secrets/ssh_hosts.sealed && git commit && git push
   ```
   CI builds, devices self-update over OTA, presets show up in the SSH app.

## Two BLE links

Both services live on the same peripheral simultaneously, so Claude
Desktop and `clawd-bridge` can be connected at the same time.

| Service        | UUID                                       | Speaks                 |
|----------------|--------------------------------------------|------------------------|
| Buddy (NUS)    | `6e400001-b5a3-f393-e0a9-e50e24dcca9e`     | Anthropic buddy spec   |
| clawd-bridge   | `c1aedb01-1d0c-4adc-9b1a-c1aedb010000`     | Our extended protocol  |

Advertising only carries the NUS UUID so Claude Desktop's filter
matches; the bridge service is discovered after connect.

## Wireless flashing (OTA)

Two flows are supported.

### Push from your laptop (espota)

```bash
cd firmware && pio run -e cardputer-ota -t upload
```

The device advertises as `clawdputer.local`. Needs `wifi_secrets.h` on
the *first* USB flash so credentials reach NVS; afterwards the credentials
survive across OTA updates.

### GitOps — push to main, devices update themselves

Every push touching `firmware/**` triggers `.github/workflows/firmware.yml`
which builds `firmware.bin` + `version.txt` and uploads them to the
`latest` GitHub release. On-device, `services/updater` polls every 5
minutes (or on demand via Settings → "check for updates") and self-flashes
when the SHA differs.

**Rollback safety:** the new partition is marked "pending" in NVS before
flashing. If three consecutive boots fail to confirm health within 30s,
`updater::begin()` points the bootloader back at the previous slot.

## Sealed secrets

Sensitive connection data (SSH hosts, credentials) is encrypted with a
per-device AES-256-GCM seal key before it enters the repo. The seal key
is generated alongside the device's Ed25519 SSH keypair on first boot
and persisted in NVS. Plaintext `tools/hosts.json` stays gitignored on
the workstation; only the sealed `firmware/secrets/*.sealed` blobs
travel through git.

See [`protocol/WIRE.md`](protocol/WIRE.md) for the on-the-wire format
and [`tools/seal-hosts.py`](tools/seal-hosts.py) for the encrypt script.

## Status

Working end to end on a single device:

- Coverflow launcher with persistent status bar (BLE/wifi/battery)
- Buddy approve/deny against Claude Desktop
- Chat over bridge with streaming `claude --print --output-format stream-json`
- Usage snapshot pulling from stats-cache + `/cost`
- SSH key auth against Mac, sealed + saved + ad-hoc host management
- WiFi scan/connect from device
- OTA + GitOps + app-level rollback
- Backbuffer-rendered UI, no flicker, side-button (G0) shortcut to home
- C++17 firmware; flash 45.6% used, RAM 21.8%

Known limitations (next things to tackle):

- BLE-only bridge — no rechability outside ~5-10 m of the Mac
- Bridge is pull-only — no proactive push when stats change
- ANSI escape sequences are stripped in chat/SSH terminal views

## Documentation entry points

- [`CLAUDE.md`](CLAUDE.md) — orientation for future Claude sessions
- [`protocol/WIRE.md`](protocol/WIRE.md) — wire protocol spec
- [`host/README.md`](host/README.md) — bridge daemon details and launchd install
