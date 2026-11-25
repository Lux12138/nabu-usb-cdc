#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""hp.py -- hot patch helper for the nabu XNU kernel.

Why this exists
---------------
Cold-iterating (build -> flash cust/misc/boot -> wait for the desktop) costs
minutes per experiment.  KDP can write kernel text directly, and the kernel's
``kdp_machine_vm_write`` runs ``flush_dcache64`` + ``invalidate_icache64`` after
every fragment (verified in kdpr27 at 0xfffffff007487f2c / 0xfffffff007487f3c,
see MEMORY §179).  So a patched instruction executes immediately.

Safety rules baked in
---------------------
* Read-before-write.  ``kdp_machine_vm_read`` does *no* cache maintenance, so a
  read after the write may return stale bytes.  The original must be captured
  first, and that capture is written to a rollback file.
* Fragments never exceed MAX_KDP_DATA_SIZE (1024), matching the kernel's
  ``cmp w8, #0x401`` guard in ``_kdp_writemem64``.
* Dry-run is the default for anything that writes: you must pass --go.

Usage
-----
  hp.py sym <name>                      # resolve a symbol to an address
  hp.py read <addr> <len>               # read bytes (hex + ascii)
  hp.py dis <addr> [n]                  # disassemble n instructions
  hp.py nop <addr> [count]              # replace with NOP (default 1)
  hp.py poke <addr> <hexbytes>          # write raw bytes
  hp.py restore <rollback-file>         # put the original bytes back
  hp.py diff <name> --new <hexbytes>    # show what would change (no write)

Every writing verb needs --go.  Without it the tool reads the original, shows
the plan, and exits.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
KDP_CLIENT = os.path.join(HERE, "kdp_client")
DEFAULT_KERNEL = "/mnt/e/nabu/hold/kdpr27/kernel.macho"
ROLLBACK_DIR = "/mnt/e/nabu/state/rollback"

# llvm-nm / llvm-objdump only exist inside WSL, while the serial port only
# exists on Windows.  So the binary tools go through `wsl`, and the KDP client
# runs under Windows python.  Keep those two facts in one place.
WSL = r"C:\Windows\System32\wsl.exe"


def llvm(tool: str, args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run([WSL, tool] + args, capture_output=True, text=True,
                          timeout=120)

# AArch64 NOP is the canonical "do nothing" instruction.
A64_NOP = 0xD503201F
A64_NOP_BYTES = struct.pack("<I", A64_NOP)


def kdp(args: list[str], transport: str = "udp") -> subprocess.CompletedProcess:
    """Invoke kdp_cli.py with the right interpreter for the transport.

    UDP goes to the mux's bridge on 127.0.0.1:41139, which only exists on the
    Windows side -- so Windows python is required.  Serial can run either way.
    """
    if transport == "udp":
        exe = r"C:\Program Files\Python312\python.exe"
    else:
        exe = sys.executable
    cmd = [exe, os.path.join(KDP_CLIENT, "kdp_cli.py"),
           "--transport", transport]
    if transport == "udp":
        cmd += ["--host", "127.0.0.1", "--udp-port", "41139"]
    cmd += args
    return subprocess.run(cmd, capture_output=True, text=True, timeout=120)


def parse_int(s: str) -> int:
    s = s.strip()
    if s.lower().startswith("0x"):
        return int(s, 16)
    return int(s, 0)


def resolve_symbol(name: str, kernel: str) -> int:
    """llvm-nm lookup.  Targeted, never a whole-tree search (J1/J2).

    C++ names are mangled, so an exact miss falls back to substring matching
    and prints every candidate -- guessing which overload is meant is how you
    patch the wrong function.
    """
    out = llvm("llvm-nm", [kernel])
    if out.returncode != 0:
        raise SystemExit(f"llvm-nm failed: {out.stderr.strip()}")
    want = {"_" + name, name}
    exact = []
    fuzzy = []
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        sym = parts[2]
        try:
            addr = int(parts[0], 16)
        except ValueError:
            continue
        if sym in want:
            exact.append((sym, addr))
        elif name in sym:
            fuzzy.append((sym, addr))
    if exact:
        sym, addr = exact[0]
        if len(exact) > 1:
            print(f"warning: {len(exact)} exact matches, using the first")
        if sym != name:
            print(f"resolved {name} -> {sym}")
        return addr
    if fuzzy:
        print(f"{name} is mangled; candidates:")
        for sym, addr in fuzzy[:10]:
            print(f"  0x{addr:016x}  {sym}")
        if len(fuzzy) == 1:
            print(f"using the only match")
            return fuzzy[0][1]
        raise SystemExit("ambiguous: pass the mangled name, or use `sym <full>`")
    raise SystemExit(f"symbol not found: {name}")


def read_bytes(addr: int, length: int, transport: str) -> bytes:
    r = kdp(["readmem", hex(addr), str(length), "--format", "hex",
             "--fresh"], transport)
    if r.returncode != 0:
        raise SystemExit(f"readmem failed:\n{r.stdout}\n{r.stderr}")
    hexpart = "".join(r.stdout.split())
    # kdp_cli prints "0xADDR: deadbeef" style lines in hex mode; keep hex chars.
    cleaned = "".join(c for c in hexpart if c in "0123456789abcdefABCDEF")
    data = bytes.fromhex(cleaned)
    if len(data) < length:
        raise SystemExit(f"short read: got {len(data)} of {length} bytes")
    return data[:length]


def write_bytes(addr: int, data: bytes, transport: str) -> None:
    r = kdp(["writemem", hex(addr), "--hexdata", data.hex(), "--fresh"],
            transport)
    if r.returncode != 0:
        raise SystemExit(f"writemem failed:\n{r.stdout}\n{r.stderr}")
    print(r.stdout.strip())


def save_rollback(addr: int, original: bytes, label: str) -> str:
    os.makedirs(ROLLBACK_DIR, exist_ok=True)
    path = os.path.join(ROLLBACK_DIR, f"{label}-{addr:016x}.bin")
    with open(path, "wb") as f:
        f.write(struct.pack("<Q", addr) + original)
    return path


def describe(data: bytes) -> str:
    words = []
    for i in range(0, len(data) - 3, 4):
        words.append(f"{struct.unpack_from('<I', data, i)[0]:08x}")
    return " ".join(words)


def disassemble(addr: int, count: int, kernel: str) -> str:
    end = addr + 4 * count
    out = llvm("llvm-objdump",
               ["-d", f"--start-address={hex(addr)}",
                f"--stop-address={hex(end)}", kernel])
    return out.stdout


# --------------------------------------------------------------------------
# verbs
# --------------------------------------------------------------------------

def cmd_sym(args) -> int:
    addr = resolve_symbol(args.name, args.kernel)
    print(f"{args.name} = 0x{addr:016x}")
    return 0


def cmd_read(args) -> int:
    addr, length = parse_int(args.addr), parse_int(args.len)
    data = read_bytes(addr, length, args.transport)
    for off in range(0, len(data), 16):
        chunk = data[off:off + 16]
        hx = " ".join(f"{b:02x}" for b in chunk)
        asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        print(f"{addr + off:016x}  {hx:<47}  {asc}")
    print(f"\nwords: {describe(data)}")
    return 0


def cmd_dis(args) -> int:
    addr = parse_int(args.addr)
    print(disassemble(addr, args.count, args.kernel))
    return 0


def _apply(addr: int, new: bytes, label: str, go: bool, transport: str) -> int:
    print(f"target  : 0x{addr:016x}  ({len(new)} byte(s), label={label})")
    original = read_bytes(addr, len(new), transport)
    print(f"original: {original.hex()}")
    print(f"new     : {new.hex()}")
    if original == new:
        print("(identical -- nothing to do)")
        return 0
    path = save_rollback(addr, original, label)
    print(f"rollback: {path}")
    if not go:
        print("\n(dry run -- re-run with --go to write)")
        return 0
    write_bytes(addr, new, transport)
    print("written (kernel flushed D-cache and invalidated I-cache)")
    return 0


def cmd_nop(args) -> int:
    addr = parse_int(args.addr)
    return _apply(addr, A64_NOP_BYTES * args.count, args.label,
                  args.go, args.transport)


def cmd_poke(args) -> int:
    addr = parse_int(args.addr)
    new = bytes.fromhex(args.hexbytes.replace(" ", "").replace("_", ""))
    return _apply(addr, new, args.label, args.go, args.transport)


def cmd_restore(args) -> int:
    with open(args.rollback, "rb") as f:
        blob = f.read()
    addr = struct.unpack_from("<Q", blob, 0)[0]
    original = blob[8:]
    print(f"restoring {len(original)} byte(s) to 0x{addr:016x} from {args.rollback}")
    if not args.go:
        print(f"original: {original.hex()}\n(dry run -- re-run with --go)")
        return 0
    write_bytes(addr, original, args.transport)
    print("restored")
    return 0


def cmd_diff(args) -> int:
    addr = resolve_symbol(args.name, args.kernel) + args.offset
    new = bytes.fromhex(args.new.replace(" ", "").replace("_", ""))
    original = read_bytes(addr, len(new), args.transport)
    print(f"{args.name}+0x{args.offset:x} = 0x{addr:016x}")
    print(f"original: {original.hex()}")
    print(f"proposed: {new.hex()}")
    if original == new:
        print("identical")
        return 0
    changed = [i for i in range(len(new)) if original[i] != new[i]]
    print(f"{len(changed)} byte(s) differ: {changed}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="hp.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--kernel", default=DEFAULT_KERNEL)
    p.add_argument("--transport", choices=("udp", "serial"), default="udp")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("sym"); s.add_argument("name")

    s = sub.add_parser("read")
    s.add_argument("addr"); s.add_argument("len")

    s = sub.add_parser("dis")
    s.add_argument("addr"); s.add_argument("count", nargs="?", type=int, default=12)

    s = sub.add_parser("nop")
    s.add_argument("addr"); s.add_argument("count", nargs="?", type=int, default=1)
    s.add_argument("--label", default="nop"); s.add_argument("--go", action="store_true")

    s = sub.add_parser("poke")
    s.add_argument("addr"); s.add_argument("hexbytes")
    s.add_argument("--label", default="poke"); s.add_argument("--go", action="store_true")

    s = sub.add_parser("restore")
    s.add_argument("rollback"); s.add_argument("--go", action="store_true")

    s = sub.add_parser("diff")
    s.add_argument("name"); s.add_argument("--new", required=True)
    s.add_argument("--offset", type=lambda x: parse_int(x), default=0)
    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    handlers = {
        "sym": cmd_sym, "read": cmd_read, "dis": cmd_dis,
        "nop": cmd_nop, "poke": cmd_poke, "restore": cmd_restore,
        "diff": cmd_diff,
    }
    return handlers[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
