#!/usr/bin/env python3
"""Generic decoder for any VA in the correct candidate image.

Reuses the instruction decoder from dump_notify3.py but takes the VA on the
command line.  Needed because llvm-objdump refuses these VAs on the candidate
macho; native_prelink.backed_offset is the only mapping that works.
"""
import sys

sys.path.insert(0, "/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/kits/NativePrelink")
from native_prelink import macho, backed_offset  # noqa: E402

CAND = "/root/merge/l738-device/build/xnu-prelink/native-owner-mobile-graphics-candidate.macho"

def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v

def decode(w, pc):
    if (w & 0x3B000000) == 0x28000000:
        opc = (w >> 30) & 3
        L = (w >> 22) & 1
        imm = sx((w >> 15) & 0x7F, 7) * (8 if opc == 2 else 4)
        rt, rn, rt2 = (w & 31), ((w >> 5) & 31), ((w >> 10) & 31)
        pre = "ldp" if L else "stp"
        base = "x" if opc != 0 else "w"
        return f"{pre} {base}{rt},{base}{rt2},[x{rn},#{imm}]"
    for mask, val, size in ((0xFFC00000, 0xF9400000, 8), (0xFFC00000, 0xF9000000, 8),
                            (0xFFC00000, 0xB9400000, 4), (0xFFC00000, 0xB9000000, 4)):
        if (w & mask) == val:
            L = (w >> 22) & 1
            rt, rn = w & 31, (w >> 5) & 31
            imm = ((w >> 10) & 0xFFF) * size
            return f"{'ldr' if L else 'str'} {'x' if size==8 else 'w'}{rt},[x{rn},#{imm}]"
    if (w & 0x7F000000) in (0x11000000, 0x51000000, 0x91000000, 0xD1000000):
        sub = (w >> 30) & 1
        sz = "x" if (w >> 31) else "w"
        imm = (w >> 10) & 0xFFF
        if (w >> 22) & 1:
            imm <<= 12
        return f"{'sub' if sub else 'add'} {sz}{(w>>0)&31},{sz}{(w>>5)&31},#{imm}"
    if (w & 0x7FE0FC00) in (0x8B000000, 0xCB000000):
        return f"{'sub' if (w>>30)&1 else 'add'} x{(w>>0)&31},x{(w>>5)&31},x{(w>>16)&31}"
    if (w & 0x7E000000) == 0x34000000:
        return f"{'cbnz' if (w>>24)&1 else 'cbz'} x{(w>>0)&31},{pc+4*sx((w>>5)&0x7FFFF,19):#x}"
    if (w & 0xFF000010) == 0x54000000:
        return f"b.cond {pc+4*sx((w>>5)&0x7FFFF,19):#x}"
    if (w & 0x7C000000) == 0x14000000:
        return f"{'bl' if (w>>31)&1 else 'b '} {pc+4*sx(w&0x3FFFFFF,26):#x}"
    if (w & 0xFFFFFC1F) == 0xD61F0000:
        return f"br x{(w>>5)&31}"
    if (w & 0xFFFFFC1F) == 0xD63F0000:
        return f"blr x{(w>>5)&31}"
    if (w & 0x7F800000) == 0x52800000:
        return f"movz x{(w>>0)&31},#{((w>>5)&0xFFFF)<<(((w>>21)&3)*16):#x}"
    if (w & 0x7F800000) == 0x72A00000:
        return f"movk x{(w>>0)&31},#{((w>>5)&0xFFFF)<<(((w>>21)&3)*16):#x}"
    if (w & 0xFFFFFC1F) == 0xAA0003E0:
        return f"mov x{(w>>0)&31},x{(w>>16)&31}"
    if (w == 0xD65F03C0):
        return "ret"
    return ""

if __name__ == "__main__":
    va = int(sys.argv[1], 0)
    n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x100
    blob = open(CAND, "rb").read()
    img = macho(blob)
    data = blob[backed_offset(img["segments"], va, n, len(blob)):][:n]
    for i in range(0, n, 4):
        w = int.from_bytes(data[i:i+4], "little")
        print(f"{va+i:016x}: {data[i:i+4].hex()}  {decode(w, va+i)}")
