# clawdputer

Firmware and host-side bridge that turn the [M5Stack Cardputer](https://docs.m5stack.com/en/core/Cardputer)
into a companion device for Claude, plus a small platform for additional
standalone apps.

The first installed app is a Cardputer port of
[`anthropics/claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy):
the Cardputer pairs with Claude on the Mac over BLE, shows what Claude is
doing, and lets you approve or deny permission prompts from the device — no
window switching.

Beyond buddy, the firmware is structured as a small declarative app
platform. New apps drop in as `firmware/src/apps/<name>/`, register
themselves with a single macro, and pick up the launcher and shared
services (BLE, WiFi, SD, NVS) for free.

## Repo layout

```
firmware/                ESP32 firmware (PlatformIO)
  src/
    core/                app registry, event bus
    services/            ble, (wifi, sd, …)
    apps/<name>/         one directory per app
    main.cpp             entrypoint
host/                    Mac-side bridge daemon (planned)
protocol/                wire-protocol reference (planned)
```

## Status

- `firmware/src/apps/buddy/` — Claude Desktop companion working over BLE NUS
  with encrypted bonding. Status display, approve/deny, owner/name/status
  acks.
- `host/` and `protocol/` — not yet started; tracked as the next milestones.

## Build & flash

```bash
cd firmware
pio run                 # compile
pio run -t upload       # flash over USB
pio device monitor      # serial console
```

PlatformIO targets the `m5stack-stamps3` board (the ESP32-S3 module inside
the Cardputer) with the Arduino framework and the `M5Cardputer` library.

### Wireless updates (OTA)

Three different OTA flows are available; pick by use case.

**1. Push from your laptop (espota)** — fastest iteration:

```bash
pio run -e cardputer-ota -t upload
```

The device advertises itself as `clawdputer.local` on the LAN. Requires
`firmware/src/wifi_secrets.h` with `CLAWD_WIFI_SSID` / `CLAWD_WIFI_PASS`
when *first* flashed via USB — credentials then migrate to NVS and
survive future updates.

**2. GitOps — push to main, device updates itself.**

Every push that touches `firmware/**` triggers
`.github/workflows/firmware.yml`, which builds the binary and publishes
to a `latest` release on GitHub. The device's `updater` service polls
that release every 5 minutes and self-flashes when a new build SHA
appears. Manual trigger: Settings → "check for updates".

The current running version's short git SHA is embedded at build time
(`CLAWD_BUILD_SHA`) and shown in Sysinfo / Settings.

**Rollback safety:** before each self-flash the new partition is marked
"pending" in NVS. After reboot the new firmware must run cleanly for 30
seconds to clear the flag. If three consecutive boots fail to confirm
health, the bootloader is pointed back at the previous partition and
the device reverts. Manual recovery: USB-flash from a working host.

**3. Initial provisioning** — see "First-time setup" below.

### First-time setup

1. Copy `firmware/src/wifi_secrets.h.example` to
   `firmware/src/wifi_secrets.h` and fill in `CLAWD_WIFI_SSID` /
   `CLAWD_WIFI_PASS`.
2. `cd firmware && pio run -e cardputer -t upload` (one USB flash).
3. The firmware migrates the credentials to NVS on first boot. From
   that point on, `wifi_secrets.h` is no longer needed — both espota
   and GitOps updates inherit the stored credentials.

## Hardware

M5Stack Cardputer: ESP32-S3, 320×240 IPS, 56-key keyboard, BLE, WiFi, IR,
microphone, speaker, microSD.
