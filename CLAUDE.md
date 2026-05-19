# clawdputer — notes for future Claude sessions

This codebase is primarily AI-developed. This file is the orientation
hand-off so a future Claude session can be productive in five minutes.

## What this is

Firmware for the M5Stack Cardputer (ESP32-S3) plus a Mac-side bridge
daemon. Together they turn the device into a companion for Claude:

- A passive companion to Claude Desktop (status display, approve/deny
  on permission prompts) over the Anthropic-defined buddy protocol.
- An active remote for a `claude` CLI session running on the host —
  typed prompts on the device keyboard, streamed output on the screen.

Both halves live in this repo. Adding more standalone apps (ssh client,
notes, IR remote, …) is the intended growth path.

## Repo layout

```
firmware/                       ESP32 firmware (PlatformIO)
  src/main.cpp                  bootstraps services, runs active app
  src/core/                     App struct, registry, event bus
  src/services/                 ble, wifi, ota — shared between apps
  src/apps/<name>/<name>.cpp    one directory per app, REGISTER_APP() inside
  src/wifi_secrets.h            gitignored; SSID/password for OTA
  platformio.ini                envs: cardputer (USB) and cardputer-ota
host/                           Mac-side bridge daemon (Swift)
  Sources/clawd-bridge/         CoreBluetooth central + claude CLI runner
  install/                      launchd plist installer
web/                            Browser simulator (GitHub Pages)
  app.js, index.html, styles    240×135 canvas + JS reimplementations
  apps.json                     generated catalog mirroring firmware apps
tools/gen-web-manifest.py       parses firmware App {...} blocks → apps.json
protocol/WIRE.md                authoritative wire-protocol spec
README.md                       user-facing instructions
```

## Two BLE links — keep them separate

The firmware exposes two BLE services on the same peripheral by design:

1. **NUS (Nordic UART Service)** — Anthropic's `claude-desktop-buddy`
   protocol. Read-only status + approve/deny. Claude Desktop is the
   only peer. We do not extend it; the spec is theirs.

2. **clawd-bridge service** — our UUIDs, used by `host/clawd-bridge` to
   drive a `claude` CLI session and stream output back.

Both peers can be connected simultaneously (NimBLE multi-connection).
Apps filter events by `EventSource` (`NusLink` vs `BridgeLink`).
Advertising only carries the NUS UUID — there's no room in the 31-byte
primary packet for two 128-bit UUIDs. The bridge service is discovered
after connect.

## What's possible and what isn't

Buddy is **read-only + permission decisions**. There is no public hook
to inject prompts into a running Claude Desktop GUI session. Don't try
to add that via accessibility scripting — it breaks across app updates
and isn't what the user wants.

The "remote-control Claude from Cardputer" feature works because the
*controlled* sessions are `claude` CLI processes spawned by the bridge,
not Claude Desktop GUI sessions. They run on the Mac with full file /
MCP / repo access.

## Adding things

**A new firmware app:**
1. Create `firmware/src/apps/<name>/<name>.cpp`.
2. Define `static App my_app = { .id=..., .name=..., .description=..., .onEnter=..., ... };`
3. Set `.services` to the bitwise OR of `SVC_BLE`, `SVC_WIFI`, `SVC_SD` the app needs.
   The framework starts/stops radios automatically on app switch — do **not** call
   `wifi::resume()` or `ble::resume()` manually in `onEnter`.
4. End with `REGISTER_APP(my_app);` — registry collects it on boot.
5. Subscribe to events in `onEnter` (`events::subscribe(...)`),
   unsubscribe in `onExit`. Filter by `e.source` to your link.
6. `apps/home` is the launcher and picks up new apps automatically.
7. To switch app programmatically, call `clawd_request_app(other_app)`
   (declared `extern` in any app .cpp); the switch is applied on the
   next loop iteration.

**Key input contract:**
- Printable ASCII (`0x20`–`0x7E`) arrives via `onKey(char)`.
- `'\n'` for Enter, `'\b'` for Backspace.
- Fn-modified arrow keys are translated to `key::Up/Down/Left/Right`
  (see `core/key.h`).
- Tab is reserved — it always returns to the home launcher and never
  reaches an app's `onKey`.

**A new shared service:**
1. Create `firmware/src/services/<name>.{h,cpp}`.
2. Expose `begin()` / lifecycle. Publish events with a new `EventSource`
   value if apps should react.
3. Start it from `main.cpp::setup()`.

**A new wire-protocol message:**
1. Edit `protocol/WIRE.md` first — it's the contract.
2. Update both `firmware/src/apps/<app>/` and
   `host/Sources/clawd-bridge/Protocol.swift` to match.
3. Build both sides before considering it done.

## Build, flash, run

```bash
# Firmware
cd firmware
pio run -e cardputer                 # build USB env only
pio run -e cardputer -t upload       # flash via USB
pio run -e cardputer-ota -t upload   # flash via WiFi (needs wifi_secrets.h)
pio device monitor                   # serial console at 115200

# Bridge
cd host
swift build
.build/debug/clawd-bridge            # foreground, with stdin REPL
host/install/install.sh              # install as launchd agent
host/install/uninstall.sh
```

`pio run` without `-e` builds both envs; the OTA one will fail without
WiFi creds, which is fine and expected.

There is no automated test suite. End-to-end verification means
flashing firmware and running the bridge against the actual device.

## Conventions

- **C++17** for firmware. Static initializers run before `setup()` —
  that's how app registration works; don't rely on init order between
  TUs.
- Prefer **`std::string`** over Arduino `String` outside the M5 display
  / print API where `String` is unavoidable.
- **Swift 5.9 / macOS 13.** CoreBluetooth runs on the main queue
  (`dispatchMain()` in `main.swift`).
- **No comments explaining *what* code does** — name things well
  instead. Comments are for non-obvious *why*: a hidden constraint, a
  workaround, surprising behavior.
- **Don't scaffold speculatively.** The user wants a small, focused
  platform. Resist creating empty app directories or "future" hooks
  before they're needed.
- **Don't expand buddy.** Buddy's protocol is fixed by Anthropic.

## Quick verification flow when changing wire format

1. Update `protocol/WIRE.md`.
2. Update firmware app + `host/Sources/clawd-bridge/Protocol.swift`.
3. `cd firmware && pio run -e cardputer`
4. `cd host && swift build`
5. Flash, then run bridge. Watch logs in
   `~/Library/Logs/clawd-bridge/` if running as a launchd agent.

## GitOps self-update

Every push to `main` that touches `firmware/**` runs
`.github/workflows/firmware.yml`, which builds and publishes
`firmware.bin` + `version.txt` to the `latest` GitHub release.

On device, `services/updater.cpp`:
- fetches `version.txt` + `firmware.bin` live from normal boot (see
  "Live OTA" below). User-triggered from Settings → "check & install →".
- runs a background FreeRTOS task (`updater_bg`) that polls `version.txt`
  60 s after boot, then every 6 h. When a newer SHA is published, status
  flips to `UpdateAvailable` and the statusbar shows a yellow ↑ — no
  flash happens until the user taps "check & install →".
  Caveat: the bg check is heap-gated (skips if largest free block < 28 KB,
  matching mbedTLS handshake needs). On this hardware that gate is rarely
  open in normal-boot steady-state — the indicator is mostly decorative
  in practice. The manual "check & install →" path always works because
  `installNow()` pauses BLE + canvas first.
- compares the SHA against `CLAWD_BUILD_SHA` (set by
  `firmware/scripts/embed_version.py` from `git rev-parse --short HEAD`)
- streams `firmware.bin` into the inactive OTA partition via HTTPUpdate
  when newer

**TLS works from normal boot.** This codebase builds Arduino as an
ESP-IDF component (`framework = arduino, espidf`) so we can rebuild
mbedTLS from source with `CONFIG_MBEDTLS_DYNAMIC_BUFFER=y` plus tuned
content-len caps (`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=4096`,
`CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048`). With those, the github.com
handshake fits in the fragmented heap of a fully-booted firmware. So
`services/github.cpp` (used by `health.cpp` and the report app) and
LibSSH (used by `apps/ssh`) talk HTTPS/TLS live from normal boot
without ceremony. See `firmware/sdkconfig.defaults` for the full set of
required flags — `AUTOSTART_ARDUINO=y`, `BT_NIMBLE_ENABLED=n` (we use
the NimBLE-Arduino library instead), and `MBEDTLS_KEY_EXCHANGE_PSK=y`
(else `ssl_client.cpp` fails to link) are non-obvious gotchas.

`services/telemetry.cpp` (single NVS slot, last-wins) survives as an
offline fallback for `report.cpp` when a live submit fails — it gets
drained on the next successful direct submit, not by the recovery boot.

**Live OTA from normal boot.** `updater::installNow()` runs HTTPUpdate
in place: pauses BLE + releases the canvas + pauses SD to free
contiguous heap, fetches the manifest, streams `firmware.bin` via
stock `WiFiClientSecure` + HTTPUpdate, and reboots into the new image
on success (single reboot via `rebootOnUpdate(true)`). On failure the
UI is restored and the caller returns — no forced reboot. Status and
last error are persisted to NVS for the Settings UI.

A previous incarnation of OTA used a two-reboot "recovery" dance to
get an unfragmented heap for the github.com TLS handshake. The
IDF-component build with `MBEDTLS_DYNAMIC_BUFFER` removed the need;
the recovery scaffolding (`runRecovery()`, `isRecoveryBoot()`, the
early branch in `main.cpp::setup()`, the NVS `recovery` flag) was
deleted in Phase 6. If live OTA fails on hardware, USB reflash is the
fallback.

**Rollback** is app-level (ESP-IDF's built-in rollback is not enabled
because it requires a framework rebuild):
1. Before flashing, `pending=true` and `boots=0` written to NVS.
2. After reboot, `updater::begin()` increments `boots` if `pending` is
   still set.
3. If `boots > 3` we call `esp_ota_set_boot_partition()` on the
   previous slot and reset — restoring the last-known-good firmware.
4. `updater::tick()` clears `pending` 30 seconds into a healthy run.

**Credentials survive updates.** `services/wifi.cpp` reads SSID/password
from NVS. First-boot migration copies compile-time secrets (from
`wifi_secrets.h`) into NVS so CI-built firmware (which has no secrets
file) keeps the same network on subsequent updates.

**Sealed PAT is per-device.** `firmware/secrets/github_pat.sealed` is
AES-256-GCM encrypted against `identity::sealKey()` — 32 random bytes
generated once per device in NVS namespace `identity`. The committed
sealed file only decrypts on the device whose key it was sealed
against. An NVS wipe (correctly) destroys the device identity, so the
sealed PAT will fail to decrypt afterwards (`[sealed]
gcm_auth_decrypt failed -18`) and both `health.cpp` auto-issue
submission and the report app stop posting to GitHub. (The recovery
boot no longer needs the seal key — `runRecoveryImpl()` only fetches
the manifest and flashes.) To re-seal:

1. Temporarily add `Serial.printf("[identity] SEAL_KEY=%s\n",
   g_sealKeyB64.c_str());` after `g_sealKeyB64.assign(...)` in
   `identity.cpp::loadOrGenerateSealKey()`, flash, capture the base64
   key over serial, revert the print.
2. `export CLAWD_SEAL_KEY="<captured>"; python3 tools/seal-pat.py`
   (reads `tools/github_pat.txt`, writes
   `firmware/secrets/github_pat.sealed`).
3. Rebuild and flash. `scripts/embed_secrets.py` regenerates
   `src/secrets/sealed_blobs.h` from the sealed file on every
   `pio run`, then commit the updated `.sealed` file so CI builds
   carry it too.

Multi-device support would require deriving the seal key from eFuse
MAC (weaker — anyone with the MAC can decrypt) or surrendering the
per-device guarantee. Single-developer setup, so the manual re-seal
on NVS wipe is acceptable.

## Notes on the chat path

The bridge spawns `claude` per turn with:

```
claude -p --output-format stream-json --include-partial-messages [--continue]
```

`StreamJsonParser` reads NDJSON events and emits:
- `text_delta` → `chat.chunk` event to device
- `tool_use` block start → `chat.status` event ("⚙ ToolName")
- final `result` → `chat.end` with output-token count

Subsequent turns pass `--continue` so the conversation context survives
across spawns. The conversation history lives wherever `claude` keeps
it (per cwd), not in clawdputer.

## Persisted state

- BLE bonds in NVS (cleared by `ble::clearBonds()` / the buddy `unpair`
  command).
- `claude` CLI conversation history per `CLAWD_CHAT_CWD` directory.
- Nothing else yet. Future apps should use NVS via a small wrapper
  service when they need persistence.

## Web demo (GitHub Pages)

`web/` ships a browser simulator of the device at
https://chaerem.github.io/clawdputer/. The display is a real 240×135
canvas; apps are reimplemented in JavaScript with the same layouts and
key bindings as the firmware. BLE, WiFi, the bridge daemon, and the
`claude` CLI are stubbed — the simulated chat answers any prompt from a
small keyword KB. All demo state (chat history, SSH hosts added in the
demo, Buddy approve/deny counts, last-selected card) persists to the
browser's `localStorage`.

**Auto-adapting catalog.** `tools/gen-web-manifest.py` parses every
`firmware/src/apps/<name>/<name>.cpp`, extracts the `App xxx_app = { … }`
fields (id, name, description, hidden, keysAsArrows, services), and
writes `web/apps.json`. At startup the JS calls `applyManifest()`:

- JS apps in the manifest get name/description/hidden/keysAsArrows
  overridden from firmware (so renaming an app in the .cpp updates the
  tile automatically).
- Firmware apps without a JS reimplementation get a generic stub tile
  with the name, description, and source path.
- JS apps that no longer exist in firmware are dropped from the launcher.

The pages workflow (`.github/workflows/pages.yml`) regenerates
`apps.json` before every deploy and triggers on
`firmware/src/apps/**`, so a new app in firmware → new stub tile on the
hosted demo on the next push. Run `python3 tools/gen-web-manifest.py`
locally after editing firmware app metadata; the file is committed for
file:// previews.

**PR previews.** `.github/workflows/pr-preview.yml` deploys every PR
touching `web/**` or `firmware/src/apps/**` to
`https://chaerem.github.io/clawdputer/pr-preview/pr-<N>/`, posts a
sticky comment on the PR with the link, and cleans up on close. Both
the main and preview deploys publish to the `gh-pages` branch
(GitHub Pages source is "Deploy from a branch" → `gh-pages` → `/`);
the main deploy uses `keep_files: true` so PR preview subdirectories
survive.

To add a fully-interactive JS reimplementation of a new firmware app:
edit `web/app.js`, define the app the same way the existing
`buddy/chat/ssh/settings` ones are defined, and call `registerApp(...)`.
The manifest overlay will then enrich it with the live firmware
metadata.

## Wokwi simulator

The hosted demo's "Open in Wokwi" badge points at
`https://wokwi.com/github/CHaerem/clawdputer/tree/wokwi-firmware`. The
orphan `wokwi-firmware` branch is force-pushed by the build job in
`.github/workflows/firmware.yml` on every main firmware build and
contains: `firmware/wokwi.bin` (merged bootloader + partition table +
ota_data + app, built from the cardputer-wokwi PIO env),
`firmware/firmware.elf`, `firmware/wokwi.toml`, `firmware/diagram.json`,
and the compiled `cardputer-keyboard` custom chip wasm. Wokwi clones
the branch, finds everything relative to `wokwi.toml`, and boots the
firmware.

**Verified pinout** (from M5GFX + M5Cardputer/IOMatrix.cpp):

| Function          | GPIO(s) |
|-------------------|---------|
| ST7789 MOSI       | 35 |
| ST7789 SCK        | 36 |
| ST7789 DC         | 34 |
| ST7789 CS         | 37 |
| ST7789 RST        | 33 |
| ST7789 BL (PWM)   | 38 |
| Keyboard COL out  | 8, 9, 11 (→ 74HC138 demux) |
| Keyboard ROW in   | 13, 15, 3, 4, 5, 6, 7 (active-low, pull-up) |
| G0 button         | 0 |
| RGB LED (WS2812)  | 21 |
| External I2C SDA/SCL | 1 / 2 |
| microSD CS/SCLK/MISO/MOSI | 40 / 14 / 39 / 12 |

`diagram.json` wires a minimal stand-in for the Cardputer: a custom
`chip-cardputer-keyboard` for the matrix scan plus a G0 push button.
The ST7789 is NOT wired — Wokwi has no StampS3 board, only the
WROOM-1-based DevKitC-1 which hides GPIO 33/34 (the display RST/DC
pins on real Cardputer) as internal flash pins. The firmware still
drives those GPIOs on boot; they just no-op in the simulator. GPIO 45
and 48 are grounded to break M5Unified's PowerHub I2C autodetect that
would otherwise misidentify the board.

The chip's `chip.c` is compiled to `chip.wasm` by
`wokwi/builder-clang-wasm` (see the `Compile cardputer-keyboard
custom chip` step in `.github/workflows/firmware.yml`); `wokwi.toml`'s
`[[chip]] binary = "wokwi-chips/cardputer-keyboard/chip.wasm"`
references the build output. Locally: `cd firmware/wokwi-chips/
cardputer-keyboard && make` (requires Docker). The .wasm is gitignored.

Today the chip is a **boot-only stub** that declares the 10 keyboard
pins and never pulls a row low — the firmware sees "no keys pressed,"
which is enough for the smoke test (launcher renders) but means
**interactive key injection in Wokwi isn't wired**. An earlier
version tried a `keys` text-input attribute (`x,y;x,y;…`), but the
public Wokwi Custom Chips API (`attr_init`/`attr_read` and their
`_float` variants) supports only integer/float attributes and no
attribute-change callbacks — string attrs and `attr_watch` don't
exist. A real key-injection path would either pack the 56-cell
pressed-mask into ≥2 `uint32` attributes (clunky UI, no labels) or
wire individual `wokwi-pushbutton`s into the row pins via the
diagram — the latter is the cleaner long-term option but hasn't been
built yet.

**Variant detection.** M5Unified probes GPIO 5/6/8/9 to pick between
`board_M5Cardputer` and `board_M5CardputerADV`. In the simulator the
ST7789 panel-ID read returns 0 (the panel isn't wired) so autodetect
falls back to PowerHub via I2C 0x50 on GPIO 45/48 — we ground those
pins in `diagram.json` and set `cfg.fallback_board =
board_M5Cardputer` in `main.cpp` so M5Cardputer init still runs.

**Two PIO envs.** `cardputer` is the real-hardware build (used by
firmware.yml for OTA releases). `cardputer-wokwi` adds
`-DCLAWD_WOKWI_BUILD=1` which trips an `#ifdef` in `main.cpp::setup()`
turning off `internal_imu`, `internal_rtc`, `internal_mic`,
`internal_spk`, `clear_display`, `output_power` — peripherals that
hang on I2C/I2S transfers Wokwi doesn't ACK. Real-hardware firmware
keeps the defaults (all on).

**Per-PR firmware previews.** `firmware.yml` builds the simulator
firmware for any PR touching `firmware/**`, force-pushes it to a
`wokwi-firmware-pr-<N>` orphan branch, and posts a sticky comment on
the PR linking to
`https://wokwi.com/github/<owner>/<repo>/tree/wokwi-firmware-pr-<N>`
so reviewers can boot that PR's exact firmware in the browser. The
branch is deleted on PR close/merge. The hosted badge in
`web/index.html` still points at main's `wokwi-firmware` branch — PR
previews are discovered via the PR comment, not the static demo.

**Smoke test as regression net.** Same workflow runs `wokwi-cli`
against the merged image on every firmware-touching PR
(`continue-on-error: true` — the smoke step's result is surfaced in
the sticky PR comment rather than gating merge). The outcome appears
in `steps.smoke.outcome` and is shown in the comment.

## Keeping docs in sync

When pushing changes that affect user-facing behavior, app list, or
architecture, update both:
- `README.md` — user-facing: apps table, status bullets, hardware notes
- `CLAUDE.md` — session hand-off: conventions, adding things, architecture

Neither file needs every implementation detail — `README.md` describes
*what it does*, `CLAUDE.md` describes *how to work on it*.

## When in doubt

- `protocol/WIRE.md` is the firmware ↔ bridge contract.
- `README.md` has the user-facing build instructions.
- Memory files under
  `~/.claude/projects/-Users-christopherhaerem-Privat-Cardputer/memory/`
  capture user preferences and project background — read them before
  making style or scope decisions.
