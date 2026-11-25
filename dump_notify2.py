#!/usr/bin/env python3
"""Disassemble native_legacy_vsync_notify from the SAME candidate image the
build uses.

Last attempt read kernelcache.decompressed and got string data, because
build-gop-legacy-display.py's read() actually uses
  build/xnu-prelink/native-owner-mobile-graphics-candidate.macho
via native_prelink.macho + backed_offset.  Use that exact file so the VA maps
to real machine code.

The first 16 bytes are known to be ff0302d1 fd7b07a9 fdc30191 a0831ff8
(= sub sp,#0xc0; stp ...; stp ...; add x0,...), which is the sanity check.
"""
import sys

sys.path.insert(0, "/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/kits/NativePrelink")

from native_prelink import macho, backed_offset  # noqa: E402

CAND = "/root/merge/l738-device/build/xnu-prelink/native-owner-mobile-graphics-candidate.maho"
CAND = CAND.replace(".maho", ".macho")
VA = 0xfffffff005796720
N = 0x140

blob = open(CAND, "rb").read()
img = macho(blob)
off = backed_offset(img["segments"], VA, N, len(blob))
data = blob[off:off + N]

print(f"candidate = {CAND}")
print(f"file offset = 0x{off:x}  ({N} bytes at 0x{VA:x})")
print(f"first16 = {data[:16].hex()}  (expect ff0302d1fd7b07a9fdc30191a0831ff8)")
print()

# Minimal decoder: ldr/str immediate, add/sub, cbz/cbnz, b, bl, movz, ret.
def decode(w, pc):
    if (w & 0xFFC00000) == 0xF9400000:
        return f"ldr x{(w>>0)&31},[x{(w>>5)&31},#x{((w>>10)&0xFFF)*8:x}]"
    if (w & 0xFFC00000) == 0xF9000000:
        return f"str x{(w>>0)&31},[x{(w>>5)&31},#x{((w>>10)&0xFFF)*8:x}]"
    if (w & 0xFFC00000) == 0xB9400000:
        return f"ldr w{(w>>0)&31},[x{(w>>5)&31},#x{((w>>10)&0xFFF)*4:x}]"
    if (w & 0xFFC00000) == 0xB9000000:
        return f"str w{(w>>0)&31},[x{(w>>5)&31},#x{((w>>10)&0xFFF)*4:x}]"
    if (w & 0xFFE00C00) == 0xB4000000:
        op = "cbz" if (w >> 24) & 1 == 0 else "cbnz"
        v = (w >> 5) & 0x7FFFF
        return f"{op} x{(w>>0)&31}, {pc+4*v:#x}"
    if (w & 0xFC000000) == 0x94000000:
        v = w & 0x3FFFFFF
        if v & (1 << 25):
            v -= 1 << 26
        return f"bl {pc + 4*v:#x}"
    if (w & 0xFC000000) == 0x14000000:
        v = w & 0x3FFFFFF
        if v & (1 << 25):
            v -= 1 << 26
        return f"b  {pc + 4*v:#x}"
    if (w & 0xFFC00000) == 0xD2800000:
        return f"movz x{(w>>0)&31},#{((w>>5)&0xFFFF)<<(((w>>21)&3)*16):#x}"
    if w == 0xD65F03C0:
        return "ret"
    return ""

for i in range(0, N, 4):
    w = int.from_bytes(data[i:i+4], "little")
    print(f"  {VA+i:016x}: {data[i:i+4].hex()}   {decode(w, VA+i)}")
