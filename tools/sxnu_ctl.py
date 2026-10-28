#!/usr/bin/env python3
"""SXNU1 USB control-channel client over the CDC COM port.

Frame layout is taken from QcomUsbDebug's kernel/QcomUsbControl.h:

    request : SXNU1 <session:16hex> <seq:8hex> <VERB> <crc:8hex>\n
    reply   : SXNU1R <session:16hex> <seq:8hex> <VERB> <status> [extras] <crc:8hex>\n

CRC-32/IEEE covers the line up to, but excluding, the single space that
precedes the CRC field. HELLO must carry session=0 and seq=0; every other verb
must use the established session and the exact next sequence number, so seq is
stateful and never reused.
"""

import argparse
import binascii
import re
import sys
import time

VERBS = ("ping", "status", "reboot", "screenshot", "shotnext")
REPLY_RE = re.compile(rb"SXNU1R ([0-9a-f]{16}) ([0-9a-f]{8}) ([A-Z]+) (.*) ([0-9a-f]{8})")


def crc32(body: bytes) -> int:
    return binascii.crc32(body) & 0xFFFFFFFF


class Sxnu:
    def __init__(self, port, baud=115200, timeout=2.0):
        try:
            import serial
        except ImportError:
            sys.exit("pyserial is required:  pip install pyserial")
        self.tio = serial.Serial(port, baud, timeout=timeout)
        self.timeout = timeout
        self.session = 0
        self.seq = 0

    def close(self):
        try:
            self.tio.close()
        except Exception:
            pass

    def _drain(self):
        self.tio.reset_input_buffer()

    def _frame(self, verb: str) -> bytes:
        body = "SXNU1 %016x %08x %s" % (self.session, self.seq, verb)
        return ("%s %08x\n" % (body, crc32(body.encode()))).encode()

    def _read_reply(self, verb: str):
        verb_up = verb.upper()
        buf = b""
        deadline = time.time() + self.timeout + 1.0
        while time.time() < deadline:
            chunk = self.tio.read(512)
            if not chunk:
                continue
            buf += chunk
            for m in REPLY_RE.finditer(buf):
                if m.group(3).decode() != verb_up:
                    continue
                body = buf[m.start():m.end() - 9]
                want = int(m.group(5), 16)
                got = crc32(body)
                if want != got:
                    print("  CRC mismatch: want=%08x got=%08x" % (want, got), file=sys.stderr)
                    return None
                return {"session": int(m.group(1), 16), "seq": int(m.group(2), 16),
                        "verb": verb_up, "tail": m.group(4).decode()}
            buf = buf[-4096:]
        return None

    def hello(self):
        self.session = 0
        self.seq = 0
        self._drain()
        self.tio.write(self._frame("HELLO"))
        self.tio.flush()
        rep = self._read_reply("hello")
        if not rep:
            return None
        self.session = rep["session"]
        parts = dict(p.split("=", 1) for p in rep["tail"].split() if "=" in p)
        self.seq = int(parts.get("last_seq", "0"), 16)
        rep["last_seq"] = self.seq
        return rep

    def cmd(self, verb: str):
        if not self.session:
            raise RuntimeError("no session: hello first")
        self.seq += 1
        self._drain()
        self.tio.write(self._frame(verb.upper()))
        self.tio.flush()
        return self._read_reply(verb)


def find_port():
    """Locate the board CDC. Windows renumbers COMx and may reuse a number, so match
    on identity: VID/PID, or the bus-reported description."""
    from serial.tools import list_ports
    for p in list_ports.comports():
        if p.vid == 0x1209 and p.pid == 0x316D:
            return p.device
    for p in list_ports.comports():
        d = (p.description or "") + " " + (p.hwid or "")
        if "Qualcomm XNU" in d or "QCOMXNUUSBDEBUG" in d.upper():
            return p.device
    return None


def main():
    ap = argparse.ArgumentParser(description="SXNU1 control client")
    ap.add_argument("--port", default=None,
                    help="default: auto-detect USB\\VID_1209&PID_316D")
    ap.add_argument("verb", nargs="?", default="status", choices=("hello",) + VERBS)
    ap.add_argument("--confirm", action="store_true", help="required for reboot")
    args = ap.parse_args()

    if args.verb == "reboot" and not args.confirm:
        sys.exit("reboot needs --confirm. Note: ABL will boot the SAME payload; this "
                 "resets the board, it does not load a new kernel.")

    port = args.port or find_port()
    if not port:
        sys.exit("board CDC not found (VID_1209&PID_316D). COM1 is the PC's own UART; "
                 "the board port is absent -> board USB not enumerated.")
    print("port %s" % port)

    dev = Sxnu(port)
    try:
        rep = dev.hello()
        if not rep:
            sys.exit("HELLO failed: no SXNU1R reply. Check the board is in XNU with "
                     "qcom-usb-ctl=1 and qcom-usb-debug=1, and that %s is free." % port)
        print("HELLO ok  session=%016x last_seq=%d  %s" % (dev.session, dev.seq, rep["tail"]))
        if args.verb == "hello":
            return

        rep = dev.cmd(args.verb)
        if rep is None:
            sys.exit("%s: no reply" % args.verb)
        print("%-11s %s" % (args.verb.upper(), rep["tail"]))
        if args.verb == "reboot":
            print("board is resetting; COM port will disappear shortly")
    finally:
        dev.close()


if __name__ == "__main__":
    main()
