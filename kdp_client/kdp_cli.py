#!/usr/bin/env python3
"""kdp_cli.py -- command line client for XNU KDP-over-serial.

Usage
-----
    python kdp_cli.py --port COM21 connect
    python kdp_cli.py --port COM21 version
    python kdp_cli.py --port COM21 hostinfo
    python kdp_cli.py --port COM21 readmem 0xfffffe0000000000 64
    python kdp_cli.py --port COM21 readphys 0x80000000 64 --cpu 0
    python kdp_cli.py --port COM21 readregs --cpu 0
    python kdp_cli.py --port COM21 resume
    python kdp_cli.py --port COM21 suspend
    python kdp_cli.py --port COM21 bpadd 0xfffffe0012345678
    python kdp_cli.py --port COM21 bpdel 0xfffffe0012345678
    python kdp_cli.py --port COM21 bplist
    python kdp_cli.py --port COM21 disconnect
    python kdp_cli.py --port COM21 step
    python kdp_cli.py selftest

On Linux/WSL use /dev/ttyACM0 (or a pyserial URL such as socket://host:port).
On Windows use the COM port number, e.g. COM21.

Session persistence
-------------------
The kernel keeps a KDP session (sequence counter + session key) as long as it
stays inside the debugger loop.  To make one-shot commands composable, the CLI
stores ``seq``/``key`` in ``kdp_session.json`` next to this file and reuses
them on the next invocation.  ``disconnect`` (or a kernel re-entry) clears the
session.  Use ``--fresh`` to force a brand new CONNECT (seq=0).

NOTE: everything here is protocol-complete but has NOT been validated on real
hardware.  See README.md.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

import kdp_proto
import kdp_serial
from kdp_proto import (
    ARM_THREAD_STATE64,
    KdpError,
    KdpSession,
    KdpTimeout,
    KdpUnsupported,
    REQUEST_NAMES,
)

HERE = os.path.dirname(os.path.abspath(__file__))
SESSION_FILE = os.path.join(HERE, "kdp_session.json")
BREAKPOINT_FILE = os.path.join(HERE, "kdp_breakpoints.json")


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------

def parse_int(text: str) -> int:
    """Parse an integer that may be decimal or 0x-prefixed hex."""
    t = text.strip()
    return int(t, 16) if t.lower().startswith("0x") or t.lower().startswith("-0x") \
        else int(t, 10)


def hexdump(data: bytes, base: int = 0, width: int = 16) -> str:
    lines = []
    for off in range(0, len(data), width):
        chunk = data[off:off + width]
        hexpart = " ".join(f"{b:02x}" for b in chunk)
        asciipart = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{base + off:016x}  {hexpart:<{width * 3}}  {asciipart}")
    return "\n".join(lines)


def load_json(path: str, default):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError):
        return default


def save_json(path: str, obj) -> None:
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(obj, fh, indent=2)


def load_session() -> dict:
    return load_json(SESSION_FILE, {})


def save_session(state: dict) -> None:
    save_json(SESSION_FILE, state)


def clear_session() -> None:
    try:
        os.remove(SESSION_FILE)
    except OSError:
        pass


def load_breakpoints() -> list:
    return load_json(BREAKPOINT_FILE, [])


def save_breakpoints(addrs: list) -> None:
    save_json(BREAKPOINT_FILE, addrs)


# --------------------------------------------------------------------------
# command implementations
# --------------------------------------------------------------------------

def _apply_state(sess: KdpSession, state: dict) -> None:
    sess.seq = int(state.get("seq", 0)) & 0xFF
    sess.key = int(state.get("key", sess.key)) & 0xFFFFFFFF
    sess.reply_port = int(state.get("reply_port", sess.reply_port))
    sess.exception_port = int(state.get("exception_port", sess.exception_port))
    sess.connected = bool(state.get("connected", False))


def _state_from(sess: KdpSession) -> dict:
    return {
        "seq": sess.seq,
        "key": sess.key,
        "reply_port": sess.reply_port,
        "exception_port": sess.exception_port,
        "connected": sess.connected,
    }


def _ensure_session(sess: KdpSession, state: dict, fresh: bool) -> None:
    """Reuse a persisted session or perform a fresh CONNECT."""
    if state.get("connected") and not fresh:
        _apply_state(sess, state)
        print(f"[cli] reusing persisted session: seq={sess.seq} key=0x{sess.key:08x}")
        return
    sess.connect()
    save_session(_state_from(sess))
    print(f"[cli] connected: key=0x{sess.key:08x} seq={sess.seq}")


def cmd_connect(sess, args, state):
    # `connect` always performs a fresh CONNECT (seq=0).
    if args.fresh:
        clear_session()
    try:
        reply = sess.connect()
    except kdp_proto.KdpProtocolError as exc:
        # Most commonly ALREADY_CONNECTED, i.e. the kernel is still in a
        # session left behind by an earlier run.
        print(f"[cli] {exc}", file=sys.stderr)
        print("[cli] the kernel may still be in an earlier KDP session; "
              "re-trigger KDP (panic/NMI) or reboot, then retry.",
              file=sys.stderr)
        return 1
    save_session(_state_from(sess))
    print(f"[cli] CONNECT ok: error={reply.error_name} key=0x{sess.key:08x} "
          f"seq={sess.seq}")
    try:
        ver = sess.version()
        print(f"[cli] VERSION: remote_version={ver['version']} "
              f"feature=0x{ver['feature']:x} bp_supported={ver['bp_supported']}")
    except KdpError as exc:
        print(f"[cli] VERSION failed: {exc}")
    try:
        hi = sess.hostinfo()
        print(f"[cli] HOSTINFO: cpus={hi['cpu_count']} "
              f"cpus_mask=0x{hi['cpus_mask']:x} cpu_type=0x{hi['cpu_type']:x} "
              f"cpu_subtype=0x{hi['cpu_subtype']:x}")
    except KdpError as exc:
        print(f"[cli] HOSTINFO failed: {exc}")
    save_session(_state_from(sess))
    return 0


def cmd_disconnect(sess, args, state):
    _ensure_session(sess, state, args.fresh)
    reply = sess.disconnect()
    clear_session()
    print(f"[cli] DISCONNECT: error={reply.error_name}")
    return 0


def cmd_reboot(sess, args, state):
    """KDP_HOSTREBOOT -- reset the board from the debugger.

    This is the way out of a KDP session on arm64. DISCONNECT only clears the
    session; kdp_debugger_loop() then goes round again and the kernel does not
    resume (T39). HOSTREBOOT is handled inside kdp_connection_wait() by a direct
    kdp_machine_reboot(), so the machine goes down instead.

    No reply is expected -- the kernel is on its way out -- so "sent" is as far
    as this command can confirm. Check with adb devices and the COM port list.
    """
    _ensure_session(sess, state, args.fresh)
    sess.host_reboot()
    clear_session()
    print("[cli] HOSTREBOOT sent (no reply is expected; the board is resetting)")
    print("[cli] confirm with: adb devices  /  COM3 gone")
    return 0


def cmd_version(sess, args, state):
    _ensure_session(sess, state, args.fresh)
    ver = sess.version()
    print(f"remote_version = {ver['version']}")
    print(f"feature        = 0x{ver['feature']:x}")
    print(f"bp_supported   = {ver['bp_supported']}")
    print(f"pad0           = 0x{ver['pad0']:x}")
    print(f"pad1           = 0x{ver['pad1']:x}")
    save_session(_state_from(sess))
    return 0


def cmd_hostinfo(sess, args, state):
    _ensure_session(sess, state, args.fresh)
    hi = sess.hostinfo()
    print(f"cpus_mask   = 0x{hi['cpus_mask']:x}")
    print(f"cpu_count   = {hi['cpu_count']}")
    print(f"cpu_type    = 0x{hi['cpu_type']:x}")
    print(f"cpu_subtype = 0x{hi['cpu_subtype']:x}")
    save_session(_state_from(sess))
    return 0


def cmd_maxbytes(sess, args, state):
    _ensure_session(sess, state, args.fresh)
    print(f"max_bytes = {sess.maxbytes()}")
    save_session(_state_from(sess))
    return 0


def cmd_kernelversion(sess, args, state):
    _ensure_session(sess, state, args.fresh)
    print(sess.kernelversion())
    save_session(_state_from(sess))
    return 0


def _emit_memory(data: bytes, args, address: int) -> None:
    if args.out:
        with open(args.out, "wb") as fh:
            fh.write(data)
        print(f"[cli] wrote {len(data)} bytes to {args.out}")
        return
    if args.format == "hex":
        print(data.hex())
    else:
        print(hexdump(data, base=address))


def cmd_readmem(sess, args, state):
    _ensure_session(sess, state, args.fresh)
    addr = parse_int(args.addr)
    length = parse_int(args.length)
    data = sess.readmem64(addr, length)
    _emit_memory(data, args, addr)
    save_session(_state_from(sess))
    return 0


def cmd_readphys(sess, args, state):
    _ensure_session(sess, state, args.fresh)
    addr = parse_int(args.addr)
    length = parse_int(args.length)
    data = sess.readphys64(addr, length, lcpu=args.cpu)
    _emit_memory(data, args, addr)
    save_session(_state_from(sess))
    return 0


def cmd_readregs(sess, args, state):
    _ensure_session(sess, state, args.fresh)
    regs = sess.readregs(cpu=args.cpu, flavor=args.flavor)
    if "x" not in regs:
        print(f"flavor={regs['flavor']} raw={regs['raw'].hex()}")
        save_session(_state_from(sess))
        return 0
    for i in range(29):
        print(f"x{i:<2} = 0x{regs[f'x{i}']:016x}")
    print(f"fp   = 0x{regs['fp']:016x}")
    print(f"lr   = 0x{regs['lr']:016x}")
    print(f"sp   = 0x{regs['sp']:016x}")
    print(f"pc   = 0x{regs['pc']:016x}")
    print(f"cpsr = 0x{regs['cpsr']:08x}")
    print(f"flags= 0x{regs['flags']:08x}")
    save_session(_state_from(sess))
    return 0


def cmd_resume(sess, args, state):
    _ensure_session(sess, state, args.fresh)
    reply = sess.resume(args.mask)
    clear_session()
    print(f"[cli] RESUMECPUS mask=0x{args.mask & 0xFFFFFFFF:x} "
          f"error={reply.error_name}")
    return 0


def cmd_suspend(sess, args, state):
    _ensure_session(sess, state, args.fresh)
    reply = sess.suspend()
    print(f"[cli] SUSPEND error={reply.error_name}")
    save_session(_state_from(sess))
    return 0


def cmd_bpadd(sess, args, state):
    _ensure_session(sess, state, args.fresh)
    addr = parse_int(args.addr)
    reply = sess.bp_add(addr)
    if reply.error == kdp_proto.KDPERR_NO_ERROR:
        addrs = load_breakpoints()
        if addr not in addrs:
            addrs.append(addr)
        save_breakpoints(addrs)
    print(f"[cli] BREAKPOINT64_SET 0x{addr:x}: error={reply.error_name}")
    save_session(_state_from(sess))
    return 0


def cmd_bpdel(sess, args, state):
    _ensure_session(sess, state, args.fresh)
    addr = parse_int(args.addr)
    reply = sess.bp_remove(addr)
    if reply.error == kdp_proto.KDPERR_NO_ERROR:
        addrs = load_breakpoints()
        if addr in addrs:
            addrs.remove(addr)
        save_breakpoints(addrs)
    print(f"[cli] BREAKPOINT64_REMOVE 0x{addr:x}: error={reply.error_name}")
    save_session(_state_from(sess))
    return 0


def cmd_bplist(sess, args, state):
    addrs = load_breakpoints()
    if not addrs:
        print("(no breakpoints recorded on the host side)")
        return 0
    for idx, addr in enumerate(addrs):
        print(f"bp{idx:<3} 0x{addr:016x}")
    return 0


def cmd_step(sess, args, state):
    """Single-step is not available; report why without touching a session."""
    raise KdpUnsupported(
        "KDP_SW_STEP does not exist in XNU 17.7.11; single-step is not "
        "available over this KDP"
    )


def cmd_reattach(sess, args, state):
    if state.get("connected") and not args.fresh:
        _apply_state(sess, state)
    reply = sess.reattach()
    save_session(_state_from(sess))
    print(f"[cli] REATTACH: error={reply.error_name}")
    return 0


# --------------------------------------------------------------------------
# argument parsing
# --------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="kdp_cli.py",
        description="KDP-over-serial client for XNU 17.7.11 (nabu / SM8150).",
    )
    p.add_argument("--port", help="serial port: COM21 (Windows) or /dev/ttyACM0 "
                                  "(required when --transport serial)")
    p.add_argument("--transport", choices=("serial", "udp"), default="serial",
                   help="'serial' = talk to the device directly (default); "
                        "'udp' = talk to kdp_net_bridge over a UDP socket")
    p.add_argument("--host", default="127.0.0.1",
                   help="bridge address for --transport udp (default 127.0.0.1)")
    p.add_argument("--udp-port", type=int, default=kdp_proto.KDP_REMOTE_PORT,
                   help=f"bridge UDP port for --transport udp "
                        f"(default {kdp_proto.KDP_REMOTE_PORT})")
    p.add_argument("--poll", type=float, default=0.1,
                   help="socket poll timeout in seconds (default 0.1)")
    p.add_argument("--baud", type=int, default=115200, help="baud rate (default 115200)")
    p.add_argument("--timeout", type=float, default=kdp_proto.KDP_REXMIT_SECS,
                   help="per-attempt reply timeout in seconds (default 3)")
    p.add_argument("--retries", type=int, default=kdp_proto.KDP_REXMIT_TRIES,
                   help="retransmit attempts (default 8)")
    p.add_argument("--connect-timeout", type=float, default=10.0,
                   help="CONNECT timeout in seconds (default 10)")
    p.add_argument("--reply-port", type=int, default=kdp_proto.UDP_HOST_COMM_BASE,
                   help="UDP reply port announced in CONNECT (default 41140)")
    p.add_argument("--exception-port", type=int, default=kdp_proto.UDP_HOST_EXCEP_BASE,
                   help="UDP exception port announced in CONNECT (default 41145)")
    p.add_argument("--key", type=lambda s: parse_int(s), default=0x1337C0DE,
                   help="session key (default 0x1337c0de)")
    p.add_argument("--fresh", action="store_true",
                   help="ignore the persisted session and force a new CONNECT")
    p.add_argument("--verbose", action="store_true", help="dump frames and packets")

    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("connect", help="CONNECT, then query VERSION/HOSTINFO")
    sub.add_parser("reattach", help="KDP_REATTACH")
    sub.add_parser("disconnect", help="KDP_DISCONNECT and clear the session")
    sub.add_parser("reboot", help="KDP_HOSTREBOOT: reset the board (the arm64 way out)")
    sub.add_parser("version", help="KDP_VERSION")
    sub.add_parser("hostinfo", help="KDP_HOSTINFO")
    sub.add_parser("maxbytes", help="KDP_MAXBYTES")
    sub.add_parser("kernelversion", help="KDP_KERNELVERSION")

    def add_mem_args(sp):
        sp.add_argument("addr", help="address (hex or decimal)")
        sp.add_argument("length", help="length in bytes (hex or decimal)")
        sp.add_argument("--format", choices=("hexdump", "hex"), default="hexdump",
                        help="output format (default hexdump)")
        sp.add_argument("--out", help="write raw bytes to this file instead")

    sp = sub.add_parser("readmem", help="KDP_READMEM64 (64-bit virtual address)")
    add_mem_args(sp)
    sp = sub.add_parser("readphys", help="KDP_READPHYSMEM64 (64-bit physical address)")
    add_mem_args(sp)
    sp.add_argument("--cpu", type=int, default=0, help="logical CPU (default 0)")

    sp = sub.add_parser("readregs", help="KDP_READREGS (AArch64 thread state)")
    sp.add_argument("--cpu", type=int, default=0, help="CPU number (default 0)")
    sp.add_argument("--flavor", type=int, default=ARM_THREAD_STATE64,
                    help="register flavor (default 6 = ARM_THREAD_STATE64)")

    sp = sub.add_parser("resume", help="KDP_RESUMECPUS")
    sp.add_argument("--mask", type=lambda s: parse_int(s), default=0xFFFFFFFF,
                    help="CPU mask (default 0xffffffff)")

    sub.add_parser("suspend", help="KDP_SUSPEND")

    sp = sub.add_parser("bpadd", help="KDP_BREAKPOINT64_SET (software breakpoint)")
    sp.add_argument("addr", help="address (hex or decimal)")
    sp = sub.add_parser("bpdel", help="KDP_BREAKPOINT64_REMOVE")
    sp.add_argument("addr", help="address (hex or decimal)")
    sub.add_parser("bplist", help="list breakpoints recorded on the host side")
    sub.add_parser("step", help="single-step (NOT supported by this KDP)")
    sub.add_parser("selftest", help="run the offline loopback self-test")

    return p


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def run_selftest() -> int:
    try:
        import selftest
    except ImportError:
        print("[cli] selftest.py not found next to kdp_cli.py", file=sys.stderr)
        return 2
    return selftest.main()


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    if args.command == "selftest":
        return run_selftest()
    if args.command == "bplist":
        return cmd_bplist(None, args, {})
    if args.command == "step":
        try:
            return cmd_step(None, args, {})
        except KdpUnsupported as exc:
            print(f"[cli] step: {exc}", file=sys.stderr)
            return 2

    if args.transport == "serial" and not args.port:
        print("[cli] --port is required with --transport serial "
              "(or use --transport udp)", file=sys.stderr)
        return 2

    state = {} if args.fresh else load_session()

    try:
        if args.transport == "udp":
            import kdp_udp
            transport = kdp_udp.open_udp(
                host=args.host, port=args.udp_port,
                timeout=max(args.poll, 0.02), verbose=args.verbose,
            )
            print(f"[cli] UDP transport -> {args.host}:{args.udp_port} "
                  f"(local {transport.local_addr[0]}:{transport.local_addr[1]})")
        else:
            transport = kdp_serial.open_serial(
                args.port, baudrate=args.baud, verbose=args.verbose
            )
    except Exception as exc:  # pyserial raises SerialException
        target = args.port if args.transport == "serial" else \
            f"{args.host}:{args.udp_port}"
        print(f"[cli] cannot open {target}: {exc}", file=sys.stderr)
        return 2

    sess = kdp_proto.KdpSession(
        transport,
        reply_port=args.reply_port,
        exception_port=args.exception_port,
        key=args.key,
        timeout=args.timeout,
        retries=args.retries,
        connect_timeout=args.connect_timeout,
        verbose=args.verbose,
    )

    handlers = {
        "connect": cmd_connect,
        "reattach": cmd_reattach,
        "disconnect": cmd_disconnect,
        "reboot": cmd_reboot,
        "version": cmd_version,
        "hostinfo": cmd_hostinfo,
        "maxbytes": cmd_maxbytes,
        "kernelversion": cmd_kernelversion,
        "readmem": cmd_readmem,
        "readphys": cmd_readphys,
        "readregs": cmd_readregs,
        "resume": cmd_resume,
        "suspend": cmd_suspend,
        "bpadd": cmd_bpadd,
        "bpdel": cmd_bpdel,
    }

    rc = 0
    try:
        rc = handlers[args.command](sess, args, state) or 0
    except KdpUnsupported as exc:
        print(f"[cli] {exc}", file=sys.stderr)
        rc = 2
    except KdpTimeout as exc:
        print(f"[cli] timeout: {exc}", file=sys.stderr)
        print("[cli] the kernel may not be in KDP, or the session may be "
              "out of sync; try `connect` again or re-trigger KDP "
              "(panic/NMI).", file=sys.stderr)
        rc = 3
    except KdpError as exc:
        print(f"[cli] KDP error: {exc}", file=sys.stderr)
        rc = 4
    except KeyboardInterrupt:
        print("\n[cli] interrupted", file=sys.stderr)
        rc = 130
    finally:
        transport.close()

    return rc


if __name__ == "__main__":
    sys.exit(main())
