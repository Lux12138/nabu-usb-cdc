#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Read-only CDC throughput meter for the nabu log/KDP port (pipe1).

Measures bytes/s of the kernel log stream on the second CDC interface.
No kernel changes, no writes to the device: just open the port (DTR=1 so
the device starts sending) and count bytes for a fixed window.

Usage:
  python cdc_rate.py COM6 30            # 30 s sample
  python cdc_rate.py COM6 30 --json     # machine-readable
  python cdc_rate.py COM6 30 -o out.bin # also save the raw stream
"""
import argparse, json, sys, time

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("port", help="COM port to read (log/KDP pipe, e.g. COM6)")
    ap.add_argument("seconds", type=float, nargs="?", default=30.0)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("-o", "--out", help="save raw bytes here")
    args = ap.parse_args()
    try:
        import serial
    except ImportError as exc:
        print("pyserial unavailable: %s" % exc, file=sys.stderr)
        return 2
    try:
        s = serial.Serial(args.port, 115200, timeout=0.5)
    except Exception as exc:
        print("cannot open %s: %s" % (args.port, exc), file=sys.stderr)
        return 2
    if hasattr(s, "reset_input_buffer"):
        s.reset_input_buffer()
    n = 0
    t0 = time.time()
    chunks = []
    last_report = t0
    f = open(args.out, "wb") if args.out else None
    try:
        while True:
            now = time.time()
            if now - t0 >= args.seconds:
                break
            b = s.read(8192)
            if b:
                n += len(b)
                if f: f.write(b)
            if now - last_report >= 10.0:
                el = now - t0
                print("[%5.1fs] %d bytes  %.1f KiB/s" % (el, n, n / el / 1024.0),
                      file=sys.stderr, flush=True)
                last_report = now
    finally:
        s.close()
        if f: f.close()
    el = time.time() - t0
    rate = n / el
    result = {
        "port": args.port, "seconds": round(el, 3), "bytes": n,
        "bytes_per_s": round(rate, 1), "KiB_per_s": round(rate / 1024.0, 2),
        "MiB_per_s": round(rate / 1048576.0, 4),
    }
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print("%s: %d bytes in %.2f s = %.1f B/s = %.2f KiB/s = %.4f MiB/s" %
              (args.port, n, el, rate, rate / 1024.0, rate / 1048576.0))
    return 0

if __name__ == "__main__":
    sys.exit(main())
