#!/usr/bin/env python3
"""Route A: giulia command names -> our 48-verb console, verified one by one.

Reuses nabu_console.Session, which owns ONE CDC handle and runs a reader thread
that separates SXNU1R replies from the continuous kernel log flood. Do NOT use
sxnu_ctl here: its synchronous read gets drowned by >1MB of log between frames.

Only ONE CDC handle per boot. Stop nabu_console.py before running this tool.
"""
import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nabu_console as nc

# giulia command -> (verb, arg, note). None verb => no device-side equivalent.
MAP = {
    "help":            (None, None, "host-only: this tool's command list"),
    "ping":            ("PING", "", "1:1"),
    "status":          ("STATUS", "", "1:1"),
    "uptime":          ("UPTIME", "", "1:1"),
    "bootargs":        ("BOOTARGS", "", "1:1"),
    "info":            ("VERSION", "", "build info (closest to giulia info)"),
    "display":         ("DISPREGS", "", "display register snapshot (narrower)"),
    "gpu.status":      ("GMUSTATE", "", "GMU prep state (narrower)"),
    "gpu.hfi.status":  ("SMMUDUMP", "", "APPROX: SMMU bank, not HFI queue headers"),
    "screen.snapshot": ("SCREENSHOT", "", "1:1 framebuffer capture"),
    "memory":          ("SYSCTL", "hw.memsize", "APPROX: sysctl, not giulia mem stats"),
    "ps":              ("TASKDUMP", "", "APPROX: raw task words, not decoded ps"),
    "ufs":             ("DUMLIST", "", "APPROX: UFS dump slots, not giulia ufs stats"),
    "usb":             ("PING", "", "APPROX: link health via ping"),
    "replay":          (None, None, "host-only: log analysis (debug_terminal Capture)"),
    "sample":          (None, None, "host-only: GUI sampler needs screen stream"),
    "features":        (None, None, "host-only: derive from this MAP"),
    "shell":           (None, None, "needs userspace daemon + shell + AMFI"),
    "wifi.status":     (None, None, "no wifi verb"),
    "wifi.start":      (None, None, "no wifi verb"),
    "reboot.fastboot": (None, None, "our REBOOT goes to recovery, not fastboot"),
    "reboot.status":   (None, None, "no pending-reboot query verb"),
    "gpu.observe":     (None, None, "gated observation window not implemented"),
    "gpu.hfi.pin":     (None, None, "DMA buffer pin not implemented"),
    "gpu.hfi.query":   (None, None, "HFI query not implemented"),
    "gpu.hfi.poll":    (None, None, "HFI poll not implemented"),
    "gpu.hfi.log":     (None, None, "GMU log buffer read not implemented"),
    "screen.start":    (None, None, "no screen binary stream"),
    "screen.stop":     (None, None, "no screen binary stream"),
    "screen.configure":(None, None, "no screen binary stream"),
    "input.status":    (None, None, "no HID provider"),
    "input.release":   (None, None, "no HID provider"),
    "input.keepalive": (None, None, "no HID provider"),
}

DEVICE_CMDS = [
    "help", "ping", "status", "replay", "sample", "features", "info", "uptime",
    "memory", "ps", "bootargs", "display", "usb", "ufs", "shell",
    "wifi.status", "wifi.start", "reboot.fastboot", "reboot.status",
    "gpu.status", "gpu.observe", "gpu.hfi.status", "gpu.hfi.pin", "gpu.hfi.query",
    "gpu.hfi.poll", "gpu.hfi.log", "screen.snapshot", "screen.start", "screen.stop",
    "screen.configure", "input.status", "input.release", "input.keepalive",
]


def run(port, out_dir, only=None, timeout=20.0):
    raw = str(Path(out_dir) / "route-a-com.bin")
    def log(msg):
        print("[bridge]", msg)

    sess = nc.Session(port, raw, log)
    try:
        # HELLO: session=0, seq=0
        frame = "SXNU1 %016x %08x HELLO" % (0, 0)
        body = frame + " %08x" % (nc.crc32(frame.encode()) if hasattr(nc, "crc32") else _crc(frame.encode()))
        # nabu_console has its own crc32 import; build via its helper if available
        sess.send((body + "\n").encode())
        rep = sess.wait_reply("HELLO", timeout, seq=0)
        if not rep:
            print("HELLO failed; another console holding the port?")
            return 1
        parts = dict(p.split("=", 1) for p in rep.split() if "=" in p)
        sess.session = int(parts.get("session", "0"), 16)
        sess.seq = int(parts.get("last_seq", "0"), 16)
        print("HELLOK session=%016x last_seq=%d" % (sess.session, sess.seq))

        results = {}
        for cmd in DEVICE_CMDS:
            if only and cmd not in only:
                continue
            verb, arg, note = MAP.get(cmd, (None, None, "not in map"))
            if verb is None:
                print("%-18s NOMAP  %s" % (cmd, note))
                results[cmd] = ("NOMAP", note)
                continue
            sess.seq += 1
            frame = "SXNU1 %016x %08x %s" % (sess.session, sess.seq, verb)
            if arg:
                frame += " " + arg
            frame += " %08x" % _crc(frame.encode())
            sess.send((frame + "\n").encode())
            r = sess.wait_reply(verb, timeout, seq=sess.seq)
            if r:
                print("%-18s %-4s   %s" % (cmd, "OK", r[:90]))
                results[cmd] = ("OK", r)
            else:
                print("%-18s ERR    %s" % (cmd, note))
                results[cmd] = ("ERR", note)
            time.sleep(0.1)
        ok = sum(1 for v in results.values() if v[0] == "OK")
        nomap = sum(1 for v in results.values() if v[0] == "NOMAP")
        print("\nsummary: %d OK / %d ERR / %d NOMAP (of %d)" %
              (ok, sum(1 for v in results.values() if v[0] == "ERR"), nomap, len(results)))
        return 0
    finally:
        sess.running = False
        try:
            sess.s.close()
        except Exception:
            pass


def _crc(b):
    import binascii
    return binascii.crc32(b) & 0xFFFFFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="CDC port; omit to wait for VID/PID 1209:316D")
    ap.add_argument("--out", default=r"E:\nabu\logs")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--timeout", type=float, default=20.0)
    a = ap.parse_args()
    if a.dry_run:
        for c in DEVICE_CMDS:
            v, _, n = MAP.get(c, (None, None, "?"))
            print("%-18s -> %-12s %s" % (c, v or "(none)", n))
        return 0
    # Wait for the CDC port to enumerate, polling every 2s (never long sleeps).
    port = a.port
    if not port:
        import serial.tools.list_ports as lp
        deadline = time.time() + 300
        while time.time() < deadline:
            for d in lp.comports():
                if d.vid == 0x1209 and d.pid == 0x316D:
                    port = d.device
                    print("CDC FOUND: " + port)
                    break
            if port:
                break
            time.sleep(2)
        if not port:
            print("CDC never appeared")
            return 1
    return run(port, a.out, set(a.only) if a.only else None, a.timeout)


if __name__ == "__main__":
    sys.exit(main())
