#!/usr/bin/env python3
"""Single-process CDC session: one COM handle for both log capture and SXNU1 frames.

The port is opened once. A reader thread polls in_waiting (never blocks in read,
so the process stays killable and the device never sees an abandoned read),
appends every byte to <tag>-com.bin and feeds a line splitter. SXNU1R replies are
handed to the main thread through a queue; everything else is just log.

    python port-session.py --tag r10z3d --capture 300          # pure capture
    python port-session.py --tag r10z3d --delay 180 hello status
    python port-session.py --tag r10z3d --confirm reboot

HARD RULE (2026-10-28, r10z3c + r10z3k): one CDC open per boot cycle. After
close(), reopening the same device instance either blocks inside serial.Serial()
or fails with ERROR_GEN_FAILURE(31); only a board reset restores it. Put every
verb a round needs into a single run (e.g. --confirm hello status reboot), never
close-then-reopen, never run a second process against the same port.

Exit codes: 0 ok, 2 no reply, 3 port absent/unavailable, 4 USB lost mid-flight.
"""
import argparse
import os
import queue
import sys
import threading
import time

import serial

import sxnu_ctl


class Session:
    def __init__(self, port, out_path):
        self.port = port
        self.s = serial.Serial(port, 115200, timeout=0.05, write_timeout=2.0)
        self.out = open(out_path, "ab")
        self.replies = queue.Queue()
        self.partial = b""
        self.running = True
        self.alive = True
        self.total = 0
        self.lost_at = None
        self.lost_why = ""
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()

    def _reader(self):
        while self.running:
            try:
                n = self.s.in_waiting
            except Exception as e:
                self.lost_why = "in_waiting: %s" % e
                break
            if not n:
                time.sleep(0.02)
                continue
            try:
                chunk = self.s.read(n)
            except Exception as e:
                self.lost_why = "read: %s" % e
                break
            if not chunk:
                continue
            self.out.write(chunk)
            self.out.flush()
            self.total += len(chunk)
            buf = self.partial + chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip(b"\r")
                if line:
                    if line.startswith(b"SXNU1R"):
                        self.replies.put(line)
            self.partial = buf[-512:]
        if self.running:
            self.alive = False
            self.lost_at = time.strftime("%H:%M:%S")
            print("USB LOST at %s (%s), total %d B" % (self.lost_at, self.lost_why, self.total), flush=True)

    def capture(self, secs, label):
        t0 = time.time()
        before = self.total
        while time.time() - t0 < secs:
            if not self.alive:
                print("STOP %s: USB lost at %s after %d s, %d new bytes (total %d)"
                      % (label, self.lost_at, int(time.time() - t0), self.total - before, self.total), flush=True)
                return False
            time.sleep(0.5)
        print("CAPTURE-OK %s: %d s survived, %d new bytes, total %d B"
              % (label, int(secs), self.total - before, self.total), flush=True)
        return True

    def send(self, frame):
        self.s.write(frame)
        self.s.flush()

    def wait_reply(self, timeout):
        try:
            return self.replies.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        self.running = False
        try:
            self.t.join(timeout=1.0)
        except Exception:
            pass
        try:
            self.s.close()
        except Exception:
            pass
        try:
            self.out.close()
        except Exception:
            pass


def frame(session, seq, verb):
    body = "SXNU1 %016x %08x %s" % (session, seq, verb)
    return ("%s %08x\n" % (body, sxnu_ctl.crc32(body.encode()))).encode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="default: auto-detect VID_1209&PID_316D")
    ap.add_argument("--tag", default="session")
    ap.add_argument("--confirm", action="store_true")
    ap.add_argument("--delay", type=int, default=0, help="pure capture seconds before the first frame")
    ap.add_argument("--capture", type=int, default=0, help="pure capture only for N seconds, no frames")
    ap.add_argument("--after", type=int, default=0, help="keep capturing N seconds after the last verb")
    ap.add_argument("--pause", type=int, default=0,
                    help="seconds to capture when the pseudo-verb 'pause' appears in the verb list")
    ap.add_argument("verbs", nargs="*")
    args = ap.parse_args()
    verbs = [v for v in args.verbs if v]
    bad = [v for v in verbs if v.lower() not in ("hello", "pause") + sxnu_ctl.VERBS]
    if bad:
        sys.exit("unknown verb(s): %s" % ", ".join(bad))
    if "reboot" in verbs and not args.confirm:
        sys.exit("reboot needs --confirm")
    if not verbs and not args.capture:
        sys.exit("nothing to do: give verbs, or --capture N")

    port = args.port or sxnu_ctl.find_port()
    if not port:
        print("PORT ABSENT: find_port()=None (COM1 is the PC UART, board CDC not enumerated)", flush=True)
        return 3
    out = "E:/nabu/logs/%s-com.bin" % args.tag
    if args.delay or args.capture:
        try:
            size = os.path.getsize(out)
        except OSError:
            size = 0
        if size:
            print("note: %s already has %d B, appending" % (out, size), flush=True)
    try:
        dev = Session(port, out)
    except Exception as e:
        print("PORT UNAVAILABLE %s: %s (find_port()=%r)" % (port, e, sxnu_ctl.find_port()), flush=True)
        return 3
    print("open %s -> %s" % (port, out), flush=True)

    ok = True
    try:
        if args.delay:
            if not dev.capture(args.delay, "delay"):
                return 4
        if args.capture:
            if not dev.capture(args.capture, "capture"):
                return 4
        session, seq = 0, 0
        for verb in verbs:
            verb = verb.upper()
            if verb == "PAUSE":
                if args.pause <= 0:
                    print("pause: --pause is 0, skipping", flush=True)
                    continue
                if not dev.capture(args.pause, "pause"):
                    return 4
                continue
            if verb == "HELLO":
                session, seq = 0, 0
            else:
                seq += 1
            f = frame(session, seq, verb)
            print("%s TX %s" % (time.strftime("%H:%M:%S"), f.decode().rstrip()), flush=True)
            dev.send(f)
            rep = dev.wait_reply(10.0 if verb != "REBOOT" else 3.0)
            if rep is None:
                print("STOP at %s: no SXNU1R within timeout (log %d B)" % (verb, dev.total), flush=True)
                ok = False
                return 2
            print("%s RX %s" % (time.strftime("%H:%M:%S"), rep.decode(errors="replace")), flush=True)
            fields = rep.split()
            if len(fields) >= 3:
                try:
                    session = int(fields[1], 16)
                    seq = int(fields[2], 16)
                except ValueError:
                    pass
            if verb == "REBOOT":
                print("reboot acknowledged; watching port for 20 s", flush=True)
                dev.capture(20, "post-reboot")
        if args.after:
            if not dev.capture(args.after, "after"):
                return 4
    finally:
        dev.close()
        print("closed, log %d B" % dev.total, flush=True)
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
