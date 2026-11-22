#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Regenerate the payload's unixnu.toml FROM board.toml, so the packed image can
never drift away from the build source again.

Order of truth (highest first):
  /root/merge/l738-device/board.toml   <- the source we maintain
  <hold>/payload.img  ::/EFI/UniXNU/unixnu.toml   <- what actually boots

`unixnu payload --dry-run` prints the exact "kernel args:" line it would put in
the toml. We parse that line and splice it into a copy of an existing toml,
keeping every other field (size/loader/title/dram/trust cache) byte-identical.
That is why this is safe: only the kernel_args array changes.

Usage: python sync_payload_args.py <hold-dir>
"""
import re, subprocess, sys, tempfile, os

DEVICE = "/root/merge/l738-device"
OUT = "/root/merge/out-v6"
UNIXNU = "/root/fresh/UniXNU-latest"

hold = sys.argv[1] if len(sys.argv) > 1 else "/mnt/e/nabu/hold/kb-19-kdpconn"
img = os.path.join(hold, "payload.img")

if not os.path.exists(img):
    sys.exit("missing " + img)

# 1) ask unixnu for the authoritative arg list
r = subprocess.run(
    [sys.executable, "-I", "unixnu", "payload", "--out", OUT, "-D", DEVICE, "--dry-run"],
    cwd=UNIXNU, capture_output=True, text=True, timeout=600)
line = ""
for l in r.stdout.splitlines():
    if l.startswith("== kernel args:"):
        line = l
        break
if not line:
    sys.exit("dry-run did not print kernel args\n" + r.stdout[-1500:] + r.stderr[-800:])
args = line.split("== kernel args:", 1)[1].strip().split()
print("source args: %d entries, debug=%s" % (
    len(args), next((a for a in args if a.startswith("debug=")), "?")))

# 2) pull the current toml out of the image (streamed)
cur = subprocess.run(["mtype", "-i", img, "::/EFI/UniXNU/unixnu.toml"],
                     capture_output=True)
if cur.returncode:
    sys.exit("mtype failed: " + cur.stderr.decode("utf-8", "replace")[:400])
toml = cur.stdout.decode("utf-8", "replace")

# 3) replace only the args array
m = re.search(r'args\s*=\s*"(.*?)"', toml, re.S)
if not m:
    sys.exit("no args=\"...\" line in the packed toml")
new_args = " ".join(args)
toml2 = toml[:m.start(1)] + new_args + toml[m.end(1):]
if toml2 == toml:
    print("toml already in sync")
    sys.exit(0)

# 4) write back once
tmp = tempfile.NamedTemporaryFile("w", suffix=".toml", delete=False, encoding="utf-8")
tmp.write(toml2)
tmp.close()
w = subprocess.run(["mcopy", "-o", "-i", img, tmp.name, "::/EFI/UniXNU/unixnu.toml"],
                   capture_output=True)
os.unlink(tmp.name)
if w.returncode:
    sys.exit("mcopy failed: " + w.stderr.decode("utf-8", "replace")[:400])

# 5) verify from the stream
back = subprocess.run(["mtype", "-i", img, "::/EFI/UniXNU/unixnu.toml"],
                      capture_output=True).stdout.decode("utf-8", "replace")
ok = new_args in back
print("verify: args match =", ok)
print("  debug=0x14e:", "debug=0x14e" in back,
      " cdc-rx:", "nabu-cdc-rx=1" in back,
      " usb-write:", "qcom-usb-write=1" in back)
print("payload sha16:", subprocess.check_output(
    "sha256sum %s | cut -c1-16" % img, shell=True, text=True).strip())
sys.exit(0 if ok else 1)
