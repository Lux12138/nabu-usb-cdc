#!/usr/bin/env python3
"""kdp_net_bridge.py -- UDP <-> USB CDC serial bridge for XNU KDP (nabu).

What this does
--------------
Device side (nabu, SM8150) registers a pair of *raw Ethernet frame* callbacks
with XNU's KDP core, so the kernel hands the transport complete
Ethernet + IPv4 + UDP + KDP frames instead of the classic 0xFA/CRC serial
stream (see T50 us-net-kdp.md section B6).  Because a USB CDC bulk pipe has no
frame boundaries, those Ethernet frames are wrapped in the small ``KDPF``
header defined in ``kdp_cdc_frame.py``.

This program is the host half of that link:

    KDP client (lldb / kdp_cli)             nabu device
    ----------------------                  -------------
    UDP socket  :41139  <-------------->    USB CDC (COM21 / /dev/ttyACM0)
    bare KDP packet          bridge           Ethernet+IP+UDP+KDP frame
                        (this program)

Two UDP payload conventions are supported (``--udp-mode``):

* ``kdp`` (default, Apple lldb compatible)
    One UDP datagram = one **bare KDP packet** (8-byte kdp_hdr_t + body),
    exactly what ``lldb/source/Plugins/Process/MacOSX-Kernel/CommunicationKDP.cpp``
    writes to the UDP connection.  The bridge wraps outgoing datagrams in
    Ethernet/IP/UDP before putting them on CDC, and unwraps incoming Ethernet
    frames before sending them back over UDP.  A datagram that already looks
    like an Ethernet/IP/UDP KDP frame is passed through unchanged, so our own
    ``kdp_cli.py`` (which builds the L2/L3 wrapper itself) works too.

* ``eth``
    One UDP datagram = one raw Ethernet frame; pure byte pump.  Useful when
    another host-side component wants to see the whole frame.

Usage
-----
    python kdp_net_bridge.py --port COM21 --udp-port 41139
    python kdp_net_bridge.py --port /dev/ttyACM0 --udp-host 127.0.0.1
    python kdp_net_bridge.py --selftest

Dependencies
------------
Python 3.10+.  Real hardware needs ``pyserial`` (``python -m pip install
pyserial``).  The self-test needs nothing but the standard library plus the
optional ``kdp_proto.py`` next to ``../kdp_client``.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import socket
import sys
import threading
import time
import traceback

from kdp_cdc_frame import (
    CdcDeframer,
    FrameError,
    FrameStats,
    MAX_PAYLOAD,
    frame_packet,
    parse_header,
)

# --------------------------------------------------------------------------
# Protocol constants (osfmk/kdp/kdp_protocol.h, verified on the local tree)
# --------------------------------------------------------------------------

KDP_REMOTE_PORT = 41139          # kdp_protocol.h:72
MAX_KDP_PKT_SIZE = 1200          # kdp_protocol.h:710
ETH_HDR_LEN = 14
IP_HDR_LEN = 20
UDP_HDR_LEN = 8
KDP_PAYLOAD_OFFSET = ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN   # 42
ETHERTYPE_IP = 0x0800
IPPROTO_UDP = 17


# --------------------------------------------------------------------------
# Reuse the existing L2/L3 + KDP helpers from the sibling kdp_client package
# --------------------------------------------------------------------------

def _candidate_paths():
    here = os.path.dirname(os.path.abspath(__file__))
    yield os.environ.get("KDP_PROTO_PATH", "")
    yield os.path.join(here, "kdp_proto.py")
    yield os.path.join(here, "..", "kdp_client", "kdp_proto.py")
    yield os.path.join(here, "..", "..", "answers", "kdp_client", "kdp_proto.py")


def load_kdp_proto():
    """Import kdp_proto.py from the sibling kdp_client directory."""
    for path in _candidate_paths():
        if not path or not os.path.isfile(path):
            continue
        spec = importlib.util.spec_from_file_location("_nabu_kdp_proto", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module      # dataclasses need this on 3.12+
        spec.loader.exec_module(module)
        return module, path
    return None, None


KDP_SERIAL, KDP_SERIAL_SRC = (None, None)


def load_kdp_serial():
    """Import kdp_serial.py from the sibling kdp_client directory."""
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.environ.get("KDP_SERIAL_PATH", ""),
        os.path.join(here, "kdp_serial.py"),
        os.path.join(here, "..", "kdp_client", "kdp_serial.py"),
        os.path.join(here, "..", "..", "answers", "kdp_client", "kdp_serial.py"),
    ]
    import importlib.util
    for path in candidates:
        if not path or not os.path.isfile(path):
            continue
        spec = importlib.util.spec_from_file_location("_nabu_kdp_serial", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return module, path
    return None, None


KDP_PROTO, KDP_PROTO_SRC = (None, None)


def _require_serial():
    global KDP_SERIAL, KDP_SERIAL_SRC
    if KDP_SERIAL is None:
        KDP_SERIAL, KDP_SERIAL_SRC = load_kdp_serial()
    if KDP_SERIAL is None:
        raise RuntimeError(
            "cannot locate kdp_serial.py; expected it next to "
            "../kdp_client/kdp_serial.py (set KDP_SERIAL_PATH to override)"
        )
    return KDP_SERIAL


def _require_proto():
    global KDP_PROTO, KDP_PROTO_SRC
    if KDP_PROTO is None:
        KDP_PROTO, KDP_PROTO_SRC = load_kdp_proto()
    if KDP_PROTO is None:
        raise RuntimeError(
            "cannot locate kdp_proto.py; expected it next to "
            "../kdp_client/kdp_proto.py (set KDP_PROTO_PATH to override)"
        )
    return KDP_PROTO


# --------------------------------------------------------------------------
# L2/L3 helpers (thin wrappers around kdp_proto; kept here so the bridge is
# importable even if kdp_proto moves)
# --------------------------------------------------------------------------

def build_eth_frame(kdp_packet: bytes) -> bytes:
    """Wrap a bare KDP packet in Ethernet + IPv4 + UDP (kdp_send() layout)."""
    proto = _require_proto()
    return proto.build_l2l3(kdp_packet)


def extract_kdp_payload(frame: bytes) -> bytes | None:
    """Validate an Ethernet/IP/UDP KDP frame and return the bare KDP packet.

    Mirrors the checks in XNU's ``kdp_poll()``: ether_type, ip.proto, ip_hl and
    the UDP port (either direction).
    """
    proto = _require_proto()
    return proto.parse_l2l3(frame)


# --------------------------------------------------------------------------
# Bridge
# --------------------------------------------------------------------------

# --------------------------------------------------------------------------
# CDC framing codecs
#
# Two wire formats exist:
#   * "serial" -- the *current* device behaviour.  XNU's kdp_send()/kdp_reply()
#     always prepend Ethernet(14)+IP/UDP(28) and hand the whole frame to
#     kdp_send_data(), which in serial mode is kdp_serial_send(); so the
#     payload inside the classic 0xFA/CRC/0xFB frame is already a complete
#     Ethernet frame (kdp_udp.c:544-687, verified on the local tree).
#   * "kdpf"   -- an alternative host-defined format for a future device side
#     that sends raw Ethernet frames directly (no escaping, fixed 12 B header).
#
# Both expose the same surface: ``encode(payload) -> bytes``,
# ``decode(bytes) -> list[payload]`` and a ``.stats`` object with as_dict().
# --------------------------------------------------------------------------

CDC_FRAMINGS = ("serial", "kdpf")


class KdpfCodec:
    """Wrap/unwrap Ethernet frames in the KDPF format (kdp_cdc_frame.py)."""

    name = "kdpf"

    def __init__(self) -> None:
        self.deframer = CdcDeframer()
        self.stats = self.deframer.stats

    def encode(self, payload: bytes) -> bytes:
        return frame_packet(payload)

    def decode(self, data: bytes) -> list[bytes]:
        return self.deframer.feed_bytes(data)

    def maybe_resync(self, idle_seconds: float) -> bool:
        return self.deframer.maybe_resync(idle_seconds)


class SerialCodec:
    """Wrap/unwrap Ethernet frames in Apple's serial-KDP format (kdp_serial.py).

    Reuses the validated implementation from ``kdp_client/kdp_serial.py``
    rather than reimplementing the 0xFA / escape / CRC32 state machine.
    """

    name = "serial"

    def __init__(self, serial_module=None) -> None:
        mod = serial_module if serial_module is not None else _require_serial()
        self.mod = mod
        self.deframer = mod.SerialDeframer()
        self.stats = self.deframer.stats

    def encode(self, payload: bytes) -> bytes:
        return self.mod.frame_packet(payload)

    def decode(self, data: bytes) -> list[bytes]:
        return self.deframer.feed_bytes(data)

    def maybe_resync(self, idle_seconds: float) -> bool:
        # The serial state machine resynchronises on the next 0xFA; there is no
        # declared length to time out against, so nothing extra is needed.
        return False


def make_codec(framing: str):
    if framing == "serial":
        return SerialCodec()
    if framing == "kdpf":
        return KdpfCodec()
    raise ValueError(f"unknown cdc framing {framing!r}; expected one of {CDC_FRAMINGS}")


class BridgeStats:
    def __init__(self) -> None:
        self.udp_tx = 0           # UDP datagrams sent
        self.udp_rx = 0           # UDP datagrams received
        self.kdp_wrap = 0         # UDP -> CDC wraps performed
        self.kdp_unwrap = 0       # CDC -> UDP unwraps performed
        self.passthrough = 0      # datagrams/frames forwarded untouched
        self.udp_oversize = 0
        self.udp_non_kdp = 0      # frames on CDC that are not KDP-over-UDP
        self.udp_no_peer = 0      # device -> UDP with no known peer
        self.writes_blocked = 0   # serial write errors
        self.cdc = None           # set by the bridge to the codec's stats object

    def as_dict(self) -> dict:
        out = {k: v for k, v in self.__dict__.items() if k != "cdc"}
        out["cdc"] = self.cdc.as_dict()
        return out

    def __str__(self) -> str:
        cdc = " ".join(f"{k}={v}" for k, v in self.cdc.as_dict().items())
        return (
            f"cdc[{cdc}] udp_tx={self.udp_tx} udp_rx={self.udp_rx} "
            f"wrap={self.kdp_wrap} unwrap={self.kdp_unwrap} "
            f"passthrough={self.passthrough} oversize={self.udp_oversize} "
            f"non_kdp={self.udp_non_kdp} no_peer={self.udp_no_peer} "
            f"serial_err={self.writes_blocked}"
        )


class KdpNetBridge:
    """Bidirectional UDP <-> CDC bridge.

    Parameters
    ----------
    serial:
        Object exposing the pyserial surface used here: ``read(n) -> bytes``,
        ``write(bytes)``, ``reset_input_buffer()``, ``close()``.  Tests inject
        a virtual port, so the bridge never imports pyserial itself.
    udp_host / udp_port:
        Address to bind.  Port 41139 is ``KDP_REMOTE_PORT``.
    udp_mode:
        ``"kdp"`` (bare KDP datagrams, default) or ``"eth"`` (raw Ethernet).
    """

    def __init__(self, serial, udp_host: str = "0.0.0.0",
                 udp_port: int = KDP_REMOTE_PORT,
                 udp_mode: str = "kdp",
                 cdc_framing: str = "serial",
                 poll_timeout: float = 0.1,
                 cdc_idle_reset: float = 1.0,
                 verbose: bool = False,
                 logger=None) -> None:
        if udp_mode not in ("kdp", "eth"):
            raise ValueError(f"udp_mode must be 'kdp' or 'eth', got {udp_mode!r}")
        self.serial = serial
        self.udp_host = udp_host
        self.udp_port = int(udp_port)
        self.udp_mode = udp_mode
        self.cdc_framing = cdc_framing
        self.codec = make_codec(cdc_framing)
        self.poll_timeout = float(poll_timeout)
        self.cdc_idle_reset = float(cdc_idle_reset)
        self.verbose = verbose
        self.log = logger or self._default_logger

        self.stats = BridgeStats()
        self.stats.cdc = self.codec.stats
        self.stop_event = threading.Event()
        self.peer: tuple[str, int] | None = None
        self._peer_lock = threading.Lock()
        self._serial_lock = threading.Lock()
        self._udp_lock = threading.Lock()
        self._sock: socket.socket | None = None
        self._threads: list[threading.Thread] = []

    # -- logging -----------------------------------------------------------
    @staticmethod
    def _default_logger(level: str, message: str) -> None:
        stamp = time.strftime("%H:%M:%S")
        print(f"[{stamp}] [{level}] {message}", flush=True)

    def _logv(self, message: str) -> None:
        if self.verbose:
            self.log("DEBUG", message)

    # -- lifecycle ---------------------------------------------------------
    def start(self) -> None:
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind((self.udp_host, self.udp_port))
        self._sock.settimeout(self.poll_timeout)
        bound = self._sock.getsockname()
        self.log("INFO", f"UDP listening on {bound[0]}:{bound[1]} "
                         f"(mode={self.udp_mode})")

        self._threads = [
            threading.Thread(target=self._serial_reader, name="cdc-rx", daemon=True),
            threading.Thread(target=self._udp_reader, name="udp-rx", daemon=True),
        ]
        for thread in self._threads:
            thread.start()
        self.log("INFO", "bridge running; Ctrl-C to stop")

    def stop(self) -> None:
        if self.stop_event.is_set():
            return
        self.log("INFO", "stopping ...")
        self.stop_event.set()
        # Closing the socket unblocks recvfrom(); closing the port unblocks read().
        try:
            if self._sock is not None:
                self._sock.close()
        except OSError:
            pass
        try:
            self.serial.close()
        except Exception:
            pass
        for thread in self._threads:
            thread.join(timeout=2.0)
        self.log("INFO", f"stopped: {self.stats}")

    def run_forever(self) -> None:
        self.start()
        try:
            while not self.stop_event.is_set():
                self.stop_event.wait(0.5)
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()

    # -- CDC -> UDP --------------------------------------------------------
    def _serial_reader(self) -> None:
        while not self.stop_event.is_set():
            try:
                data = self.serial.read(4096)
            except Exception as exc:
                if self.stop_event.is_set():
                    return
                self.log("ERROR", f"serial read failed: {exc}")
                self.stats.writes_blocked += 1
                self.stop_event.wait(0.2)
                continue
            if not data:
                # A half-received frame must not swallow the next one forever.
                if self.codec.maybe_resync(self.cdc_idle_reset):
                    self.log("WARN", "CDC idle timeout: dropped a partial frame")
                continue
            try:
                for payload in self.codec.decode(data):
                    self._on_cdc_payload(payload)
            except Exception:
                self.log("ERROR", "deframer crash:\n" + traceback.format_exc())

    def _on_cdc_payload(self, payload: bytes) -> None:
        """Handle one CDC payload (= raw Ethernet frame) from the device.

        Both framings carry the same payload: XNU's kdp_send()/kdp_reply()
        prepend Ethernet+IP/UDP before handing the buffer to the transport
        (kdp_udp.c), so ``serial`` and ``kdpf`` are interchangeable here.
        """
        if len(payload) > MAX_PAYLOAD:
            self.stats.udp_oversize += 1
            self.log("WARN", f"CDC payload {len(payload)} B exceeds MTU, dropped")
            return

        if self.udp_mode == "eth":
            self.stats.passthrough += 1
            self._send_udp(payload)
            return

        kdp = extract_kdp_payload(payload)
        if kdp is None:
            self.stats.udp_non_kdp += 1
            self._logv(f"non-KDP Ethernet frame ({len(payload)} B) ignored")
            return
        self.stats.kdp_unwrap += 1
        self._logv(f"CDC->UDP KDP {len(kdp)} B "
                   f"(hdr={kdp[:8].hex() if len(kdp) >= 8 else kdp.hex()})")
        self._send_udp(kdp)

    # -- UDP -> CDC --------------------------------------------------------
    def _udp_reader(self) -> None:
        while not self.stop_event.is_set():
            assert self._sock is not None
            try:
                data, addr = self._sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                if self.stop_event.is_set():
                    return
                self.log("ERROR", "UDP recv failed")
                continue
            self.stats.udp_rx += 1
            with self._peer_lock:
                self.peer = addr
            try:
                self._on_udp_datagram(data)
            except Exception:
                self.log("ERROR", "UDP handler crash:\n" + traceback.format_exc())

    def _on_udp_datagram(self, data: bytes) -> None:
        if len(data) > MAX_PAYLOAD:
            self.stats.udp_oversize += 1
            self.log("WARN", f"UDP datagram {len(data)} B exceeds MTU, dropped")
            return

        if self.udp_mode == "eth":
            self.stats.passthrough += 1
            self._send_cdc(data)
            return

        # Default mode: lldb sends a bare KDP packet.  Our own kdp_cli sends a
        # complete Ethernet/IP/UDP frame; detect and pass that through.
        if _looks_like_kdp_eth_frame(data):
            self.stats.passthrough += 1
            self._logv(f"UDP->CDC passthrough Ethernet frame ({len(data)} B)")
            self._send_cdc(data)
            return

        if len(data) < 8:
            self.stats.udp_non_kdp += 1
            self._logv(f"UDP datagram {len(data)} B is too short for a KDP header")
            return
        frame = build_eth_frame(data)
        self.stats.kdp_wrap += 1
        self._logv(f"UDP->CDC KDP {len(data)} B -> Ethernet frame {len(frame)} B")
        self._send_cdc(frame)

    # -- sinks -------------------------------------------------------------
    def _send_cdc(self, payload: bytes) -> None:
        if len(payload) > MAX_PAYLOAD:
            # Both device-side receivers cap the buffer at KDP_MAXPACKET.
            self.stats.udp_oversize += 1
            self.log("WARN", f"payload {len(payload)} B exceeds MTU, not sent")
            return
        try:
            wire = self.codec.encode(payload)
        except FrameError as exc:
            self.log("WARN", f"cannot frame payload: {exc}")
            return
        with self._serial_lock:
            try:
                self.serial.write(wire)
            except Exception as exc:
                self.stats.writes_blocked += 1
                self.log("ERROR", f"serial write failed: {exc}")
                return
        self.stats.cdc.frames_tx += 1
        self.stats.cdc.bytes_tx += len(wire)
        self._logv(f"wrote {len(wire)} B to CDC ({self.cdc_framing} frame)")

    def _send_udp(self, payload: bytes) -> None:
        with self._peer_lock:
            peer = self.peer
        if peer is None:
            self.stats.udp_no_peer += 1
            self._logv("device sent data before any UDP peer appeared; dropped")
            return
        with self._udp_lock:
            try:
                self._sock.sendto(payload, peer)
            except OSError as exc:
                self.log("ERROR", f"UDP send to {peer} failed: {exc}")
                return
        self.stats.udp_tx += 1
        self._logv(f"sent {len(payload)} B to {peer[0]}:{peer[1]}")


def _looks_like_kdp_eth_frame(data: bytes) -> bool:
    """Cheap structural test: is this an Ether/IPv4/UDP frame carrying KDP?"""
    if len(data) < KDP_PAYLOAD_OFFSET:
        return False
    if int.from_bytes(data[12:14], "big") != ETHERTYPE_IP:
        return False
    if (data[14] >> 4) != 4 or (data[14] & 0x0F) != 5:
        return False
    if data[23] != IPPROTO_UDP:
        return False
    sport = int.from_bytes(data[34:36], "big")
    dport = int.from_bytes(data[36:38], "big")
    return sport == KDP_REMOTE_PORT or dport == KDP_REMOTE_PORT


# --------------------------------------------------------------------------
# Serial open helper (pyserial is imported lazily so --selftest needs nothing)
# --------------------------------------------------------------------------

def open_serial(url: str, baudrate: int, poll_timeout: float, verbose: bool = False):
    try:
        import serial  # type: ignore
    except ImportError as exc:  # pragma: no cover - environment dependent
        raise SystemExit(
            "pyserial is required for real hardware: "
            "python -m pip install pyserial"
        ) from exc
    port = serial.serial_for_url(url, baudrate=baudrate, timeout=poll_timeout)
    if verbose:
        print(f"[bridge] opened {url} @ {baudrate} baud")
    return port


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="kdp_net_bridge.py",
        description="UDP <-> USB CDC bridge for XNU KDP over raw Ethernet frames.",
    )
    p.add_argument("--port", help="serial port: COM21 (Windows) or /dev/ttyACM0")
    p.add_argument("--baud", type=int, default=115200,
                   help="CDC baud rate (default 115200; the USB CDC bulk "
                        "throughput is independent of this value)")
    p.add_argument("--udp-host", default="0.0.0.0",
                   help="UDP bind address (default 0.0.0.0)")
    p.add_argument("--udp-port", type=int, default=KDP_REMOTE_PORT,
                   help=f"UDP port (default {KDP_REMOTE_PORT} = KDP_REMOTE_PORT)")
    p.add_argument("--udp-mode", choices=("kdp", "eth"), default="kdp",
                   help="'kdp' = bare KDP packets (lldb compatible, default); "
                        "'eth' = raw Ethernet frames")
    p.add_argument("--cdc-framing", choices=CDC_FRAMINGS, default="serial",
                   help="'serial' (default) = the current device behaviour: "
                        "Apple's 0xFA/CRC32/0xFB frame whose payload is already "
                        "a full Ethernet+IP/UDP+KDP frame; 'kdpf' = host-defined "
                        "'KDPF'+len+CRC32 framing for a future raw-frame device")
    p.add_argument("--poll", type=float, default=0.1,
                   help="socket/serial poll timeout in seconds (default 0.1)")
    p.add_argument("--cdc-idle-reset", type=float, default=1.0,
                   help="drop a partial CDC frame after this many seconds "
                        "without bytes (0 disables, default 1.0)")
    p.add_argument("--stats-interval", type=float, default=0.0,
                   help="print statistics every N seconds (0 disables, default)")
    p.add_argument("--verbose", action="store_true", help="log every frame")
    p.add_argument("--selftest", action="store_true",
                   help="run the offline loopback self-test and exit")
    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    if args.selftest:
        import selftest_loopback
        return selftest_loopback.run_all()

    if not args.port:
        print("[bridge] --port is required (or use --selftest)", file=sys.stderr)
        return 2

    # The CDC framing codec must be importable; the L2/L3 helpers are only
    # needed when the UDP side speaks bare KDP packets (--udp-mode kdp).
    try:
        _require_serial()
        print(f"[bridge] using kdp_serial from {KDP_SERIAL_SRC}")
    except RuntimeError as exc:
        print(f"[bridge] {exc}", file=sys.stderr)
        return 2
    if args.udp_mode == "kdp":
        try:
            _require_proto()
            print(f"[bridge] using kdp_proto from {KDP_PROTO_SRC}")
        except RuntimeError as exc:
            print(f"[bridge] {exc}", file=sys.stderr)
            return 2

    try:
        serial_port = open_serial(args.port, args.baud, args.poll, args.verbose)
    except Exception as exc:
        print(f"[bridge] cannot open {args.port}: {exc}", file=sys.stderr)
        return 2

    bridge = KdpNetBridge(
        serial_port,
        udp_host=args.udp_host,
        udp_port=args.udp_port,
        udp_mode=args.udp_mode,
        cdc_framing=args.cdc_framing,
        poll_timeout=args.poll,
        cdc_idle_reset=args.cdc_idle_reset,
        verbose=args.verbose,
    )

    if args.stats_interval > 0:
        def _stats_worker():
            while not bridge.stop_event.wait(args.stats_interval):
                bridge.log("STAT", str(bridge.stats))
        threading.Thread(target=_stats_worker, name="stats", daemon=True).start()

    bridge.run_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
