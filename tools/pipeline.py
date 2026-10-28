#!/usr/bin/env python3
"""Unattended round on nabu: XNU -> REBOOT -> TWRP -> flash -> boot -> verify.

Fixed order (2026-10-28, plan A; verified by r10z3m):

  step0  accept any starting state: XNU (CDC), TWRP (adb recovery) or fastboot
  step1  [XNU only] one single-open session: HELLO, STATUS, REBOOT
  step2  [XNU only] board CDC disappears within 10 s
  step3  [XNU only] adb sees `recovery` within 90 s
  step4  adb reboot bootloader -> fastboot within 60 s
  step5  verify payload sha256+size, flash cust, boot boot-merge-r3.img
  step6  board CDC re-enumerates within 60 s (new kernel)
  step7  one single-open session: HELLO+STATUS as soon as the new CDC appears
         (frames must go early -- RX goes dead around boot+70..90 s), then keep
         capturing N s on the same handle (--after N)
  step8  park (default): after the capture, send REBOOT on the same handle so the
         board lands in TWRP again -- the next round then needs no keypress at
         all. `--no-park` keeps the old behaviour (board stays in the new XNU).
  step8  print the five timestamps

Hard rules (learned the hard way):
  * one CDC open per boot cycle -- after close() the same device instance either
    blocks in Serial() or fails with ERROR_GEN_FAILURE(31) until a board reset,
    so every verb a cycle needs must go through a single port-session run;
  * stop at the first timeout and report the step number, never retry;
  * never write misc, never touch WDT, never dd-write cust, never TerminateProcess
    a process holding the CDC (use kill-reader.ps1 and check the process is gone).

Usage:
  python pipeline.py --tag r10z3m --payload-dir E:/nabu/build/merge-r10z3m \
      --expect-payload-sha 7500af56...
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import time

TOOLS = "E:/nabu/tools"
LOGS = "E:/nabu/logs"
PORT_SESSION = os.path.join(TOOLS, "port-session.py")


class Stop(Exception):
    pass


def now():
    return time.strftime("%H:%M:%S")


def logline(fh, msg):
    line = "[%s] %s" % (now(), msg)
    print(line, flush=True)
    fh.write(line + "\n")
    fh.flush()


def find_port():
    sys.path.insert(0, TOOLS)
    import sxnu_ctl
    return sxnu_ctl.find_port()


def adb_state():
    try:
        r = subprocess.run(["adb", "devices"], capture_output=True, text=True,
                           errors="replace", timeout=10)
    except Exception:
        return None
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[1] in ("recovery", "device"):
            return parts[1]
    return None


def fastboot_serial():
    try:
        r = subprocess.run(["fastboot", "devices"], capture_output=True, text=True,
                           errors="replace", timeout=10)
    except Exception:
        return None
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            return parts[0]
    return None


def detect_state():
    if find_port():
        return "xnu"
    if adb_state() == "recovery":
        return "twrp"
    if fastboot_serial():
        return "fastboot"
    return None


def wait_for(pred, timeout, interval, fh, label):
    t0 = time.time()
    while time.time() - t0 < timeout:
        v = pred()
        if v:
            logline(fh, "%s after %.1fs" % (label, time.time() - t0))
            return v
        time.sleep(interval)
    logline(fh, "%s NOT seen within %ds" % (label, timeout))
    return None


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run_tool(cmd, timeout, fh, label):
    logline(fh, "RUN %s" % " ".join(cmd))
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, errors="replace",
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        raise Stop("%s timed out after %ds" % (label, timeout))
    for line in (p.stdout or "").splitlines():
        logline(fh, "  " + line)
    for line in (p.stderr or "").splitlines():
        logline(fh, "  ERR " + line)
    logline(fh, "%s rc=%d in %.1fs" % (label, p.returncode, time.time() - t0))
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--payload-dir", required=True)
    ap.add_argument("--expect-payload-sha", default=None)
    ap.add_argument("--capture", type=int, default=20,
                    help="seconds to sample before the park REBOOT. Keep the REBOOT "
                         "inside boot+60 s: the control RX window closes around "
                         "boot+70 s, so a later frame is silently dropped (r10z3n-park1)")
    ap.add_argument("--payload-bytes", type=int, default=536870912)
    ap.add_argument("--no-park", dest="park", action="store_false",
                    help="do not send the final REBOOT; leave the board in the new XNU")
    ap.add_argument("--full", action="store_true",
                    help="full-boot round: no host-side park REBOOT; sample N s and rely on "
                         "the kernel's nabu-park-after=N timer to reset into TWRP "
                         "(required because control RX dies around boot+70 s)")
    args = ap.parse_args()

    os.makedirs(LOGS, exist_ok=True)
    plog = os.path.join(LOGS, args.tag + "-pipeline.txt")
    fh = open(plog, "a")
    ts = {}

    try:
        # step0: accept XNU, TWRP or fastboot as a starting point
        state = detect_state()
        if state is None:
            logline(fh, "step0: nothing visible yet, re-check in 30 s")
            time.sleep(30)
            state = detect_state()
        if state is None:
            raise Stop("step0: board is in none of XNU / TWRP / fastboot")
        logline(fh, "step0 OK: board state = %s" % state)

        if state == "xnu":
            # step1: the control channel is only usable on this boot cycle
            p = run_tool([sys.executable, PORT_SESSION, "--tag", args.tag + "-ctl",
                          "--confirm", "hello", "status", "reboot"], 120, fh, "step1 control")
            if not re.search(r"^\d{2}:\d{2}:\d{2} TX .* REBOOT", p.stdout, re.M):
                raise Stop("step1: REBOOT frame was not sent")
            m = re.search(r"^(\d{2}:\d{2}:\d{2}) TX .* REBOOT", p.stdout, re.M)
            ts["REBOOT TX"] = m.group(1)

            # step2
            if wait_for(lambda: find_port() is None, 10, 0.5, fh, "CDC gone") is None:
                raise Stop("step2: board CDC still enumerated 10 s after REBOOT")
            ts["CDC gone"] = now()

            # step3
            if wait_for(lambda: adb_state() == "recovery", 90, 2, fh, "adb recovery") is None:
                raise Stop("step3: no adb recovery within 90 s")
            ts["TWRP"] = now()
        elif state == "twrp":
            ts["TWRP"] = now() + " (start)"

        # step4
        if state in ("xnu", "twrp"):
            subprocess.run(["adb", "reboot", "bootloader"], timeout=30,
                           capture_output=True, text=True, errors="replace")
            fb = wait_for(fastboot_serial, 60, 2, fh, "fastboot")
            if not fb:
                raise Stop("step4: no fastboot within 60 s")
        else:
            fb = fastboot_serial()
        ts["fastboot"] = now()

        # step5
        payload = os.path.join(args.payload_dir, "payload.img")
        bootimg = os.path.join(args.payload_dir, "boot-merge-r3.img")
        size = os.path.getsize(payload)
        digest = sha256_file(payload)
        logline(fh, "payload %s  %d B  sha256 %s" % (payload, size, digest))
        if size != args.payload_bytes:
            raise Stop("step5: payload size %d != %d" % (size, args.payload_bytes))
        if args.expect_payload_sha and digest.lower() != args.expect_payload_sha.lower():
            raise Stop("step5: payload sha mismatch")
        p = run_tool(["fastboot", "-s", fb, "flash", "cust", payload], 300, fh, "step5 flash")
        if p.returncode != 0:
            raise Stop("step5: flash rc=%d" % p.returncode)
        p = run_tool(["fastboot", "-s", fb, "boot", bootimg], 60, fh, "step5 boot")
        if p.returncode != 0:
            raise Stop("step5: boot rc=%d" % p.returncode)

        # step6
        port2 = wait_for(find_port, 60, 2, fh, "new kernel CDC")
        if not port2:
            raise Stop("step6: CDC did not come back within 60 s")
        ts["new kernel (CDC back)"] = now()

        # step7 (+ park: leave the board in TWRP for the next round)
        if args.full:
            cmd = [sys.executable, PORT_SESSION, "--tag", args.tag + "-run",
                   "--after", str(args.capture), "hello", "status"]
            budget = args.capture + 120
        elif args.park:
            cmd = [sys.executable, PORT_SESSION, "--tag", args.tag + "-run",
                   "--confirm", "--pause", str(args.capture),
                   "hello", "status", "pause", "reboot"]
            budget = args.capture + 150
        else:
            cmd = [sys.executable, PORT_SESSION, "--tag", args.tag + "-run",
                   "--after", str(args.capture), "hello", "status"]
            budget = args.capture + 90
        p = run_tool(cmd, budget, fh, "step7 verify")
        m = re.search(r"^(\d{2}:\d{2}:\d{2}) RX .* HELLO OK", p.stdout, re.M)
        if not m:
            raise Stop("step7: no SXNU1R for HELLO (rc=%d)" % p.returncode)
        ts["verify HELLO"] = m.group(1)
        if not args.park and not args.full and p.returncode != 0:
            raise Stop("step7: verify session rc=%d" % p.returncode)

        # step8: park handshake -- port may die before the REBOOT reply, so the
        # only criterion is COM gone + adb back in recovery
        if args.full:
            if wait_for(lambda: adb_state() == "recovery", args.capture + 120, 2, fh,
                        "auto-reset TWRP") is None:
                raise Stop("step8: board did not reach TWRP; is nabu-park-after set in the payload?")
            ts["auto-reset TWRP"] = now()
        elif args.park:
            if wait_for(lambda: adb_state() == "recovery", 90, 2, fh, "park TWRP") is None:
                raise Stop("step8: board did not reach TWRP after park reboot")
            ts["park TWRP"] = now()
    except Stop as e:
        logline(fh, "STOP: %s" % e)
        logline(fh, "timestamps so far: %r" % ts)
        logline(fh, "pipeline log -> %s" % plog)
        fh.close()
        return 2
    except Exception as e:
        logline(fh, "STOP (unexpected): %r" % e)
        fh.close()
        return 2

    logline(fh, "ROUND OK")
    for k in ("REBOOT TX", "CDC gone", "TWRP", "fastboot", "new kernel (CDC back)",
              "verify HELLO", "park TWRP", "auto-reset TWRP"):
        logline(fh, "  %-24s %s" % (k, ts.get(k, "?")))
    logline(fh, "logs: %s/{%s-ctl,%s-run}-com.bin  pipeline %s"
            % (LOGS, args.tag, args.tag, plog))
    fh.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
