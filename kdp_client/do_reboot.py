#!/usr/bin/env python3
"""Send KDP_REBOOT via kdp_cli to reset the board back to recovery.

kdp_cli's `reboot` command uses KDP_HOSTREBOOT (0x13) -> PSCI reset, which does
not need the SXNU1 control verb that is currently not replying.  Needs the
device in KDP, so send `kdp` on the console first.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kdp_proto  # noqa: E402
import kdp_udp    # noqa: E402


def main() -> int:
    transport = kdp_udp.open_udp(host="127.0.0.1", port=41139, timeout=0.2)
    sess = kdp_proto.KdpSession(transport, timeout=3, retries=4,
                                connect_timeout=10)
    try:
        sess.connect()
    except Exception as exc:  # noqa: BLE001
        print(f"connect failed: {exc}")
        print("(send `kdp` on the console first)")
        return 2
    print("connected; sending KDP_HOSTREBOOT")
    try:
        sess.reboot()
    except Exception as exc:  # noqa: BLE001
        print(f"reboot request: {exc}")
    print("reboot sent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
