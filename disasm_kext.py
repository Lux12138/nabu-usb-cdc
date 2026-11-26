#!/usr/bin/env python3
"""Disassemble a VA inside the IOMobileGraphicsFamily kext using its Mach-O
segment mapping. The kext is stripped, so llvm-objdump -d with --start-address
does nothing; backed_offset resolves VA -> file offset.
"""
import struct
import sys
sys.path.insert(0, "/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/kits/NativePrelink")
from native_prelink import macho, backed_offset  # noqa: E402

KEXT = "/mnt/d/ipsw/21H461/drivers/kexts/com.apple.iokit.IOMobileGraphicsFamily.mach"

def sx(v, b):
    return v - (1 << b) if v & (1 << (b - 1)) else v

def dec(w, pc):
    if (w & 0x7F000000) in (0x11000000, 0x51000000, 0x91000000, 0xD1000000):
        sub = (w >> 30) & 1; sz = "x" if (w >> 31) else "w"
        imm = (w >> 10) & 0xFFF
        if (w >> 22) & 1: imm <<= 12
        return f"{'sub' if sub else 'add'} {sz}{(w>>0)&31},{sz}{(w>>5)&31},#{imm}"
    if (w & 0xFFC00000) in (0xF9400000, 0xF9000000, 0xB9400000, 0xB9000000):
        sz = 8 if (w & 0xFFC00000) >= 0xF0000000 else 4
        L = (w >> 22) & 1
        return f"{'ldr' if L else 'str'} {'x' if sz==8 else 'w'}{w&31},[x{(w>>5)&31},#{((w>>10)&0xFFF)*sz}]"
    if (w & 0x7E000000) == 0x34000000:
        return f"{'cbnz' if (w>>24)&1 else 'cbz'} x{w&31},{pc+4*sx((w>>5)&0x7FFFF,19):#x}"
    if (w & 0xFC000000) == 0x94000000:
        return f"bl {pc+4*sx(w&0x3FFFFFF,26):#x}"
    if (w & 0xFC000000) == 0x14000000:
        return f"b  {pc+4*sx(w&0x3FFFFFF,26):#x}"
    if (w & 0xFFFFFC1F) == 0xD63F0000: return f"blr x{(w>>5)&31}"
    if w == 0xD65F03C0: return "ret"
    return ""

if __name__ == "__main__":
    va = int(sys.argv[1], 0); n = int(sys.argv[2], 0) if len(sys.argv)>2 else 0x80
    blob = open(KEXT, "rb").read()
    img = macho(blob)
    at = backed_offset(img["segments"], va, n, len(blob))
    print(f"VA {va:#x} -> file {at:#x}")
    d = blob[at:at+n]
    for i in range(0, n, 4):
        w = struct.unpack_from("<I", d, i)[0]
        print(f"{va+i:016x}: {d[i:i+4].hex()}  {dec(w, va+i)}")
