# Sample every CPU's PC and flag the ones sitting in backend_guard.
#
# The guard is fffffff007a84644..0x76a8: a `wfe` and a branch back to it.  A CPU
# whose PC lands in that 8-byte window is a thread that called an unimplemented
# IOMobileFramebuffer selector and will never come back.  Finding even one there
# confirms the diagnosis without patching anything.
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))

import kdp_proto, kdp_udp

GUARD_LO, GUARD_HI = 0xfffffff007a84644, 0xfffffff007a8464c
CPUS = 8


def main() -> int:
    transport = kdp_udp.open_udp(host="127.0.0.1", port=41139, timeout=0.2)
    sess = kdp_proto.KdpSession(transport, timeout=3, retries=4, connect_timeout=10)
    try:
        sess.connect()
    except Exception as exc:
        print(f"connect failed: {exc}", file=sys.stderr)
        return 2

    hits = []
    for cpu in range(CPUS):
        try:
            regs = sess.readregs(cpu=cpu)
        except Exception as exc:
            print(f"cpu{cpu}: read failed ({exc})")
            continue
        pc = regs.get("pc")
        if pc is None:
            print(f"cpu{cpu}: no pc")
            continue
        flag = ""
        if GUARD_LO <= pc < GUARD_HI:
            flag = "   <-- STUCK IN backend_guard"
            hits.append(cpu)
        print(f"cpu{cpu}: pc=0x{pc:016x}{flag}")

    print()
    if hits:
        print(f"CONFIRMED: cpu(s) {hits} parked in the unimplemented-selector trap")
    else:
        print("no CPU parked in backend_guard")
    return 0


if __name__ == "__main__":
    sys.exit(main())
