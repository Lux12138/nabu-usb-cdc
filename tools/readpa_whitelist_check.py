#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Offline check of the READPA / DUMPUFS physical-address whitelist.

qud_read_pa_allowed() decides whether a physical address may be read through
ml_io_map(). Getting it wrong is expensive in both directions:

  too permissive -> reading a nonexistent PA wedges the USB worker for good
                    (measured on kb-46: `readpa 0x3c0fc000 32` never returned,
                    port stayed open, HELLO stopped answering, board needed a
                    power cycle)
  too strict     -> the diagnostics that depend on READPA (PTDUMP's acceptance
                    cross-check, RDBLK, page-table probes) stop working

So the rule is pinned here with the real numbers this board reports, and this
script is the regression test. It mirrors the C function; if the C changes, this
must change with it.

Usage:  python -I tools/readpa_whitelist_check.py [--gphys 0xc007c000 --size 0x130000000]
"""
import argparse
import sys

# The MMIO windows WRITEPA trusts (QcomUsbDiag.c qud_write_whitelist).
# Ranges are [lo, hi).
MMIO = [
    (0x0a600000, 0x0a700000, "dwc3"),
    (0x01000000, 0x01010000, "smmu"),
    (0x01050000, 0x01090000, "gcc"),
    (0x02c90000, 0x02ca0000, "gpucc"),
    (0x0ae00000, 0x0af00000, "mdss"),
    (0x0af00000, 0x0af01000, "disp_cc"),
]


def allowed(pa, length, gphys, size):
    """Mirror of qud_read_pa_allowed()."""
    if not length:
        return False
    # RAM first: the common case (page tables, kernel data).
    if size:
        end = gphys + size
        if pa >= gphys and pa <= end and length <= end - pa:
            return True
    # Then the MMIO blocks WRITEPA may touch.
    for lo, hi, _name in MMIO:
        if pa >= lo and pa + length <= hi:
            return True
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gphys", type=lambda s: int(s, 0), default=0xc007c000)
    ap.add_argument("--size", type=lambda s: int(s, 0), default=0x130000000)
    a = ap.parse_args()

    print("RAM window: [0x%x, 0x%x)" % (a.gphys, a.gphys + a.size))
    print()

    cases = [
        # (pa, len, expect, why)
        # --- RAM: must be allowed (PTDUMP reads page tables through this) ---
        (0xcae35000, 8,   True,  "L1 page table page (PTDUMP walk)"),
        (0xcae35600, 8,   True,  "L1 entry index 192"),
        (0xc2fc9108, 16,  True,  "_gPhysBase's page (PTDUMP probe)"),
        (a.gphys,   16,   True,  "first word of RAM"),
        (a.gphys + a.size - 16, 16, True, "last 16 bytes of RAM"),
        # --- RAM: must be refused (off the end, or below the base) ---
        (a.gphys + a.size - 8, 16, False, "straddles the end of RAM"),
        (a.gphys - 8, 16, False, "just below RAM base"),
        # --- MMIO: must be allowed (RDBLK / register probes) ---
        (0x0ae01000, 4,   True,  "MDSS MDP top registers"),
        (0x02c90000, 4,   True,  "GPUCC base"),
        (0x0a600000, 8,   True,  "DWC3 base"),
        # --- MMIO: must be refused (outside every window) ---
        (0x0ae01000, 0x200000, False, "MDSS read running past its window"),
        (0x12340000, 4,   False, "unmapped gap between windows"),
        # --- The address that bricked the board on kb-46 ---
        (0x3c0fc000, 32,  False, "Raspberry Pi panic address from BCM2837.h"),
        (0x9b000000, 32,  False, "bootloader reservation, outside XNU RAM"),
        # --- Degenerate ---
        (0x0, 4,          False, "null page"),
        (a.gphys, 0,      False, "zero length"),
    ]

    failures = 0
    for pa, length, expect, why in cases:
        got = allowed(pa, length, a.gphys, a.size)
        ok = (got == expect)
        print("%s pa=0x%-10x len=0x%-8x expect=%-5s got=%-5s  %s"
              % ("PASS" if ok else "FAIL", pa, length, expect, got, why))
        if not ok:
            failures += 1

    print()
    if failures:
        print("%d FAILURE(S)" % failures)
        return 1
    print("ALL PASS (%d cases)" % len(cases))
    return 0


if __name__ == "__main__":
    sys.exit(main())
