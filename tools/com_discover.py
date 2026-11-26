#!/usr/bin/env python3
"""com_discover.py -- identify every nabu USB CDC COM port by its role.

T175 (P1/P2).  Windows assigns COM numbers arbitrarily, so a script that writes
COM3 will point at the wrong port after a re-enumeration.  This tool asks each
candidate port who it is: the device answers HELLO with ``role=0`` (console,
pipe 0) or ``role=1`` (log/KDP, pipe 1), and the result is cached in
``port_map.json`` next to this file so nabu_mux.py / kdp_net_bridge.py can pick
the port by role instead of by number.

Design constraints:
  * enumerate only usbser devices whose VID:PID is 1209:316D;
  * every probe is bounded by ``--timeout`` and wrapped, so a busy, silent or
    absent port never blocks the run and never raises;
  * exit code 0 means "at least one usable port was classified";
  * with no ``role=`` field at all (an older kernel without the T175 patch) the
    port is reported as ``unknown`` and the legacy ``--port`` override stays the
    only way to select it.

Usage:
    python com_discover.py                 # human summary + write port_map.json
    python com_discover.py --json          # machine-readable only
    python com_discover.py --timeout 2.0 --wait 30
    python com_discover.py --no-map        # do not touch port_map.json

Exit codes: 0 = at least one console/log port found, 1 = none usable,
2 = setup error (pyserial missing, bad arguments).
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import re
import sys
import time
import zlib
from pathlib import Path

VID = 0x1209
PID = 0x316D
VIDPID = "%04X:%04X" % (VID, PID)
# The gadget descriptor is really 1209:316D (module/soc/Sm8150.h:20-21, written
# into the device descriptor at Dwc3.c:641-644).  Windows' composite child PDOs
# still report 0000:0000 for MI_00/MI_02, and pyserial can hand us that zero
# pair, so the parent id 1209:316D is the authoritative match and the zero pair
# is only a fallback.  Verified 2026-11-26: the parent HardwareId is
# USB\VID_1209&PID_316D\QCOMXNUUSBDEBUG while the children say VID_0000&PID_0000.
SERIAL_TAG = "QCOMXNUUSBDEBUG"
FALLBACK_VIDPID = {"0000:0000"}
ALL_VIDPID = {VIDPID.upper()} | FALLBACK_VIDPID

ROLE_NAMES = {0: "console", 1: "log"}
ROLE_IFACE = {0: 0, 1: 1}

HERE = Path(__file__).resolve().parent
DEFAULT_MAP = HERE / "port_map.json"

# NOTE: both hooks exist so the offline self-test can drive the whole decision
# path without a COM port.  Production leaves them as the real implementations.
list_nabu_ports = None      # () -> [port entry]
open_port = None            # (device, timeout) -> file-like with read/write


class ProbeError(Exception):
    """Internal: a single port could not be probed."""


# ---------------------------------------------------------------------------
# enumerating
# ---------------------------------------------------------------------------

def _real_list_nabu_ports():
    try:
        from serial.tools.list_ports import comports
    except Exception as exc:  # ImportError or a broken backend
        raise ProbeError("pyserial unavailable: %s" % exc)
    found = []
    for p in comports():
        vid = getattr(p, "vid", None)
        pid = getattr(p, "pid", None)
        hwid = getattr(p, "hwid", "") or ""
        pair = "%04X:%04X" % (vid, pid) if vid is not None and pid is not None else ""
        hw = hwid.upper()
        # 1) authoritative: the real parent id 1209:316D, in any of the shapes
        #    Windows/pyserial use (VID:PID=, VID_...&PID_..., bare pair).
        hit = pair.upper() == VIDPID.upper()
        if not hit:
            a, b = VIDPID.split(":")
            hit = (("VID:%s&PID:%s" % (a, b)) in hw or
                   ("VID_%s&PID_%s" % (a, b)) in hw)
        # 2) fallback: the zero pair Windows reports for composite child PDOs.
        #    Only trusted together with our serial tag, otherwise any gadget with
        #    0000:0000 would be claimed.
        if not hit and pair.upper() in FALLBACK_VIDPID and SERIAL_TAG in hw:
            hit = True
        if not hit and SERIAL_TAG in hw and not pair:
            # pyserial gave us no ids at all but the serial string is ours.
            hit = True
        if hit:
            found.append(p)
    return found


# ---------------------------------------------------------------------------
# probing one port
# ---------------------------------------------------------------------------

def _hello_frame(session=0, seq=0):
    """SXNU1 <16hex session> <8hex seq> HELLO <8hex crc>\\n (crc over the body)."""
    prefix = "SXNU1 %016X %08X HELLO" % (session, seq)
    return ("%s %08X\n" % (prefix, zlib.crc32(prefix.encode("ascii")) & 0xFFFFFFFF)).encode("ascii")


def _real_open_port(device, timeout):
    import serial  # pyserial; imported late so --help works without it
    return serial.Serial(device, 115200, timeout=timeout, write_timeout=timeout)


_ROLE_RE = re.compile(r"role=([0-9]+)")


def _read_reply(handle, timeout):
    """Read until one SXNU1R line arrives or the deadline passes."""
    deadline = time.monotonic() + timeout
    buf = b""
    while True:
        remain = deadline - time.monotonic()
        if remain <= 0:
            break
        try:
            chunk = handle.read(512)
        except Exception as exc:
            raise ProbeError("read failed: %s" % exc)
        if chunk:
            buf += chunk
            if b"\n" in buf:
                for line in buf.split(b"\n"):
                    if b"SXNU1R" in line:
                        return line.decode("ascii", "replace")
                buf = b""
        else:
            # pyserial returns b"" on timeout; sleep briefly so a slow device
            # is not hammered.
            time.sleep(min(0.02, max(0.0, remain)))
    return None


def probe_port(entry, timeout):
    """Classify one port entry.  Never raises; returns a result dict."""
    device = getattr(entry, "device", None) or str(entry)
    result = {
        "port": device,
        "hwid": getattr(entry, "hwid", "") or "",
        "role": "unknown",
        "role_id": None,
        "iface": None,
        "error": None,
    }
    handle = None
    try:
        handle = open_port(device, timeout)
        handle.write(_hello_frame())
        try:
            handle.flush()
        except Exception:
            pass
        line = _read_reply(handle, timeout)
        if not line:
            result["error"] = "timeout"
            return result
        m = _ROLE_RE.search(line)
        if not m:
            result["error"] = "no-role-field"
            return result
        role_id = int(m.group(1))
        if role_id not in ROLE_NAMES:
            result["error"] = "bad-role-%d" % role_id
            result["role_id"] = role_id
            return result
        result["role"] = ROLE_NAMES[role_id]
        result["role_id"] = role_id
        result["iface"] = ROLE_IFACE[role_id]
    except Exception as exc:
        result["error"] = "%s: %s" % (type(exc).__name__, exc)
    finally:
        if handle is not None:
            try:
                handle.close()
            except Exception:
                pass
    return result


# ---------------------------------------------------------------------------
# discovery / cache
# ---------------------------------------------------------------------------

def discover(timeout, wait_s=0.0, poll_gap=0.5):
    """Enumerate + probe.  Repeats enumeration while ``wait_s`` allows and no
    usable port has shown up yet (the console may be started before flashing)."""
    deadline = time.monotonic() + wait_s
    while True:
        try:
            entries = list_nabu_ports()
        except ProbeError as exc:
            return [], str(exc)
        results = [probe_port(e, timeout) for e in entries]
        usable = [r for r in results if r["role"] in ROLE_NAMES.values()]
        if usable or wait_s <= 0 or time.monotonic() >= deadline:
            return results, None
        time.sleep(min(poll_gap, max(0.0, deadline - time.monotonic())))


def build_map(results):
    """Classify by the device's own ``role=`` field and nothing else.

    T178 §2 proposed falling back to the STATUS ``dt=`` field when both ports
    answer unknown.  That field is not a port identity: QcomXnuUsbDebug.c:96-105
    defines ``qud_dt_job.state`` as the DEVTREE job state (0 idle, 1 running,
    2 done, 3 failed).  So the fallback is deliberately NOT implemented — an
    unidentified port stays None instead of being guessed.
    """
    console = next((r for r in results if r["role"] == "console"), None)
    logp = next((r for r in results if r["role"] == "log"), None)
    return {
        "generated": _dt.datetime.now().astimezone().isoformat(timespec="seconds"),
        "vidpid": VIDPID,
        "console": console["port"] if console else None,
        "log": logp["port"] if logp else None,
        "usable": bool(console or logp),
        "ports": results,
    }


def write_map(mapping, path):
    tmp = Path(path).with_suffix(Path(path).suffix + ".tmp")
    tmp.write_text(json.dumps(mapping, indent=2, sort_keys=True) + "\n",
                   encoding="utf-8")
    os.replace(tmp, path)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(
        prog="com_discover.py",
        description="Identify nabu CDC COM ports by role (console vs log/KDP).")
    p.add_argument("--timeout", type=float, default=1.5,
                   help="per-port HELLO timeout in seconds (default 1.5)")
    p.add_argument("--wait", type=float, default=0.0,
                   help="re-enumerate for up to N seconds while no usable port "
                        "appears (default 0 = single pass)")
    p.add_argument("--map", default=str(DEFAULT_MAP),
                   help="port_map.json path (default: alongside this script)")
    p.add_argument("--no-map", action="store_true",
                   help="do not write the cache file")
    p.add_argument("--json", action="store_true",
                   help="print the mapping as JSON on stdout")
    p.add_argument("--quiet", action="store_true",
                   help="suppress the human summary")
    return p


def main(argv=None):
    global list_nabu_ports, open_port
    args = build_parser().parse_args(argv)

    if list_nabu_ports is None:
        list_nabu_ports = _real_list_nabu_ports
    if open_port is None:
        open_port = _real_open_port

    try:
        results, err = discover(args.timeout, args.wait)
    except Exception as exc:  # defensive: discovery must never traceback
        print("com_discover: %s" % exc, file=sys.stderr)
        return 2

    mapping = build_map(results)
    if err:
        mapping["error"] = err

    if not args.no_map:
        try:
            write_map(mapping, args.map)
        except Exception as exc:
            print("com_discover: cannot write %s: %s" % (args.map, exc),
                  file=sys.stderr)

    if args.json:
        print(json.dumps(mapping, indent=2, sort_keys=True))
    elif not args.quiet:
        print("nabu CDC %s -- %d candidate(s)" % (VIDPID, len(results)))
        for r in results:
            if r["role"] == "unknown":
                print("  %-10s unknown   (%s)" % (r["port"], r["error"]))
            else:
                print("  %-10s %-8s role=%d if=%d"
                      % (r["port"], r["role"], r["role_id"], r["iface"]))
        if mapping["console"]:
            print("console -> %s" % mapping["console"])
        if mapping["log"]:
            print("log     -> %s" % mapping["log"])
        if not mapping["usable"]:
            print("no usable port (is the device enumerated and not in KDP?)")
        if not args.no_map:
            print("map     -> %s" % args.map)

    return 0 if mapping["usable"] else (2 if err else 1)


if __name__ == "__main__":
    sys.exit(main())
