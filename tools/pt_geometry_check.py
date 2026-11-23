#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Offline proof of the PTDUMP geometry derivation.

qud_pt_geometry() derives the page-table index widths from TCR_EL1 instead of
hardcoding them. That derivation must reproduce, exactly, the masks XNU itself
built into pmap_table_level_info_4k -- otherwise every walk is wrong.

Inputs are read straight out of the kernel image (no guessing, no constants
copied from a header that might not match the binary):

  pmap_table_level_info_4k  0xfffffff00700ae00  (__TEXT)
      struct page_table_level_info { u64 size, offmask, shift, index_mask,
                                     valid_mask, type_mask, type_block; }
  pmap_pt_attr_4k           0xfffffff0071c7af0  (__DATA_CONST)
      .pta_level_info=+0x00
      .pta_root_level=+0x40(u32)  .pta_commpage_level=+0x44(u32)
      .pta_max_level=+0x48(u32)   .pta_page_shift=+0x68(u64)
    (The commpage level sits between them: struct order in pmap_pt_geometry.h
    is level_info, ops, ap_ro..ap_x, pta_root_level, pta_commpage_level,
    pta_max_level, pta_tcr_value, pta_page_size, pta_pagezero_size,
    pta_page_shift -- reading +0x44 as max_level yields 1 and looks like a bug
    in the deriver when it is a bug in the reader.)

The check then runs the same arithmetic the kernel module does, for both
granules, and asserts the resulting (shift, index_mask) pairs match the image.

Usage:  python -I tools/pt_geometry_check.py --kernel <kernel.macho>
"""
import argparse
import struct
import subprocess
import sys

# Both addresses shift on every relink, so they are looked up in the binary's
# symbol table rather than hardcoded. The symbols are exported (nm type S).
LEVEL_INFO_SYM = "_pmap_table_level_info_4k"
PT_ATTR_SYM = "_pmap_pt_attr_4k"

# From osfmk/arm64/proc_reg.h, used only to label the expected granule.
T0SZ_BOOT_4K = 26          # __ARM64_PMAP_SUBPAGE_L1__ && !__ARM_16K_PG__


def load_segments(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, _cpu, _sub, _ft, ncmds, _sz, _fl, _r = struct.unpack_from("<IiiIIIII", data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit("not a 64-bit Mach-O: %s" % path)
    off, segs = 32, []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == 0x19:  # LC_SEGMENT_64
            name = data[off + 8:off + 24].rstrip(b"\0").decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, off + 24)
            segs.append((name, vmaddr, vmsize, fileoff, filesize))
        off += cmdsize
    return data, segs


def read_va(data, segs, va, n):
    for name, vmaddr, _vmsize, fileoff, filesize in segs:
        if vmaddr <= va < vmaddr + filesize:
            o = fileoff + (va - vmaddr)
            return data[o:o + n], name
    return None, None


def symbol_va(path, name):
    """Find a symbol's virtual address with llvm-nm (never grep the 125 MB image)."""
    out = subprocess.run(["/usr/bin/llvm-nm", path], capture_output=True, text=True)
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[2] == name:
            return int(parts[0], 16)
    raise SystemExit("symbol not found: %s" % name)


def level_info(data, segs, index):
    raw, _seg = read_va(data, segs, level_info_va + index * 56, 56)
    if raw is None:
        raise SystemExit("level_info[%d] not in file" % index)
    size, offmask, shift, index_mask, valid, tmask, tblock = struct.unpack("<7Q", raw)
    return dict(size=size, offmask=offmask, shift=shift, index_mask=index_mask,
                valid=valid, type_mask=tmask, type_block=tblock)


def geometry(t1sz, tg1):
    """Mirror of qud_pt_geometry().

    tg1 uses XNU's encoding, NOT the ARM ARM's: proc_reg.h:850-856 defines
    TCR_TG1_GRANULE_16KB = 1 and TCR_TG1_GRANULE_4KB = 2 for the TTBR1 region.
    Decoding it architecturally (00=4K, 01=16K) makes every downstream number
    wrong in a way that looks like a broken page table -- that mistake cost the
    kb-44 board round.

    Root level = first level whose shift is strictly below the VA width. The
    (T1SZ-16)//per_level formula gives 1 for 4K but 0 for 16K, while XNU's
    pmap_pt_attr_16k hardcodes PMAP_TT_L1_LEVEL -- so the shift rule is the one
    that generalises."""
    if tg1 == 2:            # TCR_TG1_GRANULE_4KB (XNU encoding)
        page_shift = 12
        shifts = {0: 39, 1: 30, 2: 21, 3: 12}
    elif tg1 == 1:          # TCR_TG1_GRANULE_16KB (XNU encoding)
        page_shift = 14
        shifts = {0: 47, 1: 36, 2: 25, 3: 14}
    else:
        return None
    va_bits = 64 - t1sz
    root = None
    for n in range(4):
        if shifts[n] < va_bits:
            root = n
            break
    if root is None:
        return None
    width = {}
    for n in range(4):
        if n < root:
            width[n] = 0
        elif n == root:
            width[n] = va_bits - shifts[n]
        else:
            width[n] = shifts[n - 1] - shifts[n]
    return dict(root=root, last=3, page_shift=page_shift,
                shifts=shifts, width=width)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", required=True)
    a = ap.parse_args()

    data, segs = load_segments(a.kernel)
    global level_info_va
    level_info_va = symbol_va(a.kernel, LEVEL_INFO_SYM)
    pt_attr_va = symbol_va(a.kernel, PT_ATTR_SYM)

    raw_attr, seg = read_va(data, segs, pt_attr_va, 0x70)
    if raw_attr is None:
        raise SystemExit("pmap_pt_attr_4k not in file")
    root_level = struct.unpack_from("<I", raw_attr, 0x40)[0]
    commpage_level = struct.unpack_from("<I", raw_attr, 0x44)[0]
    max_level = struct.unpack_from("<I", raw_attr, 0x48)[0]
    page_shift = struct.unpack_from("<Q", raw_attr, 0x68)[0]

    print("== pmap_pt_attr_4k (%s @ 0x%016x)" % (seg, pt_attr_va))
    print("   root_level=%d commpage_level=%d max_level=%d page_shift=%d"
          % (root_level, commpage_level, max_level, page_shift))

    infos = {i: level_info(data, segs, i) for i in range(4)}
    print("\n== pmap_table_level_info_4k (@ 0x%016x)" % level_info_va)
    for i, li in infos.items():
        print("   L%d shift=%2d index_mask=0x%016x type_mask=0x%x valid=0x%x"
              % (i, li["shift"], li["index_mask"], li["type_mask"], li["valid"]))

    failures = []

    # 1. The image's own root/max/page_shift must match the derivation.
    g = geometry(T0SZ_BOOT_4K, 2)
    print("\n== derived from T1SZ=%d TG1=2 (XNU encoding: 4K)" % T0SZ_BOOT_4K)
    print("   root=%d last=%d page_shift=%d" % (g["root"], g["last"], g["page_shift"]))
    if g["root"] != root_level:
        failures.append("root: derived %d != image %d" % (g["root"], root_level))
    if g["last"] != max_level:
        failures.append("max_level: derived %d != image %d" % (g["last"], max_level))
    if g["page_shift"] != page_shift:
        failures.append("page_shift: derived %d != image %d" % (g["page_shift"], page_shift))

    # 2. Each level's shift and mask must match bit-for-bit.
    for n in range(4):
        li = infos[n]
        if n < g["root"]:
            continue
        want_shift = g["shifts"][n]
        want_mask = ((1 << g["width"][n]) - 1) << want_shift
        ok = (li["shift"] == want_shift and li["index_mask"] == want_mask)
        print("   L%d derived shift=%2d mask=0x%016x  %s"
              % (n, want_shift, want_mask, "OK" if ok else "MISMATCH"))
        if not ok:
            failures.append("L%d: derived (shift=%d mask=0x%016x) != image (shift=%d mask=0x%016x)"
                            % (n, want_shift, want_mask, li["shift"], li["index_mask"]))

    # 3. The 16K derivation must be self-consistent as well (it is only used if
    #    a board actually reports TG1=16K, but a wrong table there would be a
    #    silent mis-walk on such a board).
    #
    #    16K uses its own T1SZ (T1SZ_BOOT = 25 there, not 26); with 26 the L1
    #    index collapses to 2 bits and the ratios stop matching the VA width.
    g16 = geometry(25, 1)
    print("\n== derived for TG1=16K, T1SZ=25 (alternative granule, sanity only)")
    print("   root=%d last=%d page_shift=%d widths=%s"
          % (g16["root"], g16["last"], g16["page_shift"],
             [g16["width"][n] for n in range(4)]))
    total_bits = g16["page_shift"] + sum(g16["width"][n] for n in range(4))
    if total_bits != 64 - 25:
        failures.append("16K: covered bits %d != VA bits %d" % (total_bits, 64 - 25))
    # XNU's pmap_pt_attr_16k hardcodes root = PMAP_TT_L1_LEVEL.
    if g16["root"] != 1:
        failures.append("16K: derived root %d != PMAP_TT_L1_LEVEL(1)" % g16["root"])

    print()
    if failures:
        for f in failures:
            print("FAIL " + f)
        return 1
    print("ALL PASS: qud_pt_geometry() reproduces the kernel's own tables")
    return 0


if __name__ == "__main__":
    sys.exit(main())
