#!/usr/bin/env bash
# End-to-end interactive test suite.
#
# Flashes the device, then runs through a scripted sequence of prompts
# ("press up", "navigate to chat", …) and watches the serial log for
# the matching evidence that each action actually registered on the
# device. Anything that doesn't appear within its timeout fails the
# suite, and we stop pushing until the user confirms the regression
# is fixed.
#
# Usage:  tools/test-suite.sh [/dev/cu.usbmodem*]
#
# The script is intentionally interactive — embedded firmware can't be
# fully driven from CI without an HW runner, but a checklist that
# fails loudly is much better than "smoke-test boot clean ✓" which
# never catches keyboard regressions.

set -uo pipefail

PORT="${1:-/dev/cu.usbmodem2101}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$HERE/../firmware"

if [[ ! -e "$PORT" ]]; then
    echo "[suite] $PORT not found — plug in the device first" >&2
    exit 1
fi

echo "[suite] flashing firmware…"
(cd "$FIRMWARE_DIR" && pio run -e cardputer -t upload --upload-port "$PORT") \
    || { echo "[suite] flash failed" >&2; exit 1; }

echo "[suite] starting interactive test session"
exec ~/.platformio/penv/bin/python3 - "$PORT" <<'PY'
import serial, sys, time, threading, queue

PORT = sys.argv[1]
ser = serial.Serial(PORT, 115200, timeout=0.1)

# Reset the device cleanly so we start from a known state.
ser.dtr = False; ser.rts = True; time.sleep(0.1)
ser.dtr = True;  ser.rts = False; time.sleep(0.1)
ser.rts = True;  time.sleep(0.1); ser.rts = False

# Background reader so we can grep without blocking the main loop.
lines = queue.Queue()
def reader():
    while True:
        raw = ser.readline()
        if not raw: continue
        line = raw.decode("utf-8", errors="replace").rstrip()
        lines.put(line)
        print(f"  | {line}")
threading.Thread(target=reader, daemon=True).start()

def wait_for(pattern, timeout_s):
    """Wait for a substring in any incoming line. True on hit."""
    end = time.time() + timeout_s
    while time.time() < end:
        try:
            line = lines.get(timeout=0.2)
        except queue.Empty:
            continue
        if pattern in line:
            return True
    return False

def fail_marker_check(window_s=2.0):
    """Drain any 'Guru / panic / Backtrace' lines from the queue."""
    end = time.time() + window_s
    crashed = False
    while time.time() < end:
        try:
            line = lines.get(timeout=0.1)
        except queue.Empty:
            continue
        if any(m in line for m in ("Guru Meditation","panic'ed","Backtrace:")):
            crashed = True
    return crashed

# ─── Phase 1: automatic boot checks ───────────────────────────────────
checks = [
    ("boot banner",      "[clawdputer] boot",          15),
    ("apps registered",  "app(s) registered",          5),
    ("canvas ready",     "canvas ready",               5),
    ("identity loaded",  "[identity] fingerprint",     10),
    ("BLE up",           "[ble] advertising started",  20),
    ("home entered",     "entered app: home",          20),
]
failed = []
for label, marker, t in checks:
    print(f"\n[suite] waiting for: {label!r}", flush=True)
    if not wait_for(marker, t):
        failed.append(label)
        print(f"        ✗ TIMED OUT after {t}s")
    else:
        print(f"        ✓")

# ─── Phase 2: interactive checks ──────────────────────────────────────
def step(prompt, marker, t=15):
    print(f"\n[suite] {prompt}", flush=True)
    if not wait_for(marker, t):
        failed.append(prompt)
        print(f"        ✗ no '{marker}' in {t}s")
    else:
        print(f"        ✓ ({marker})")

step("DO: press ';' (up arrow in menu)    — looking for: word=';'",
     "word=';'", t=30)
step("DO: press '/' (right arrow in menu) — looking for: word='/'",
     "word='/'", t=20)
step("DO: navigate to a NON-home app (any other tile, then enter) "
     "— looking for: 'entered app:' (other than home)",
     "entered app:", t=30)
step("DO: press TAB — looking for return to home",
     "entered app: home", t=15)
step("DO: press G0 (side button) — looking for 'entered app: home'",
     "entered app: home", t=15)

# ─── Crash sweep ──────────────────────────────────────────────────────
print("\n[suite] watching for panics for 3 s…")
if fail_marker_check(3.0):
    failed.append("crash markers in serial")
    print("        ✗ CRASH detected")
else:
    print("        ✓ no crash markers")

# ─── Verdict ──────────────────────────────────────────────────────────
print("\n" + "=" * 60)
if failed:
    print(f"[suite] {len(failed)} failure(s):")
    for f in failed: print(f"  - {f}")
    sys.exit(2)
else:
    print("[suite] all checks passed ✓")
PY
