#!/usr/bin/env python3
"""replay_cdc_logs.py -- replay captured USB-CDC byte logs offline.

Feeds real device captures (``E:\\nabu\\logs\\kb*-com.bin``) through the
validated ``SerialDeframer`` from ``kdp_client/kdp_serial.py`` and reports:

* how many bytes / files were scanned,
* how many CRC-valid frames came out,
* how many frames were dropped by the receiver (CRC mismatch / overflow),
* the payload-length distribution,
* what fraction of the payloads are legitimate Ether/IPv4/UDP frames that use
  UDP port 41139 (``KDP_REMOTE_PORT``) -- i.e. XNU's KDP-over-Ethernet frames.

This is a strong acceptance test that needs no hardware: the payload inside a
serial-KDP frame is already a complete Ethernet frame (``kdp_udp.c`` always
calls ``kdp_send_data()`` after prepending Eth/IP/UDP), so replaying real logs
proves the bridge's ``--cdc-framing serial`` path against ground truth.

Usage::

    python replay_cdc_logs.py --glob "E:/nabu/logs/kb*-com.bin"
    python replay_cdc_logs.py --glob "E:/nabu/logs/kb2*-com.bin" --json out.json
"""

from __future__ import annotations

import argparse
import glob as globlib
import importlib.util
import json
import os
import struct
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_GLOB = "E:/nabu/logs/kb*-com.bin"

ETHERTYPE_IP = 0x0800
IPPROTO_UDP = 17
KDP_REMOTE_PORT = 41139          # osfmk/kdp/kdp_protocol.h:72
KDP_PAYLOAD_OFFSET = 42


def load_kdp_serial():
    candidates = [
        os.environ.get("KDP_SERIAL_PATH", ""),
        os.path.join(HERE, "kdp_serial.py"),
        os.path.join(HERE, "..", "kdp_client", "kdp_serial.py"),
    ]
    for path in candidates:
        if not path or not os.path.isfile(path):
            continue
        spec = importlib.util.spec_from_file_location("_replay_kdp_serial", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return module, path
    raise SystemExit("cannot locate kdp_serial.py")


SERIAL, SERIAL_SRC = load_kdp_serial()


def classify_payload(payload: bytes) -> dict:
    """Return structural facts about one deframed payload."""
    info = {
        "len": len(payload),
        "is_eth_ip_udp": False,
        "kdp_port": False,
        "verb": None,
    }
    if len(payload) < KDP_PAYLOAD_OFFSET:
        return info
    if int.from_bytes(payload[12:14], "big") != ETHERTYPE_IP:
        return info
    if payload[23] != IPPROTO_UDP:
        return info
    info["is_eth_ip_udp"] = True
    sport = int.from_bytes(payload[34:36], "big")
    dport = int.from_bytes(payload[36:38], "big")
    info["sport"], info["dport"] = sport, dport
    info["kdp_port"] = (sport == KDP_REMOTE_PORT or dport == KDP_REMOTE_PORT)
    if len(payload) > KDP_PAYLOAD_OFFSET:
        info["verb"] = payload[KDP_PAYLOAD_OFFSET] & 0x7F
    return info


def replay_file(path: str) -> dict:
    with open(path, "rb") as fh:
        data = fh.read()

    deframer = SERIAL.SerialDeframer()
    payloads = deframer.feed_bytes(data)

    start_chars = data.count(SERIAL.SKDP_START_CHAR)
    kinds = Counter()
    lengths = Counter()
    verbs = Counter()
    kdp_ok = 0
    for payload in payloads:
        info = classify_payload(payload)
        lengths[info["len"]] += 1
        if info["is_eth_ip_udp"]:
            kinds["eth_ipv4_udp"] += 1
            if info["kdp_port"]:
                kdp_ok += 1
                verbs[info["verb"]] += 1
            else:
                kinds["eth_ipv4_udp_other_port"] += 1
        else:
            kinds["not_kdp_ethernet"] += 1

    stats = deframer.stats
    # A complete serial-KDP frame contains two 0xFA bytes (the start marker and
    # the "CRC present" delimiter); anything beyond 2*frames_ok is console text
    # or the remains of a truncated frame.
    stray = max(0, start_chars - 2 * stats.frames_rx)
    return {
        "file": os.path.basename(path),
        "bytes": len(data),
        "start_chars": start_chars,
        "frames_ok": stats.frames_rx,
        "crc_errors": stats.crc_errors,
        "overflow_drops": stats.overflow_drops,
        "stray_start_chars": stray,
        "eth_ipv4_udp": kinds["eth_ipv4_udp"],
        "eth_ipv4_udp_kdp_port": kdp_ok,
        "eth_ipv4_udp_other_port": kinds["eth_ipv4_udp_other_port"],
        "not_kdp_ethernet": kinds["not_kdp_ethernet"],
        "lengths": dict(sorted(lengths.items())),
        "verbs": {str(k): v for k, v in sorted(verbs.items(), key=lambda kv: -kv[1])},
    }


def summarise(results: list[dict]) -> dict:
    total = {
        "files": len(results),
        "bytes": sum(r["bytes"] for r in results),
        "start_chars": sum(r["start_chars"] for r in results),
        "frames_ok": sum(r["frames_ok"] for r in results),
        "crc_errors": sum(r["crc_errors"] for r in results),
        "overflow_drops": sum(r["overflow_drops"] for r in results),
        "stray_start_chars": sum(r["stray_start_chars"] for r in results),
        "eth_ipv4_udp": sum(r["eth_ipv4_udp"] for r in results),
        "eth_ipv4_udp_kdp_port": sum(r["eth_ipv4_udp_kdp_port"] for r in results),
        "eth_ipv4_udp_other_port": sum(r["eth_ipv4_udp_other_port"] for r in results),
        "not_kdp_ethernet": sum(r["not_kdp_ethernet"] for r in results),
    }
    lengths = Counter()
    for r in results:
        lengths.update({int(k): v for k, v in r["lengths"].items()})
    total["length_histogram"] = dict(sorted(lengths.items()))
    total["length_min"] = min(lengths) if lengths else None
    total["length_max"] = max(lengths) if lengths else None
    total["length_mode"] = lengths.most_common(5)
    total["pct_kdp_port_of_ok"] = (
        round(100.0 * total["eth_ipv4_udp_kdp_port"] / total["frames_ok"], 2)
        if total["frames_ok"] else 0.0
    )
    return total


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--glob", default=DEFAULT_GLOB,
                        help=f"glob pattern for capture files (default {DEFAULT_GLOB})")
    parser.add_argument("--json", dest="json_out", default=None,
                        help="also write the aggregate summary as JSON")
    parser.add_argument("--per-file", action="store_true",
                        help="print one line per file")
    args = parser.parse_args(argv)

    files = sorted(globlib.glob(args.glob))
    if not files:
        print(f"no files match {args.glob!r}", file=sys.stderr)
        return 2

    results = [replay_file(path) for path in files]
    total = summarise(results)

    print(f"deframer : {SERIAL_SRC}")
    print(f"files    : {total['files']}")
    print(f"bytes    : {total['bytes']}")
    print(f"0xFA     : {total['start_chars']}   "
          f"(accepted {total['frames_ok']} frames = {2 * total['frames_ok']} markers, "
          f"crc drops {total['crc_errors']}, overflow drops {total['overflow_drops']}, "
          f"stray markers {total['stray_start_chars']})")
    print(f"payloads : eth/ipv4/udp = {total['eth_ipv4_udp']}, "
          f"of which udp port {KDP_REMOTE_PORT} = {total['eth_ipv4_udp_kdp_port']} "
          f"({total['pct_kdp_port_of_ok']}% of accepted frames)")
    print(f"          other udp port = {total['eth_ipv4_udp_other_port']}, "
          f"non-KDP ethernet = {total['not_kdp_ethernet']}")
    print(f"lengths  : min={total['length_min']} max={total['length_max']} "
          f"top={total['length_mode']}")

    if args.per_file:
        for r in results:
            print(f"  {r['file']}: bytes={r['bytes']} ok={r['frames_ok']} "
                  f"crc_drop={r['crc_errors']} udp41139={r['eth_ipv4_udp_kdp_port']} "
                  f"other={r['eth_ipv4_udp_other_port']}")

    if args.json_out:
        payload = {"source_glob": args.glob, "kdp_serial": SERIAL_SRC,
                   "summary": total, "files": results}
        with open(args.json_out, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2)
        print(f"json     : {args.json_out}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
