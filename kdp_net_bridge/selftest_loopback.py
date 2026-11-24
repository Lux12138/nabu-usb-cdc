#!/usr/bin/env python3
"""selftest_loopback.py -- offline self-test for the KDP-over-CDC bridge.

No hardware, no network interfaces and no pyserial are required:

* A ``VirtualSerialPair`` plays the role of the USB CDC bulk pipe.
* A ``FakeKernel`` plays the role of XNU's KDP core plus the device-side
  framing code.  It answers CONNECT / VERSION / HOSTINFO / READMEM64 /
  READPHYSMEM64 / DISCONNECT.
* Two end-to-end scenarios are exercised:

  1. ``--udp-mode eth`` with the *unmodified* ``kdp_client`` session object
     (``kdp_proto.KdpSession``) speaking to the bridge through a small UDP
     transport.  This is the path used by our own KDP client.
  2. ``--udp-mode kdp`` with a minimal "lldb-shaped" raw UDP client that sends
     bare KDP packets (REATTACH -> CONNECT -> VERSION).  This is the path used
     by Apple lldb's ``CommunicationKDP``.

Run::

    python selftest_loopback.py
    python kdp_net_bridge.py --selftest
"""

from __future__ import annotations

import os
import socket
import struct
import sys
import threading
import time
import zlib
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import kdp_cdc_frame as cdc                      # noqa: E402
from kdp_cdc_frame import (                       # noqa: E402
    CdcDeframer,
    FrameError,
    FrameStats,
    MAX_PAYLOAD,
    frame_packet,
    parse_header,
)


# --------------------------------------------------------------------------
# locate kdp_proto.py (sibling kdp_client package)
# --------------------------------------------------------------------------

def load_kdp_proto():
    candidates = [
        os.environ.get("KDP_PROTO_PATH", ""),
        os.path.join(HERE, "kdp_proto.py"),
        os.path.join(HERE, "..", "kdp_client", "kdp_proto.py"),
        os.path.join(HERE, "..", "..", "answers", "kdp_client", "kdp_proto.py"),
    ]
    import importlib.util
    for path in candidates:
        if not path or not os.path.isfile(path):
            continue
        spec = importlib.util.spec_from_file_location("_selftest_kdp_proto", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module      # dataclasses need this on 3.12+
        spec.loader.exec_module(module)
        return module, path
    return None, None


KDP, KDP_SRC = load_kdp_proto()


def load_kdp_serial():
    candidates = [
        os.environ.get("KDP_SERIAL_PATH", ""),
        os.path.join(HERE, "kdp_serial.py"),
        os.path.join(HERE, "..", "kdp_client", "kdp_serial.py"),
    ]
    import importlib.util
    for path in candidates:
        if not path or not os.path.isfile(path):
            continue
        spec = importlib.util.spec_from_file_location("_selftest_kdp_serial", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return module, path
    return None, None


SERIAL, SERIAL_SRC = load_kdp_serial()


# --------------------------------------------------------------------------
# results
# --------------------------------------------------------------------------

class Results:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.lines: list[str] = []

    def ok(self, name: str, detail: str = "") -> None:
        self.passed += 1
        self.lines.append(f"PASS  {name}" + (f"  {detail}" if detail else ""))

    def fail(self, name: str, detail: str = "") -> None:
        self.failed += 1
        self.lines.append(f"FAIL  {name}" + (f"  {detail}" if detail else ""))

    def check(self, name: str, condition: bool, detail: str = "") -> None:
        self.ok(name, detail) if condition else self.fail(name, detail)

    def dump(self) -> None:
        print("\n".join(self.lines))
        total = self.passed + self.failed
        print(f"\n{self.passed}/{total} tests passed")
        if self.failed:
            print("SELFTEST FAILED")


# --------------------------------------------------------------------------
# virtual USB CDC pipe
# --------------------------------------------------------------------------

class VirtualPort:
    """One end of a synchronous in-memory serial link."""

    def __init__(self, name: str) -> None:
        self.name = name
        self.peer: "VirtualPort | None" = None
        self._buf = bytearray()
        self._cv = threading.Condition()
        self._closed = False

    def attach(self, other: "VirtualPort") -> None:
        self.peer = other
        other.peer = self

    # pyserial-compatible surface -----------------------------------------
    def write(self, data: bytes) -> int:
        if self._closed:
            raise OSError("port is closed")
        with self.peer._cv:
            self.peer._buf.extend(data)
            self.peer._cv.notify_all()
        return len(data)

    def read(self, size: int = 1) -> bytes:
        with self._cv:
            deadline = time.monotonic() + 0.5
            while not self._buf and not self._closed:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return b""
                self._cv.wait(remaining)
            out = bytes(self._buf[:size])
            del self._buf[:size]
            return out

    def reset_input_buffer(self) -> None:
        with self._cv:
            self._buf.clear()

    def close(self) -> None:
        self._closed = True
        with self._cv:
            self._cv.notify_all()
        if self.peer is not None:
            self.peer._closed = True
            with self.peer._cv:
                self.peer._cv.notify_all()


def make_pair():
    a, b = VirtualPort("host"), VirtualPort("device")
    a.attach(b)
    return a, b


# --------------------------------------------------------------------------
# fake device (XNU KDP core + device-side KDPF framing)
# --------------------------------------------------------------------------

class FakeKernel:
    """Minimal KDP responder on a VirtualPort.

    ``framing`` selects the CDC wire format, mirroring what the device actually
    does today (``"serial"``) versus the host-defined alternative (``"kdpf"``).
    """

    def __init__(self, port: VirtualPort, key: int = 0x1337C0DE,
                 framing: str = "kdpf") -> None:
        if SERIAL is None and framing == "serial":
            raise RuntimeError("kdp_serial.py not found")
        self.port = port
        self.key = key
        self.framing = framing
        if framing == "serial":
            self.deframer = SERIAL.SerialDeframer()
            self._frame = SERIAL.frame_packet
        elif framing == "kdpf":
            self.deframer = CdcDeframer()
            self._frame = frame_packet
        else:
            raise ValueError(framing)
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.replies = 0
        self.requests: list[bytes] = []

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        self.port.close()
        self.thread.join(timeout=2.0)

    # -- helpers -----------------------------------------------------------
    def _reply(self, request: int, seq: int, body: bytes = b"", *,
               is_reply: bool = True) -> bytes:
        hdr = KDP.pack_hdr(request, is_reply, seq, KDP.KDP_HDR_LEN + len(body),
                           self.key)
        return KDP.build_l2l3(hdr + body)

    def _handle(self, frame: bytes) -> None:
        kdp = KDP.parse_l2l3(frame)
        if kdp is None or len(kdp) < KDP.KDP_HDR_LEN:
            return
        pkt = KDP.unpack_hdr(kdp)
        self.requests.append(kdp)

        if pkt.request == KDP.KDP_CONNECT:
            body = struct.pack("<I", KDP.KDPERR_NO_ERROR)
            out = self._reply(KDP.KDP_CONNECT, pkt.seq, body)
        elif pkt.request == KDP.KDP_REATTACH:
            out = self._reply(KDP.KDP_DISCONNECT, pkt.seq, b"")
        elif pkt.request == KDP.KDP_VERSION_VERB:
            out = self._reply(KDP.KDP_VERSION_VERB, pkt.seq,
                              struct.pack("<IIII", KDP.KDP_VERSION, 1, 0, 0))
        elif pkt.request == KDP.KDP_HOSTINFO:
            out = self._reply(KDP.KDP_HOSTINFO, pkt.seq,
                              struct.pack("<III", 0xFF, 7, 2))
        elif pkt.request == KDP.KDP_READMEM64:
            if len(pkt.body) < 12:
                return
            addr, length = struct.unpack_from("<QI", pkt.body, 0)
            data = bytes((addr + i) & 0xFF for i in range(min(length, 1024)))
            out = self._reply(KDP.KDP_READMEM64, pkt.seq,
                              struct.pack("<I", KDP.KDPERR_NO_ERROR) + data)
        elif pkt.request == KDP.KDP_READPHYSMEM64:
            if len(pkt.body) < 14:
                return
            addr, length = struct.unpack_from("<QI", pkt.body, 0)
            data = bytes((addr ^ i) & 0xFF for i in range(min(length, 1024)))
            out = self._reply(KDP.KDP_READPHYSMEM64, pkt.seq,
                              struct.pack("<I", KDP.KDPERR_NO_ERROR) + data)
        elif pkt.request == KDP.KDP_DISCONNECT:
            out = self._reply(KDP.KDP_DISCONNECT, pkt.seq, b"")
        else:
            out = self._reply(pkt.request, pkt.seq,
                              struct.pack("<I", KDP.KDPERR_NO_ERROR))

        wire = self._frame(out)
        self.port.write(wire)
        self.replies += 1

    def _run(self) -> None:
        while not self.stop_event.is_set():
            data = self.port.read(4096)
            if not data:
                continue
            for payload in self.deframer.feed_bytes(data):
                try:
                    self._handle(payload)
                except Exception:
                    import traceback
                    traceback.print_exc()


# --------------------------------------------------------------------------
# UDP transport used by kdp_proto.KdpSession (the "eth" mode path)
# --------------------------------------------------------------------------

class UdpTransport:
    """pyserial-free transport that carries raw Ethernet frames in UDP."""

    def __init__(self, host: str = "127.0.0.1", port: int = 41139,
                 timeout: float = 0.2) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(timeout)
        self.dest = (host, port)
        self._queue: deque[bytes] = deque()
        self.frames_tx = 0
        self.frames_rx = 0

    def send_packet(self, packet: bytes) -> None:
        self.sock.sendto(packet, self.dest)
        self.frames_tx += 1

    def receive_packet(self, timeout: float = 1.0) -> bytes | None:
        deadline = time.monotonic() + timeout
        while True:
            if self._queue:
                self.frames_rx += 1
                return self._queue.popleft()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.sock.settimeout(min(remaining, 0.2))
            try:
                data, _addr = self.sock.recvfrom(65535)
            except socket.timeout:
                continue
            self._queue.append(data)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# tests: codec
# --------------------------------------------------------------------------

def test_codec(results: Results) -> None:
    # 1. round-trip across the interesting size range
    sizes = [1, 2, 13, 14, 42, 64, 127, 128, 255, 256, 511, 512, 1023,
             1024, 1242, 1500, 1517, 1518]
    bad = 0
    for size in sizes:
        payload = bytes((i * 31 + size) & 0xFF for i in range(size))
        d = CdcDeframer()
        got = d.feed_bytes(frame_packet(payload))
        if got != [payload]:
            bad += 1
    results.check("codec_roundtrip_sizes", bad == 0,
                  f"{len(sizes)} sizes, {bad} failures")

    # 2. split delivery (byte-by-byte and awkward chunking)
    payload = bytes(range(256)) * 4
    wire = frame_packet(payload)
    d = CdcDeframer()
    got = []
    for i in range(0, len(wire), 7):
        got.extend(d.feed_bytes(wire[i:i + 7]))
    results.check("codec_split_delivery", got == [payload],
                  f"{len(wire)} B delivered in 7 B chunks")

    # 3. corrupted payload -> dropped, then resync on the next frame
    good = frame_packet(b"second-frame-payload" * 4)
    bad_frame = bytearray(frame_packet(b"first-frame-payload!!" * 4))
    bad_frame[-3] ^= 0xFF
    d = CdcDeframer()
    got = d.feed_bytes(bytes(bad_frame) + good)
    results.check("codec_crc_drop_and_resync", got == [b"second-frame-payload" * 4],
                  f"crc_errors={d.stats.crc_errors}")
    results.check("codec_crc_counter", d.stats.crc_errors == 1)

    # 4. garbage prefix containing magic look-alikes
    noise = b"\x00KDPFKDPFKDP" + frame_packet(b"after-noise")
    d = CdcDeframer()
    got = d.feed_bytes(noise)
    results.check("codec_resync_garbage", got == [b"after-noise"],
                  f"resyncs={d.stats.resyncs}")

    # 5. bad version / flags / zero length are rejected
    payload = b"x" * 32
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    base = struct.pack("<4sBBHI", cdc.MAGIC, 1, 0, len(payload), crc)
    variants = {
        "version": struct.pack("<4sBBHI", cdc.MAGIC, 2, 0, len(payload), crc),
        "flags": struct.pack("<4sBBHI", cdc.MAGIC, 1, 1, len(payload), crc),
        "zero_len": struct.pack("<4sBBHI", cdc.MAGIC, 1, 0, 0, crc),
        "huge_len": struct.pack("<4sBBHI", cdc.MAGIC, 1, 0, 60000, crc),
    }
    for name, header in variants.items():
        d = CdcDeframer()
        got = d.feed_bytes(header + payload)
        results.check(f"codec_reject_{name}", got == [] and
                      (d.stats.bad_headers + d.stats.oversize_drops) >= 1)

    # 6. a frame that is completed but corrupt is dropped; the next frame
    #    arrives intact and is recovered.
    d = CdcDeframer()
    got = d.feed_bytes(base + payload[:16] + b"\xaa" * 16 + frame_packet(b"next"))
    results.check("codec_corrupt_then_next", got == [b"next"],
                  f"crc_errors={d.stats.crc_errors}")

    # 7. an idle timeout rescues a truncated frame (device dropped bytes)
    d = CdcDeframer()
    d.feed_bytes(base + payload[:16])
    d.last_byte_at = time.monotonic() - 1.0
    reset = d.maybe_resync(0.5)
    got = d.feed_bytes(frame_packet(b"after-truncation"))
    results.check("codec_idle_resync", reset and got == [b"after-truncation"],
                  f"reset={reset} resyncs={d.stats.resyncs}")

    # 7. empty / oversize payload refused by the framer
    try:
        frame_packet(b"")
        results.check("codec_refuse_empty", False, "no exception raised")
    except FrameError:
        results.check("codec_refuse_empty", True)
    try:
        frame_packet(b"x" * (MAX_PAYLOAD + 1))
        results.check("codec_refuse_oversize", False, "no exception raised")
    except FrameError:
        results.check("codec_refuse_oversize", True)

    # 8. worst-case overhead: framing is a fixed 12 bytes
    results.check("codec_overhead_fixed",
                  len(frame_packet(b"y" * 100)) == 112)

    # 9. header parser agreement with a hand-built header
    parsed = parse_header(base)
    results.check("codec_parse_header", parsed == (0, len(payload), crc))


# --------------------------------------------------------------------------
# tests: the *serial* CDC framing (the format the device actually uses today)
# --------------------------------------------------------------------------

def test_serial_codec(results: Results) -> None:
    if SERIAL is None:
        results.fail("serial_codec", "kdp_serial.py not found")
        return
    import kdp_net_bridge as bridge

    # 1. round-trip, including payloads that contain bytes needing escaping.
    #    The receiver's dsBuffer is 1518 B, so the largest accepted payload is
    #    1517 B (a 1518 B payload hits the overflow guard on its last byte).
    sizes = [1, 13, 42, 64, 127, 255, 511, 512, 1000, 1242, 1517]
    bad = 0
    for size in sizes:
        payload = bytes((i * 31 + size) & 0xFF for i in range(size))
        wire = SERIAL.frame_packet(payload)
        d = SERIAL.SerialDeframer()
        got = d.feed_bytes(wire)
        if got != [payload]:
            bad += 1
    results.check("serial_roundtrip_sizes", bad == 0,
                  f"{len(sizes)} sizes incl. 0xFA/0xFB/0xFE/0x0A-heavy payloads")

    # 2. every escapable byte survives a round trip
    payload = bytes([0xFA, 0xFB, 0xFE, 0x0A, 0x00, 0xFF]) * 20
    d = SERIAL.SerialDeframer()
    got = d.feed_bytes(SERIAL.frame_packet(payload))
    results.check("serial_escape_roundtrip", got == [payload])

    # 3. corruption -> CRC drop, then resync on the next frame
    good = SERIAL.frame_packet(b"second-serial-frame" * 4)
    corrupt = bytearray(SERIAL.frame_packet(b"first-serial-frame!" * 4))
    corrupt[-4] ^= 0xFF                       # damage inside the payload
    d = SERIAL.SerialDeframer()
    got = d.feed_bytes(bytes(corrupt) + good)
    results.check("serial_crc_drop_and_resync",
                  got == [b"second-serial-frame" * 4],
                  f"crc_errors={d.stats.crc_errors}")
    results.check("serial_stats_counters", d.stats.crc_errors == 1)

    # 4. a payload > 1518 B is dropped by the receiver (dsBuffer limit)
    huge = b"x" * 1600
    d = SERIAL.SerialDeframer()
    got = d.feed_bytes(SERIAL.frame_packet(huge))
    results.check("serial_overflow_drop", got == [] and d.stats.overflow_drops >= 1,
                  f"overflow_drops={d.stats.overflow_drops}")

    # 5. the bridge codec must reuse the validated implementation verbatim
    codec = bridge.SerialCodec(serial_module=SERIAL)
    payload = b"\xfa\xfb\xfe\n" + os.urandom(60)
    results.check("serial_codec_matches_kdp_serial",
                  codec.encode(payload) == SERIAL.frame_packet(payload))

    # 6. an Ethernet KDP frame round-trips through the serial codec
    if KDP is not None:
        net = KDP.build_l2l3(
            KDP.pack_hdr(KDP.KDP_CONNECT, False, 0, KDP.KDP_HDR_LEN + 12, 0xC0DE)
            + struct.pack("<HH", 41140, 41145) + b"greetings\x00")
        codec_deframer = SERIAL.SerialDeframer()
        out = codec_deframer.feed_bytes(codec.encode(net))
        results.check("serial_eth_frame_roundtrip", out == [net])


# --------------------------------------------------------------------------
# tests: parity with kdp_proto (L2/L3 layer)
# --------------------------------------------------------------------------

def test_l2l3_parity(results: Results) -> None:
    if KDP is None:
        results.fail("l2l3_parity", "kdp_proto.py not found")
        return
    import kdp_net_bridge as bridge

    mismatch = 0
    for size in (12, 22, 64, 200, 1200):
        kdp = bytes((i * 17 + 3) & 0xFF for i in range(size))
        mine = bridge.build_eth_frame(kdp)
        theirs = KDP.build_l2l3(kdp)
        if mine != theirs:
            mismatch += 1
        back = bridge.extract_kdp_payload(mine)
        if back != kdp:
            mismatch += 1
    results.check("l2l3_parity_with_kdp_proto", mismatch == 0,
                  "bridge wrappers == kdp_proto.build_l2l3/parse_l2l3")

    results.check("l2l3_offset_42",
                  KDP.KDP_PAYLOAD_OFFSET == 42)


# --------------------------------------------------------------------------
# tests: end-to-end, 'eth' mode, unmodified kdp_client session
# --------------------------------------------------------------------------

def _free_udp_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def test_end_to_end_eth_mode(results: Results) -> None:
    if KDP is None:
        results.fail("e2e_eth_mode", "kdp_proto.py not found")
        return
    import kdp_net_bridge as bridge

    framings = [f for f in ("kdpf", "serial") if f != "serial" or SERIAL]
    for framing in framings:
        tag = f"[{framing}]"
        udp_port = _free_udp_port()
        host_side, device_side = make_pair()
        kernel = FakeKernel(device_side, key=0x1337C0DE, framing=framing)
        kernel.start()

        br = bridge.KdpNetBridge(host_side, udp_host="127.0.0.1",
                                 udp_port=udp_port, udp_mode="eth",
                                 cdc_framing=framing,
                                 poll_timeout=0.05, verbose=False)
        br.start()
        try:
            transport = UdpTransport(port=udp_port, timeout=0.2)
            sess = KDP.KdpSession(transport, reply_port=41140,
                                  exception_port=41145, key=0x1337C0DE,
                                  timeout=1.0, retries=5)
            reply = sess.connect()
            results.check(f"e2e_eth_connect{tag}",
                          reply.error == KDP.KDPERR_NO_ERROR,
                          f"key=0x{sess.key:08x} seq={sess.seq}")
            ver = sess.version()
            results.check(f"e2e_eth_version{tag}",
                          ver["version"] == KDP.KDP_VERSION,
                          f"feature=0x{ver['feature']:x}")
            hi = sess.hostinfo()
            results.check(f"e2e_eth_hostinfo{tag}", hi["cpu_count"] == 8,
                          f"cpus_mask=0x{hi['cpus_mask']:x}")
            data = sess.readmem64(0xfffffe0012345000, 256)
            want = bytes((0xfffffe0012345000 + i) & 0xFF for i in range(256))
            results.check(f"e2e_eth_readmem{tag}", data == want, f"{len(data)} B")
            phys = sess.readphys64(0x80000000, 64)
            want_phys = bytes((0x80000000 ^ i) & 0xFF for i in range(64))
            results.check(f"e2e_eth_readphys{tag}", phys == want_phys,
                          f"{len(phys)} B")
            sess.disconnect()
            transport.close()
            results.check(f"e2e_eth_kernel_served{tag}", kernel.replies >= 6,
                          f"replies={kernel.replies} frames_tx={br.stats.cdc.frames_tx} "
                          f"frames_rx={br.stats.cdc.frames_rx}")
        finally:
            br.stop()
            kernel.stop()


# --------------------------------------------------------------------------
# tests: end-to-end, 'kdp' mode, lldb-shaped raw client
# --------------------------------------------------------------------------

def _recv_kdp(sock: socket.socket, timeout: float = 2.0) -> bytes | None:
    sock.settimeout(timeout)
    try:
        data, _addr = sock.recvfrom(65535)
    except socket.timeout:
        return None
    return data


def test_end_to_end_kdp_mode(results: Results) -> None:
    if KDP is None:
        results.fail("e2e_kdp_mode", "kdp_proto.py not found")
        return
    import kdp_net_bridge as bridge

    framings = [f for f in ("kdpf", "serial") if f != "serial" or SERIAL]
    for framing in framings:
        tag = f"[{framing}]"
        udp_port = _free_udp_port()
        host_side, device_side = make_pair()
        kernel = FakeKernel(device_side, key=0xA11CE5, framing=framing)
        kernel.start()

        br = bridge.KdpNetBridge(host_side, udp_host="127.0.0.1",
                                 udp_port=udp_port, udp_mode="kdp",
                                 cdc_framing=framing,
                                 poll_timeout=0.05, verbose=False)
        br.start()

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        reply_port = sock.getsockname()[1]

        try:
            # lldb shape: REATTACH(seq=0) then CONNECT(seq=0).
            key = 0xA11CE5
            reattach = (KDP.pack_hdr(KDP.KDP_REATTACH, False, 0,
                                     KDP.KDP_HDR_LEN + 2, key)
                        + struct.pack("<H", reply_port))
            sock.sendto(reattach, ("127.0.0.1", udp_port))
            r = _recv_kdp(sock)
            results.check(f"e2e_kdp_reattach_unwrapped{tag}",
                          r is not None and (r[0] & 0x80) and
                          (r[0] & 0x7F) == KDP.KDP_DISCONNECT,
                          f"reply={r[:12].hex() if r else None}")

            connect = (KDP.pack_hdr(KDP.KDP_CONNECT, False, 0,
                                     KDP.KDP_HDR_LEN + 12, key)
                       + struct.pack("<HH", reply_port, 41145) + b"greetings\x00")
            sock.sendto(connect, ("127.0.0.1", udp_port))
            r = _recv_kdp(sock)
            ok = False
            detail = f"reply={r[:12].hex() if r else None}"
            if r is not None and len(r) >= 12:
                pkt = KDP.unpack_hdr(r)
                err = struct.unpack_from("<I", r, 8)[0]
                ok = (pkt.is_reply and pkt.request == KDP.KDP_CONNECT
                      and pkt.key == key and err == KDP.KDPERR_NO_ERROR)
                detail += f" seq={pkt.seq} err={err}"
            results.check(f"e2e_kdp_connect_bare{tag}", ok, detail)

            version = KDP.pack_hdr(KDP.KDP_VERSION_VERB, False, 1,
                                    KDP.KDP_HDR_LEN, key)
            sock.sendto(version, ("127.0.0.1", udp_port))
            r = _recv_kdp(sock)
            ok = r is not None and len(r) >= 24 and (r[0] & 0x80)
            results.check(f"e2e_kdp_version_bare{tag}", ok,
                          f"reply={r[:24].hex() if r else None}")

            # A full Ethernet frame sent by our own client must pass through.
            full = KDP.build_l2l3(
                KDP.pack_hdr(KDP.KDP_HOSTINFO, False, 2, KDP.KDP_HDR_LEN, key))
            sock.sendto(full, ("127.0.0.1", udp_port))
            r = _recv_kdp(sock)
            results.check(f"e2e_kdp_eth_passthrough{tag}",
                          r is not None and (r[0] & 0x80) and
                          (r[0] & 0x7F) == KDP.KDP_HOSTINFO,
                          f"reply={r[:12].hex() if r else None}")
        finally:
            sock.close()
            br.stop()
            kernel.stop()


# --------------------------------------------------------------------------
# entry point
# --------------------------------------------------------------------------

def run_all(results: Results | None = None) -> int:
    res = results if results is not None else Results()
    started = time.time()
    test_codec(res)
    test_serial_codec(res)
    test_l2l3_parity(res)
    test_end_to_end_eth_mode(res)
    test_end_to_end_kdp_mode(res)
    res.dump()
    print(f"elapsed: {time.time() - started:.2f}s")
    return 0 if res.failed == 0 else 1


def main(argv=None) -> int:
    # kdp_net_bridge.py calls main([args]); tolerate that as well as a plain CLI.
    if argv and not isinstance(argv, (list, tuple)):
        argv = None
    if argv:
        import argparse
        p = argparse.ArgumentParser(prog="selftest_loopback.py")
        p.add_argument("--verbose", action="store_true")
        p.parse_args(argv)
    return run_all()


if __name__ == "__main__":
    sys.exit(main())
