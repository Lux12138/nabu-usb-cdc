#!/usr/bin/env python3
"""Decode the vsync client queue walk in native_legacy_vsync_notify.

Focus on the loop after `ldr x8,[x8,#0x15d8]` (0xfffffff005796830): how the
queue node is dereferenced, what offsets are read for the callback and refcon,
and how the "next" pointer is formed.  That is the layout a hand-built client
must match.
"""
import sys

sys.path.insert(0, "/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/kits/NativePrelink")
from native_prelink import macho, backed_offset  # noqa: E402

CAND = "/root/merge/l738-device/build/xnu-prelink/native-owner-mobile-graphics-candidate.macho"
VA = 0xfffffff005796720
N = 0x180

blob = open(CAND, "rb").read()
img = macho(blob)
data = blob[backed_offset(img["segments"], VA, N, len(blob)):][:N]

def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

def decode(w, pc):
    # ldp/stp pair (signed imm7 scaled)
    if (w & 0x3B000000) == 0x28000000:       # ldp/stp
        opc = (w >> 30) & 3
        V = (w >> 26) & 1
        L = (w >> 22) & 1
        imm = sx((w >> 15) & 0x7F, 7) * (8 if opc == 2 else 4)
        rt, rn, rt2 = (w & 31), ((w >> 5) & 31), ((w >> 10) & 31)
        name = "ldp" if L else "stp"
        if V:
            name += " (fp)"
        regs = (f"q{rt},q{rt2}" if opc == 3 else f"x{rt},x{rt2}") if not V else f"d{rt},d{rt2}"
        return f"{name} {regs},[x{rn},#{imm}]"
    # ldr/str unsigned immediate
    if (w & 0xFFC00000) in (0xF9400000, 0xF9000000, 0xB9400000, 0xB9000000):
        size = 8 if (w & 0xFFC00000) >= 0xF0000000 else 4
        L = (w >> 22) & 1
        rt, rn = w & 31, (w >> 5) & 31
        imm = ((w >> 10) & 0xFFF) * size
        return f"{'ldr' if L else 'str'} {'x' if size==8 else 'w'}{rt},[x{rn},#{imm}]"
    # ldr/str register offset
    if (w & 0xFFE00C00) in (0xF8600800, 0xF8200800, 0xB8600800, 0xB8200800):
        return f"ldr/str reg-offset w{(w>>0)&31} x{(w>>5)&31} x{(w>>16)&31}"
    # add/sub immediate
    if (w & 0x7F000000) in (0x11000000, 0x51000000, 0x91000000, 0xD1000000):
        sub = (w >> 30) & 1
        sz = "x" if (w >> 31) else "w"
        imm = (w >> 10) & 0xFFF
        sh = (w >> 22) & 1
        if sh: imm <<= 12
        return f"{'sub' if sub else 'add'} {sz}{(w>>0)&31},{sz}{(w>>5)&31},#{imm}"
    # add/sub register
    if (w & 0x7FE0FC00) in (0x8B000000, 0xCB000000):
        return f"{'sub' if (w>>30)&1 else 'add'} x{(w>>0)&31},x{(w>>5)&31},x{(w>>16)&31}"
    # cbz / cbnz
    if (w & 0x7E000000) == 0x34000000:
        nz = (w >> 24) & 1
        return f"{'cbnz' if nz else 'cbz'} x{(w>>0)&31},{pc + 4*sx((w>>5)&0x7FFFF,19):#x}"
    # b.cond
    if (w & 0xFF000010) == 0x54000000:
        return f"b.cond {pc + 4*sx((w>>5)&0x7FFFF,19):#x}"
    # b / bl
    if (w & 0x7C000000) == 0x14000000:
        link = (w >> 31) & 1
        return f"{'bl' if link else 'b '} {pc + 4*sx(w & 0x3FFFFFF,26):#x}"
    # br / blr
    if (w & 0xFFFFFC1F) == 0xD61F0000:
        return f"br x{(w>>5)&31}"
    if (w & 0xFFFFFC1F) == 0xD63F0000:
        return f"blr x{(w>>5)&31}"
    # movz/movk
    if (w & 0x7F800000) == 0x52800000:
        return f"movz x{(w>>0)&31},#{((w>>5)&0xFFFF)<<(((w>>21)&3)*16):#x}"
    if (w & 0x7F800000) == 0x72A00000:
        return f"movk x{(w>>0)&31},#{((w>>5)&0xFFFF)<<(((w>>21)&3)*16):#x}"
    # mov register
    if (w & 0xFFFFFC1F) == 0xAA0003E0:
        return f"mov x{(w>>0)&31},x{(w>>16)&31}"
    if w == 0xD65F03C0:
        return "ret"
    return ""

for i in range(0, N, 4):
    w = int.from_bytes(data[i:i+4], "little")
    print(f"{VA+i:016x}: {data[i:i+4].hex()}  {decode(w, VA+i)}")
