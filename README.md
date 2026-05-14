# clawdputer

Firmware for the [M5Stack Cardputer](https://docs.m5stack.com/en/core/Cardputer) that
turns it into a companion device for Claude — and a small platform for other
standalone apps.

The first target is a Cardputer port of
[`anthropics/claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy):
the Cardputer pairs with Claude on the Mac over BLE, shows what Claude is doing,
and lets you approve or deny permission prompts from the device — without
switching windows.

Around that, the firmware is structured so additional apps (notes, IR remote,
direct Anthropic API chat over WiFi, timers, …) can be added declaratively as
the project grows.

## Status

Scaffolding only. Boots, prints `clawdputer` on the display. Nothing else yet.

## Build & flash

```bash
pio run                 # compile
pio run -t upload       # flash over USB
pio device monitor      # serial console
```

PlatformIO config targets the `m5stack-stamps3` board (the ESP32-S3 module
inside the Cardputer) with the Arduino framework and the `M5Cardputer` library.

## Hardware

- M5Stack Cardputer (ESP32-S3, 320x240 IPS, 56-key keyboard, BLE, WiFi, IR,
  microphone, speaker, microSD).
