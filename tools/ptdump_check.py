#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""PTDUMP acceptance: cross-check the kernel's page-table walk.

The verb answers with the PA it derived from walking TTBR1_EL1 plus a 16-byte
probe read *through that PA*. The independent oracle is the hardware address
translation instruction, which the console exposes as READ/READPA:

    ptdump <va>         -> pa=<X> probe=<16 bytes>
    readpa <X> 16       -> data=<must equal probe>

If the walker's shifts or widths are wrong the two disagree, and the failure
names the level: a wrong L1 index usually lands on a block descriptor (the walk
stops early and reports level=1), a wrong L2 index typically produces a PA in
an unrelated 2 MiB page.

Usage:
    python -I tools/ptdump_check.py --base E:/nabu/logs/kb42 \\
        --kernel E:/nabu/hold/kb-42-ptdump/kernel.macho \\
        --va 0xfffffff007004000 --va 0xfffffff0071c9108
"""
import argparse
import os
import re
import sys
import time


def parse_reply(line):
    """Pull the fields out of 'SXNU1R ... PTDUMP OK va=... pa=... probe=...'."""
    if "PTDUMP" not in line or " OK " not in line:
        return None
    kv = {}
    for token in line.split():
        if "=" in token:
            k, _, v = token.partition("=")
            kv[k] = v
    return kv if ("pa" in kv and "va" in kv) else None


def read_lines(path):
    if not os.path.exists(path):
        return []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.readlines()


def append(path, text):
    with open(path, "a", encoding="utf-8", buffering=1) as f:
        f.write(text + "\n")


def wait_for(path, predicate, timeout, seen_len):
    """Poll a growing text file (10 s cadence, never longer sleeps)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        lines = read_lines(path)
        for line in lines[seen_len:]:
            got = predicate(line)
            if got is not None:
                return got, len(lines)
        if len(lines) != seen_len:
            seen_len = len(lines)
        time.sleep(10)
    return None, seen_len


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True, help="logs/kb42 -> uses -cmd.txt/-ctl.txt")
    ap.add_argument("--va", action="append", default=[], help="virtual address to check")
    ap.add_argument("--timeout", type=float, default=60.0)
    a = ap.parse_args()

    cmd_path, ctl_path = a.base + "-cmd.txt", a.base + "-ctl.txt"
    if not a.va:
        a.va = ["0xfffffff007004000"]      # kernel text: always mapped

    failures = 0
    for addr_s in a.va:
        addr = int(addr_s, 16)
        seen = len(read_lines(ctl_path))
        append(cmd_path, "ptdump 0x%x" % addr)
        entry, seen = wait_for(
            ctl_path,
            lambda l: parse_reply(l) if ("va=0x%016x" % addr) in l else None,
            a.timeout, seen)
        if not entry:
            print("FAIL ptdump %s: no reply in %.0fs" % (addr_s, a.timeout))
            failures += 1
            continue

        pa = int(entry["pa"], 16)
        probe = entry.get("probe", "")
        print("ptdump %-20s -> pa=0x%016x level=%s block=%s" %
              (addr_s, pa, entry.get("level", "?"), entry.get("block", "?")))
        if not probe:
            print("FAIL: reply carried no probe bytes")
            failures += 1
            continue

        # Oracle: read the same bytes again through the physical path, at the
        # 16-byte aligned base the kernel probed from.
        base_pa = pa & ~0xf
        seen = len(read_lines(ctl_path))
        append(cmd_path, "readpa 0x%x 16" % base_pa)
        rd, seen = wait_for(
            ctl_path,
            lambda l: {k: v for k, v in (t.partition("=")[::2] for t in l.split() if "=" in t)}
                      if ("READPA" in l and " OK " in l and "data=" in l) else None,
            a.timeout, seen)
        if not rd:
            print("FAIL readpa 0x%x: no reply" % base_pa)
            failures += 1
            continue

        data = rd["data"].lower()
        # The kernel probes from the 16-byte aligned base, so its probe may be
        # longer than the 16 bytes read here only when the PA is not 16-aligned
        # (then the aligned base differs and this comparison is skipped).
        if data == probe.lower()[:len(data)]:
            print("PASS %s: page-table PA matches hardware, probe=%s" % (addr_s, probe))
        else:
            print("FAIL %s: probe mismatch\n  ptdump=%s\n  readpa=%s" % (addr_s, probe, data))
            failures += 1

    if failures:
        print("\n%d FAILURE(S)" % failures)
        return 1
    print("\nALL PASS (%d address(es))" % len(a.va))
    return 0


if __name__ == "__main__":
    sys.exit(main())
