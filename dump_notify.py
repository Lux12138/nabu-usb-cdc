#!/usr/bin/env python3
"""Dump the original vsync sender using the same segment mapping the build uses.

llvm-objdump refuses 0xfffffff005796720 because that address is not covered by
the sections it is willing to disassemble, but the build script reads it fine
through native_prelink.backed_offset.  So resolve the file offset the same way
and print the bytes plus the decoded words, which is enough to see how the
client queue at +5592 is walked.
"""
import sys

sys.path.insert(0, "/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/kits/NativePrelink")

from native_prelink import macho, backed_offset  # noqa: E402

OLD = "/root/merge/out-v6/ipsw/21H461/kernelcache.decompressed"
VA = 0xfffffff005796720
N = 0xA0

blob = open(OLD, "rb").read()
img = macho(blob)
off = backed_offset(img["segments"], VA, N, len(blob))
data = blob[off:off + N]

print(f"file offset = 0x{off:x}  ({N} bytes at 0x{VA:x})")
print()
# Print as instruction words with the raw hex, and a tiny decoder for the few
# encodings that matter here (ldr/str with immediate, add, cbz, bl, mov).
for i in range(0, N, 4):
    w = int.from_bytes(data[i:i+4], "little")
    hint = ""
    top = w >> 24
    if top == 0xF9 and (w & 0x3F) == 0x08:      # ldr/str Xt,[Xn,#imm12]
        imm = ((w >> 10) & 0xFFF) * 8
        op = "ldr" if (w >> 22) & 1 else "str"
        hint = f"{op} x{(w>>0)&31},[x{(w>>5)&31},#{imm:#x}]"
    elif top == 0xB9 and (w & 0x3F) == 0x08:    # ldr/str Wt,[Xn,#imm12]
        imm = ((w >> 10) & 0xFFF) * 4
        op = "ldr" if (w >> 22) & 1 else "str"
        hint = f"{op} w{(w>>0)&31},[x{(w>>5)&31},#{imm:#x}]"
    elif (w & 0xFFC00000) == 0xF9400000:
        hint = f"ldr x{(w>>0)&31},[x{(w>>5)&31},#x{((w>>10)&0xFFF)*8:x}]"
    elif (w & 0xFFC00000) == 0xF9000000:
        hint = f"str x{(w>>0)&31},[x{(w>>5)&31},#x{((w>>10)&0xFFF)*8:x}]"
    elif (w & 0xFFE00C00) == 0xB4000000:
        hint = f"cbz x{(w>>0)&31}, +{(w>>5)&0x7FFFF:#x}*4"
    elif (w & 0xFC000000) == 0x94000000:
        v = w & 0x3FFFFFF
        if v & (1 << 25):
            v -= 1 << 26
        hint = f"bl {VA + i + 4*v:#x}"
    print(f"  {VA+i:016x}: {data[i:i+4].hex()}   {hint}")
