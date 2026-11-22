#!/usr/bin/env python3
"""Mach-O symbol table reader for the nabu USB console.

Given a kernel.macho, resolve symbol names to kernel virtual addresses so the
console's `read <addr> <len>` verb can inspect any kernel variable. This is the
host-side equivalent of Linux's `/proc/kallsyms` + devmem: no kernel change, no
brush cycle, no extra verb -- just parse the symbol table that is already in
the Mach-O.

Usage:
    python3 -I symtab.py kernel.macho lookup gPhysBase
    python3 -I symtab.py kernel.macho grep native_adreno
    python3 -I symtab.py kernel.macho list | head

The REPLY_RE verb is uppercase; symbol names here are the stripped C names
(leading underscore removed), matching what `nm` shows without the underscore.
"""
import struct
import sys

MH_MAGIC_64 = 0xfeedfacf
LC_SYMTAB = 0x2
N_STAB = 0xe0     # mask for debug symbols to skip
N_TYPE = 0x0e
N_SECT = 0x0e     # defined in a section


class Symtab:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if len(d) < 32:
            raise ValueError("too small for Mach-O 64")
        magic = struct.unpack_from("<I", d, 0)[0]
        if magic != MH_MAGIC_64:
            raise ValueError("not MH_MAGIC_64: %#x" % magic)
        ncmds = struct.unpack_from("<I", d, 16)[0]
        off = 32
        found = None
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from("<II", d, off)
            if cmd == LC_SYMTAB:
                found = struct.unpack_from("<IIII", d, off + 8)
                break
            off += cmdsize
        if not found:
            raise ValueError("no LC_SYMTAB")
        self.symoff, self.nsyms, self.stroff, self.strsize = found
        self._entries = None

    def entries(self):
        if self._entries is not None:
            return self._entries
        d = self.data
        out = []
        for i in range(self.nsyms):
            e = self.symoff + i * 16
            strx = struct.unpack_from("<I", d, e)[0]
            typ = d[e + 4]
            if typ & N_STAB:
                continue
            if (typ & N_TYPE) != N_SECT:
                continue
            value = struct.unpack_from("<Q", d, e + 8)[0]
            if not value:
                continue
            if strx >= self.strsize:
                continue
            end = d.find(b"\0", self.stroff + strx)
            if end < 0:
                continue
            raw = d[self.stroff + strx:end].decode("ascii", "ignore")
            name = raw[1:] if raw.startswith("_") else raw
            if not name:
                continue
            out.append((name, value))
        self._entries = out
        return out

    def lookup(self, name):
        target = name.lstrip("_")
        return [(n, v) for n, v in self.entries() if n == target]

    def grep(self, needle):
        low = needle.lower()
        return [(n, v) for n, v in self.entries() if low in n.lower()]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    path, op = sys.argv[1], sys.argv[2]
    try:
        st = Symtab(path)
    except Exception as e:
        print("symtab: %s" % e, file=sys.stderr)
        return 1
    if op == "lookup":
        for name, value in st.lookup(sys.argv[3]):
            print("%s = 0x%016x" % (name, value))
        if not st.lookup(sys.argv[3]):
            print("not found: %s" % sys.argv[3], file=sys.stderr)
            return 1
        return 0
    if op == "grep":
        for name, value in st.grep(sys.argv[3]):
            print("0x%016x  %s" % (value, name))
        return 0
    if op == "list":
        for name, value in st.entries():
            print("0x%016x  %s" % (value, name))
        return 0
    print("unknown op: %s" % op, file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
