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

## Hardware

M5Stack Cardputer: ESP32-S3, 320×240 IPS, 56-key keyboard, BLE, WiFi, IR,
microphone, speaker, microSD.
