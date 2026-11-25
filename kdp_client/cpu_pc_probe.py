"""Confirm or refute the "thread trapped in backend_guard" diagnosis.

Context (MEMORY §188-190): GPU submission held ~235/s while validateSwap
(selector 296) stopped entirely after the first minute of boot.  Seven
unimplemented IOMobileFramebuffer selectors (211, 245, 246, 306, 307, 338, 345)
are routed to native_legacy_backend_guard, which -- before the l191b build --
spun on `wfe` forever.  A userland thread that called one parked there with no
log line, no counter and no error.

This tool asks the kernel directly: is any CPU's PC inside that 8-byte window?

Usage (needs the device in KDP, i.e. `kdp` sent on the console first):
    python cpu_pc_probe.py

Exit codes: 0 = answer obtained, 2 = could not reach the kernel.
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kdp_proto  # noqa: E402
import kdp_udp    # noqa: E402

# The trap lives at this address in both builds; before l191b the two words
# were `wfe; b .`, after l191b they record and return.
GUARD = (0xfffffff007a84644, 0xfffffff007a8464c)
CPUS = 8


def probe(pc_lo: int, pc_hi: int) -> int:
    transport = kdp_udp.open_udp(host="127.0.0.1", port=41139, timeout=0.2)
    sess = kdp_proto.KdpSession(transport, timeout=3, retries=4,
                                connect_timeout=10)
    try:
        sess.connect()
    except Exception as exc:  # noqa: BLE001 - report and fail cleanly
        print(f"connect failed: {exc}", file=sys.stderr)
        print("(is the device in KDP? send `kdp` on the console first)",
              file=sys.stderr)
        return 2

    hits: list[int] = []
    seen = 0
    for cpu in range(CPUS):
        try:
            regs = sess.readregs(cpu=cpu)
        except Exception as exc:  # noqa: BLE001
            print(f"cpu{cpu}: read failed ({exc})")
            continue
        pc = regs.get("pc")
        if pc is None:
            print(f"cpu{cpu}: no pc in thread state")
            continue
        seen += 1
        mark = ""
        if pc_lo <= pc < pc_hi:
            mark = "   <-- TRAPPED IN backend_guard"
            hits.append(cpu)
        print(f"cpu{cpu}: pc=0x{pc:016x}{mark}")

    print()
    if seen == 0:
        print("could not read any CPU state", file=sys.stderr)
        return 2

    if hits:
        print(f"CONFIRMED: cpu(s) {hits} parked in backend_guard "
              f"(0x{pc_lo:x}..0x{pc_hi:x})")
        print("=> an unimplemented IOMobileFramebuffer selector was called and "
              "the calling thread is gone.")
        print("=> the l191b build fixes this by returning kIOReturnUnsupported.")
    else:
        print(f"no CPU in 0x{pc_lo:x}..0x{pc_hi:x} (probed {seen} cpus)")
        print("=> either this build already returns from the guard, or the "
              "trapped thread was not scheduled on a probed CPU at this instant.")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Is any CPU parked in native_legacy_backend_guard?")
    ap.add_argument("--lo", type=lambda s: int(s, 0), default=GUARD[0],
                    help="window low address (default: the guard)")
    ap.add_argument("--hi", type=lambda s: int(s, 0), default=GUARD[1],
                    help="window high address (exclusive)")
    args = ap.parse_args()
    return probe(args.lo, args.hi)


if __name__ == "__main__":
    sys.exit(main())
