#!/usr/bin/env python3
"""Plot battery telemetry from the Cardputer's serial output.

Run on the Mac while the device is plugged in:

    python3 tools/battery-plot.py /dev/cu.usbmodem* [duration_minutes]

The script tails the serial port, parses every `[batterylog] …` line the
battery app emits once per minute, and writes a PNG to /tmp on exit
(ctrl-c or after the optional duration). The on-device app keeps its
own rolling 1-hour ring; this is for longer captures and side-by-side
comparison of feature toggles.

Dependencies: pyserial + matplotlib (`pip install pyserial matplotlib`).
"""

from __future__ import annotations

import re
import sys
import time
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import serial
except ImportError as exc:
    print(f"missing dependency: {exc}", file=sys.stderr)
    print("pip install pyserial matplotlib", file=sys.stderr)
    sys.exit(1)


LINE_RE = re.compile(
    r"\[batterylog\] t=(?P<t>\d+) pct=(?P<pct>\d+) mV=(?P<mV>\d+) chg=(?P<chg>\d+)"
)


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 1
    port = sys.argv[1]
    duration_min = int(sys.argv[2]) if len(sys.argv) > 2 else None

    ser = serial.Serial(port, 115200, timeout=1.0)
    print(f"[battery-plot] listening on {port}; ctrl-c to stop")

    ts:    list[float] = []
    pct:   list[int]   = []
    mv:    list[int]   = []
    chg:   list[int]   = []
    t_end = time.time() + duration_min * 60 if duration_min else None

    try:
        while True:
            if t_end and time.time() > t_end:
                break
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip()
            m = LINE_RE.search(line)
            if not m:
                continue
            ts.append(int(m["t"]))
            pct.append(int(m["pct"]))
            mv.append(int(m["mV"]))
            chg.append(int(m["chg"]))
            print(f"  t={ts[-1]}s pct={pct[-1]}% mV={mv[-1]} chg={chg[-1]}")
    except KeyboardInterrupt:
        pass

    if not ts:
        print("[battery-plot] no samples captured", file=sys.stderr)
        return 1

    # Slope from first to last sample, in % per hour
    hours = (ts[-1] - ts[0]) / 3600.0
    drain = (pct[0] - pct[-1]) / hours if hours > 0 else 0.0
    print(f"\n[battery-plot] {len(ts)} samples over {hours*60:.1f} min → "
          f"{drain:.1f} %/hr ({pct[0]}→{pct[-1]}%)")

    out = Path("/tmp") / f"battery-{int(time.time())}.png"
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 6), sharex=True)
    ax1.plot([(t - ts[0]) / 60.0 for t in ts], pct, "g-", label="percent")
    ax1.set_ylabel("Battery %")
    ax1.set_ylim(0, 105)
    ax1.legend(loc="upper right")
    ax2.plot([(t - ts[0]) / 60.0 for t in ts], mv, "b-", label="mV")
    ax2.set_ylabel("mV")
    ax2.set_xlabel("Elapsed (minutes)")
    ax2.legend(loc="upper right")
    fig.suptitle(f"Cardputer battery — {drain:.1f} %/hr drain")
    fig.tight_layout()
    fig.savefig(out, dpi=120)
    print(f"[battery-plot] wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
