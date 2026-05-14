#!/usr/bin/env bash
# Flash firmware over USB, then monitor serial for crash markers for 30 s.
# Exits non-zero if the device is still alive but reports a panic. Used
# before pushing to main so a known-broken build doesn't enter GitOps.
#
# Usage: tools/smoke-test.sh [/dev/cu.usbmodem*]

set -uo pipefail

PORT="${1:-/dev/cu.usbmodem2101}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$HERE/../firmware"

if [[ ! -e "$PORT" ]]; then
    echo "[smoke] $PORT not found — plug in the device first" >&2
    exit 1
fi

echo "[smoke] flashing $FIRMWARE_DIR ..."
( cd "$FIRMWARE_DIR" && pio run -e cardputer -t upload --upload-port "$PORT" ) \
    || { echo "[smoke] flash failed" >&2; exit 1; }

echo "[smoke] monitoring serial for 30 s. press ctrl-c to abort."
~/.platformio/penv/bin/python3 - "$PORT" <<'PY'
import serial, sys, time
PORT = sys.argv[1]
# Only flag *crash* markers — `rst:0x` shows up on normal USB-CDC reset too.
markers = (
    "Guru Meditation",
    "abort()",
    "Backtrace:",
    "CORRUPT HEAP",
    "Assert failed",
    "panic'ed",
    "esp_panic",
    "LoadProhibited", "StoreProhibited", "LoadStoreError",
    "IllegalInstruction", "InstrProhibited",
    "Watchdog",
)
ser = serial.Serial(PORT, 115200, timeout=0.3)
# Toggle the ESP32-S3 reset line so we see boot output from the start.
ser.dtr = False; ser.rts = True; time.sleep(0.1)
ser.dtr = True;  ser.rts = False; time.sleep(0.1)
ser.rts = True;  time.sleep(0.1); ser.rts = False

duration = int(sys.argv[2]) if len(sys.argv) > 2 else 30
end = time.time() + duration
crashed = False
boot_seen = False
while time.time() < end:
    raw = ser.readline()
    if not raw:
        continue
    line = raw.decode("utf-8", errors="replace").rstrip()
    print(line)
    if "[clawdputer] boot" in line:
        boot_seen = True
    if any(m in line for m in markers):
        crashed = True

if crashed:
    print("\n[smoke] !!! crash markers detected — DO NOT PUSH !!!", file=sys.stderr)
    sys.exit(2)
if not boot_seen:
    print("\n[smoke] never saw a clean boot banner — check the device", file=sys.stderr)
    sys.exit(3)
print("\n[smoke] no crash markers in 30 s — boot clean ✓")
PY
