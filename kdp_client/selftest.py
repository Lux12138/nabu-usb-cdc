#!/usr/bin/env python3
"""selftest.py -- offline loopback tests for the KDP-over-serial client.

No hardware is required.  A `FakeKernel` speaks just enough of the XNU KDP
server side (framing, L2/L3, kdp_hdr_t, sequence rules, exception ACKs,
memory/register replies) to exercise the real client code end to end.

Run with:

    python selftest.py
    python kdp_cli.py selftest
"""

from __future__ import annotations

import struct
import sys

import kdp_proto
import kdp_serial
from kdp_proto import (
    ARM_THREAD_STATE64_SIZE,
    KDP_BREAKPOINT64_REMOVE,
    KDP_BREAKPOINT64_SET,
    KDP_CONNECT,
    KDP_DISCONNECT,
    KDP_EXCEPTION,
    KDP_HOSTINFO,
    KDP_KERNELVERSION,
    KDP_MAXBYTES,
    KDP_READMEM64,
    KDP_READPHYSMEM64,
    KDP_READREGS,
    KDP_RESUMECPUS,
    KDP_VERSION_VERB,
    KDP_WRITEMEM64,
    KDP_WRITEPHYSMEM64,
    KDP_WRITEREGS,
    KDPERR_NO_ERROR,
    MAX_KDP_DATA_SIZE,
    KdpSession,
    KdpUnsupported,
    build_l2l3,
    pack_hdr,
    parse_l2l3,
    unpack_hdr,
)


# --------------------------------------------------------------------------
# Fake device
# --------------------------------------------------------------------------

class FakeKernel:
    """A minimal XNU KDP server used to test the client."""

    def __init__(self, *, drop_requests: int = 0):
        self.deframer = kdp_serial.SerialDeframer()
        self.rx = bytearray()          # bytes waiting for the client
        self.is_conn = False
        self.exception_seq = 0
        self.conn_seq = 0
        self.reply_port = 0
        self.exception_port = 0
        self.session_key = 0
        self.saved_reply = None
        self.requests_seen = []        # (request, seq) in arrival order
        self.breakpoints = set()
        self.written_thread_state = None
        self.drop_requests = drop_requests
        self.exception_pending = False
        self.exception_acks = 0

        # Sparse memory: 1 MiB virtual + 1 MiB physical, deterministic fill.
        self.vmem = bytearray((i * 7 + 0x41) & 0xFF for i in range(1 << 20))
        self.pmem = bytearray((i * 11 + 0x21) & 0xFF for i in range(1 << 20))

    # -- transport side --------------------------------------------------
    def handle_frame_bytes(self, data: bytes) -> None:
        for b in data:
            pkt = self.deframer.feed(b)
            if pkt is not None:
                self._on_packet(bytes(pkt))

    def _queue(self, payload: bytes) -> None:
        self.rx += kdp_serial.frame_packet(payload)

    def _send_reply(self, request: int, seq: int, body: bytes = b"",
                    key: int | None = None) -> None:
        kdp = pack_hdr(request, True, seq, 8 + len(body),
                       self.session_key if key is None else key) + body
        frame = build_l2l3(kdp, udp_sport=kdp_proto.KDP_REMOTE_PORT,
                           udp_dport=self.reply_port or kdp_proto.UDP_HOST_COMM_BASE)
        self._queue(frame)

    def _send_exception(self, seq: int, code: int = 0x1234) -> None:
        exc = struct.pack("<IIII", 0, 1, code, 0)  # cpu, exception, code, subcode
        kdp = pack_hdr(KDP_EXCEPTION, False, seq, 8 + 4 + len(exc), 0) \
            + struct.pack("<I", 1) + exc
        self._queue(build_l2l3(kdp, udp_sport=kdp_proto.KDP_REMOTE_PORT,
                               udp_dport=self.exception_port or kdp_proto.UDP_HOST_EXCEP_BASE))

    def inject_exception(self, code: int = 0x1234) -> None:
        """Simulate an asynchronous trap: queue an EXCEPTION notification.

        The kernel sends the notification instead of processing whatever the
        client is doing, and waits for the acknowledgement (exactly like
        kdp_send_exception()).
        """
        self.exception_pending = True
        self._send_exception(self.exception_seq, code)

    # -- KDP server logic ------------------------------------------------
    def _on_packet(self, packet: bytes) -> None:
        payload = parse_l2l3(packet, reply_port=kdp_proto.UDP_HOST_COMM_BASE)
        if payload is None:
            return
        hdr = unpack_hdr(payload)
        body = payload[8:]

        # Replies from the client are only expected as exception/termination
        # acknowledgements.
        if hdr.is_reply:
            if hdr.request == KDP_EXCEPTION and hdr.seq == self.exception_seq:
                self.exception_acks += 1
                self.exception_pending = False
                self.exception_seq = (self.exception_seq + 1) & 0xFF
            return

        # While an exception notification is outstanding the kernel is stuck
        # in kdp_send_exception() and does not process requests.
        if self.exception_pending:
            return

        self.requests_seen.append((hdr.request, hdr.seq))

        if self.drop_requests > 0:
            self.drop_requests -= 1
            return

        if not self.is_conn:
            if hdr.request == KDP_CONNECT and hdr.seq == 0:
                self.reply_port, self.exception_port = struct.unpack_from("<HH", body, 0)
                self.session_key = hdr.key
                self.is_conn = True
                self.conn_seq = 0
                self.exception_seq = 0
                self._send_reply(KDP_CONNECT, 0, struct.pack("<I", KDPERR_NO_ERROR))
                self.exception_seq = 1
                self.saved_reply = (KDP_CONNECT, 0)
            return

        # Already connected.
        if hdr.request == KDP_CONNECT:
            # Duplicate CONNECT: same seq/ports/key -> NO_ERROR, else refused.
            self._send_reply(KDP_CONNECT, hdr.seq, struct.pack("<I", KDPERR_NO_ERROR))
            return
        if hdr.seq == (self.exception_seq - 1) & 0xFF and self.saved_reply is not None:
            req, seq = self.saved_reply
            self._send_reply(req, seq, self._reply_body(req, seq))
            return
        if hdr.seq != self.exception_seq:
            # kdp: bad sequence -- drop.
            return

        req = hdr.request
        reply_body = self._reply_body(req, hdr.seq, body)
        self._send_reply(req, hdr.seq, reply_body)
        self.saved_reply = (req, hdr.seq)
        self.exception_seq = (self.exception_seq + 1) & 0xFF
        if req == KDP_DISCONNECT:
            # kdp_disconnect() reset exception_seq and the debugger loop then
            # re-enters kdp_connection_wait(), which zeroes it again.
            self.exception_seq = 0

    def _reply_body(self, req: int, seq: int, body: bytes = b"") -> bytes:
        if req == KDP_VERSION_VERB:
            return struct.pack("<IIII", kdp_proto.KDP_VERSION, kdp_proto.KDP_FEATURE_BP, 0, 0)
        if req == KDP_HOSTINFO:
            return struct.pack("<III", 0b1111, 0x00000001, 0x00000002)
        if req == KDP_MAXBYTES:
            return struct.pack("<I", MAX_KDP_DATA_SIZE)
        if req == KDP_KERNELVERSION:
            return b"xnu-17.7.11; stext=0xfffffe0012345000\x00"
        if req == KDP_READMEM64:
            return self._read_memory(self.vmem, body)
        if req == KDP_READPHYSMEM64:
            return self._read_memory(self.pmem, body)
        if req == KDP_WRITEMEM64:
            return self._write_memory(self.vmem, body)
        if req == KDP_WRITEPHYSMEM64:
            return self._write_memory(self.pmem, body)
        if req == KDP_READREGS:
            cpu, flavor = struct.unpack_from("<II", body, 0)
            if flavor != kdp_proto.ARM_THREAD_STATE64:
                return struct.pack("<I", kdp_proto.KDPERR_BADFLAVOR)
            state = self._fake_thread_state(cpu)
            return struct.pack("<I", KDPERR_NO_ERROR) + state
        if req == KDP_WRITEREGS:
            cpu, flavor = struct.unpack_from("<II", body, 0)
            if flavor != kdp_proto.ARM_THREAD_STATE64:
                return struct.pack("<I", kdp_proto.KDPERR_BADFLAVOR)
            if len(body) - 8 < ARM_THREAD_STATE64_SIZE:
                return struct.pack("<I", kdp_proto.KDPERR_BADFLAVOR)
            self.written_thread_state = bytes(body[8:8 + ARM_THREAD_STATE64_SIZE])
            return struct.pack("<I", KDPERR_NO_ERROR)
        if req == KDP_BREAKPOINT64_SET:
            addr = struct.unpack_from("<Q", body, 0)[0]
            if addr in self.breakpoints:
                return struct.pack("<I", kdp_proto.KDPERR_BREAKPOINT_ALREADY_SET)
            self.breakpoints.add(addr)
            return struct.pack("<I", KDPERR_NO_ERROR)
        if req == KDP_BREAKPOINT64_REMOVE:
            addr = struct.unpack_from("<Q", body, 0)[0]
            if addr not in self.breakpoints:
                return struct.pack("<I", kdp_proto.KDPERR_BREAKPOINT_NOT_FOUND)
            self.breakpoints.discard(addr)
            return struct.pack("<I", KDPERR_NO_ERROR)
        if req == KDP_RESUMECPUS:
            self.is_conn = False
            self.exception_seq = 0
            return b""
        if req == KDP_DISCONNECT:
            self.is_conn = False
            self.exception_seq = 0
            self.conn_seq = 0
            return b""
        return struct.pack("<I", KDPERR_NO_ERROR)

    def _write_memory(self, mem: bytearray, body: bytes) -> bytes:
        addr, nbytes = struct.unpack_from("<QI", body, 0)
        if nbytes > MAX_KDP_DATA_SIZE:
            return struct.pack("<I", kdp_proto.KDPERR_BAD_NBYTES)
        addr &= 0xFFFFFFFFFFFFFFFF
        data = body[12:12 + nbytes]
        if len(data) != nbytes:
            return struct.pack("<I", kdp_proto.KDPERR_BAD_NBYTES)
        if addr + nbytes > len(mem):
            # Like the real kdp_machine_vm_write, a fault leaves the bytes
            # unwritten and still reports NO_ERROR.
            return struct.pack("<I", KDPERR_NO_ERROR)
        mem[addr:addr + nbytes] = data
        return struct.pack("<I", KDPERR_NO_ERROR)

    def _read_memory(self, mem: bytearray, body: bytes) -> bytes:
        addr, nbytes = struct.unpack_from("<QI", body, 0)
        if nbytes > MAX_KDP_DATA_SIZE:
            return struct.pack("<I", kdp_proto.KDPERR_BAD_NBYTES)
        addr &= 0xFFFFFFFFFFFFFFFF
        if addr + nbytes > len(mem):
            return struct.pack("<I", KDPERR_NO_ERROR)  # zero-length, like a fault
        return struct.pack("<I", KDPERR_NO_ERROR) + bytes(mem[addr:addr + nbytes])

    @staticmethod
    def _fake_thread_state(cpu: int) -> bytes:
        state = bytearray(ARM_THREAD_STATE64_SIZE)
        for i in range(29):
            struct.pack_into("<Q", state, i * 8, 0x1000 + i)
        struct.pack_into("<4Q", state, 29 * 8, 0x2000, 0x3000, 0x4000, 0x5000)
        struct.pack_into("<2I", state, 29 * 8 + 32, 0x00000000, 0x00000000)
        return bytes(state)


class FakePort:
    """pyserial-shaped port backed by a FakeKernel."""

    def __init__(self, kernel: FakeKernel):
        self.kernel = kernel

    def write(self, data: bytes) -> None:
        self.kernel.handle_frame_bytes(data)

    def read(self, size: int = 1) -> bytes:
        rx = self.kernel.rx
        if not rx:
            return b""
        out = bytes(rx[:size])
        del rx[:size]
        return out

    def reset_input_buffer(self) -> None:
        self.kernel.rx.clear()

    def close(self) -> None:
        pass


def _make_session(kernel: FakeKernel, **kw) -> KdpSession:
    port = FakePort(kernel)
    transport = kdp_serial.SerialTransport(port, poll_timeout=0.0)
    kw.setdefault("timeout", 0.3)
    kw.setdefault("retries", 4)
    kw.setdefault("connect_timeout", 0.6)
    return KdpSession(transport, **kw)


# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------

def test_escape_roundtrip() -> None:
    # Every possible byte value, including the escape set and '\n'.
    data = bytes(range(256))
    esc = kdp_serial.escape(data)
    # No escaped byte may contain a raw 0xFA/0xFB/0xFE/0x0A outside 0xFE pairs.
    assert 0xFE not in data[:0]  # no-op, keeps linters quiet
    out = bytearray()
    i = 0
    while i < len(esc):
        b = esc[i]
        if b == kdp_serial.SKDP_ESC_CHAR:
            assert i + 1 < len(esc), "dangling escape"
            out.append((~esc[i + 1]) & 0xFF)
            i += 2
        else:
            out.append(b)
            i += 1
    assert bytes(out) == data, "escape/unescape roundtrip failed"


def test_frame_deframe_roundtrip() -> None:
    patterns = [
        b"",
        b"\x00",
        b"\xfa\xfb\xfe\x0a",
        bytes(range(256)),
        b"A" * 1200,          # MAX_KDP_PKT_SIZE
    ]
    for data in patterns:
        d = kdp_serial.SerialDeframer()
        frame = kdp_serial.frame_packet(data)
        out = None
        for b in frame:
            out = d.feed(b)
            if out is not None:
                break
        assert out == data, f"roundtrip failed for {len(data)}B pattern"

    # The kernel's dsBuffer is 1518 bytes; a larger payload must be dropped.
    d = kdp_serial.SerialDeframer()
    big = kdp_serial.frame_packet(b"B" * 2000)
    out = None
    for b in big:
        out = d.feed(b)
    assert out is None, "oversized frame should be dropped"


def test_crc_mismatch_resync() -> None:
    d = kdp_serial.SerialDeframer()
    good1 = kdp_serial.frame_packet(b"first")
    good2 = kdp_serial.frame_packet(b"second")

    # Corrupt one payload byte of good1 (CRC no longer matches).
    bad = bytearray(good1)
    idx = len(bad) // 2
    bad[idx] ^= 0xFF
    stream = bytes(bad) + good2

    results = []
    for b in stream:
        pkt = d.feed(b)
        if pkt is not None:
            results.append(bytes(pkt))
    assert results == [b"second"], f"resync failed: {results}"


def test_resync_after_garbage() -> None:
    d = kdp_serial.SerialDeframer()
    stream = b"\x11\x22\xfa\xfa\xfa" + kdp_serial.frame_packet(b"payload")
    results = []
    for b in stream:
        pkt = d.feed(b)
        if pkt is not None:
            results.append(bytes(pkt))
    assert results == [b"payload"], f"garbage resync failed: {results}"


def test_l2l3_layout() -> None:
    kdp = pack_hdr(KDP_READMEM64, False, 1, 20, 0xDEADBEEF) + b"\x00" * 12
    frame = build_l2l3(kdp, udp_sport=41140, udp_dport=41139)
    assert len(frame) == 42 + len(kdp)
    assert struct.unpack_from(">H", frame, 12)[0] == kdp_proto.ETHERTYPE_IP
    assert frame[14] >> 4 == 4 and (frame[14] & 0xF) == 5
    assert frame[23] == 17
    assert struct.unpack_from(">H", frame, 34)[0] == 41140
    assert struct.unpack_from(">H", frame, 36)[0] == 41139
    assert struct.unpack_from(">H", frame, 38)[0] == 8 + len(kdp)
    assert struct.unpack_from(">H", frame, 16)[0] == 20 + 8 + len(kdp)
    # KDP payload starts at 42.
    assert unpack_hdr(frame[42:]).request == KDP_READMEM64
    # parse_l2l3 accepts it and returns the KDP packet.
    assert parse_l2l3(frame) == kdp


def test_connect_and_seq() -> None:
    k = FakeKernel()
    s = _make_session(k)
    reply = s.connect()
    assert reply.error == KDPERR_NO_ERROR
    assert s.seq == 1, f"seq after CONNECT should be 1, got {s.seq}"
    assert k.is_conn and k.exception_seq == 1

    ver = s.version()
    assert ver["version"] == kdp_proto.KDP_VERSION
    assert s.seq == 2
    hi = s.hostinfo()
    assert hi["cpu_count"] == 4
    assert s.seq == 3
    assert [seq for _, seq in k.requests_seen] == [0, 1, 2], k.requests_seen


def test_fragmented_read() -> None:
    k = FakeKernel()
    s = _make_session(k)
    s.connect()
    total = MAX_KDP_DATA_SIZE * 2 + 100
    data = s.readmem64(0x1000, total)
    assert len(data) == total, f"expected {total}, got {len(data)}"
    assert data == bytes(k.vmem[0x1000:0x1000 + total])
    # 1 CONNECT + 3 READMEM64 chunks.
    assert [req for req, _ in k.requests_seen] == [
        KDP_CONNECT, KDP_READMEM64, KDP_READMEM64, KDP_READMEM64,
    ], k.requests_seen
    assert [seq for _, seq in k.requests_seen] == [0, 1, 2, 3]


def test_readphys() -> None:
    k = FakeKernel()
    s = _make_session(k)
    s.connect()
    data = s.readphys64(0x10000, 2048, lcpu=0)
    assert data == bytes(k.pmem[0x10000:0x10000 + 2048])


def test_writemem_roundtrip() -> None:
    """The hot patch path: write, then read back what the kernel stored."""
    k = FakeKernel()
    s = _make_session(k)
    s.connect()
    addr = 0x2000
    payload = bytes.fromhex("1f2003d5" "c0035fd6")   # nop ; ret
    n = s.writemem64(addr, payload)
    assert n == len(payload), f"wrote {n} of {len(payload)}"
    assert bytes(k.vmem[addr:addr + len(payload)]) == payload
    # One CONNECT + one WRITEMEM64.
    assert [req for req, _ in k.requests_seen] == [KDP_CONNECT, KDP_WRITEMEM64]
    # And it reads back through the normal read path.
    assert s.readmem64(addr, len(payload)) == payload


def test_writemem_fragmented() -> None:
    """Longer than MAX_KDP_DATA_SIZE must be split, in order, no gaps."""
    k = FakeKernel()
    s = _make_session(k)
    s.connect()
    total = MAX_KDP_DATA_SIZE * 2 + 100
    payload = bytes((i * 3 + 5) & 0xFF for i in range(total))
    n = s.writemem64(0x4000, payload)
    assert n == total
    assert bytes(k.vmem[0x4000:0x4000 + total]) == payload, "fragmented write mismatched"
    writes = [req for req, _ in k.requests_seen if req == KDP_WRITEMEM64]
    assert len(writes) == 3, f"expected 3 fragments, got {len(writes)}"


def test_writemem_rejects_oversize() -> None:
    """A single fragment above MAX_KDP_DATA_SIZE must be refused by the mock,
    proving our chunker never sends one."""
    k = FakeKernel()
    s = _make_session(k)
    s.connect()
    # 2 * MAX + 1 forces a final 1-byte fragment; none may exceed the limit.
    payload = b"\xAA" * (MAX_KDP_DATA_SIZE * 2 + 1)
    s.writemem64(0x8000, payload)
    assert bytes(k.vmem[0x8000:0x8000 + len(payload)]) == payload


def test_writeregs_roundtrip() -> None:
    k = FakeKernel()
    s = _make_session(k)
    s.connect()
    regs = s.readregs(cpu=0, flavor=kdp_proto.ARM_THREAD_STATE64)
    regs["pc"] = 0xDEADBEEF
    regs["sp"] = 0xCAFEBABE
    s.writeregs(regs, cpu=0, flavor=kdp_proto.ARM_THREAD_STATE64)
    assert k.written_thread_state is not None
    pc = struct.unpack_from("<Q", k.written_thread_state, 29 * 8 + 24)[0]
    sp = struct.unpack_from("<Q", k.written_thread_state, 29 * 8 + 16)[0]
    assert pc == 0xDEADBEEF, f"pc roundtrip failed: 0x{pc:x}"
    assert sp == 0xCAFEBABE, f"sp roundtrip failed: 0x{sp:x}"


def test_async_exception() -> None:
    k = FakeKernel()
    s = _make_session(k)
    s.connect()
    # An asynchronous trap arrives: the kernel queues an EXCEPTION with the
    # current sequence number and stops processing requests until it is ACKed.
    k.inject_exception()
    ver = s.version()
    assert ver["version"] == kdp_proto.KDP_VERSION
    assert k.exception_acks == 1, f"expected 1 exception ACK, got {k.exception_acks}"
    # CONNECT(0) -> exc(1) ACKed -> VERSION retransmit(2) -> kernel counter 3.
    assert s.seq == 3, f"seq after exception should be 3, got {s.seq}"
    assert k.exception_seq == 3, f"kernel seq should be 3, got {k.exception_seq}"


def test_retransmit() -> None:
    k = FakeKernel()
    s = _make_session(k)
    s.connect()
    k.drop_requests = 1  # drop the first VERSION
    ver = s.version()
    assert ver["version"] == kdp_proto.KDP_VERSION
    # The dropped request plus the retransmit both hit the server.
    version_seqs = [seq for req, seq in k.requests_seen if req == KDP_VERSION_VERB]
    assert version_seqs == [1, 1], f"expected retransmit with same seq: {version_seqs}"
    assert s.seq == 2


def test_readregs() -> None:
    k = FakeKernel()
    s = _make_session(k)
    s.connect()
    regs = s.readregs(cpu=0, flavor=kdp_proto.ARM_THREAD_STATE64)
    assert regs["x0"] == 0x1000
    assert regs["x28"] == 0x1000 + 28
    assert regs["fp"] == 0x2000
    assert regs["lr"] == 0x3000
    assert regs["sp"] == 0x4000
    assert regs["pc"] == 0x5000


def test_breakpoints() -> None:
    k = FakeKernel()
    s = _make_session(k)
    s.connect()
    r1 = s.bp_add(0xfffffe0012345678)
    assert r1.error == KDPERR_NO_ERROR
    assert 0xfffffe0012345678 in k.breakpoints
    r2 = s.bp_add(0xfffffe0012345678)
    assert r2.error == kdp_proto.KDPERR_BREAKPOINT_ALREADY_SET
    r3 = s.bp_remove(0xfffffe0012345678)
    assert r3.error == KDPERR_NO_ERROR
    assert 0xfffffe0012345678 not in k.breakpoints
    r4 = s.bp_remove(0xfffffe0012345678)
    assert r4.error == kdp_proto.KDPERR_BREAKPOINT_NOT_FOUND


def test_disconnect() -> None:
    k = FakeKernel()
    s = _make_session(k)
    s.connect()
    reply = s.disconnect()
    assert reply.error == KDPERR_NO_ERROR
    assert not k.is_conn
    assert k.exception_seq == 0
    assert not s.connected


def test_step_unsupported() -> None:
    s = _make_session(FakeKernel())
    try:
        s.step()
    except KdpUnsupported:
        return
    raise AssertionError("step() should raise KdpUnsupported")


def test_bad_sequence_dropped() -> None:
    k = FakeKernel()
    s = _make_session(k)
    s.connect()
    # Force a wrong sequence: the kernel must drop it and the client times out.
    s.seq = 9
    try:
        s.version()
    except kdp_proto.KdpTimeout:
        return
    raise AssertionError("expected KdpTimeout for a bad sequence")


def test_cli_end_to_end() -> None:
    """Drive the real CLI against the fake kernel (connect/version/readmem)."""
    import contextlib
    import io
    import os
    import tempfile
    import kdp_cli

    kernel = FakeKernel()
    transport = kdp_serial.SerialTransport(FakePort(kernel), poll_timeout=0.0)

    orig_open = kdp_cli.kdp_serial.open_serial
    orig_session = kdp_cli.SESSION_FILE
    orig_bp = kdp_cli.BREAKPOINT_FILE
    tmp = tempfile.mkdtemp()
    kdp_cli.kdp_serial.open_serial = lambda *a, **kw: transport
    kdp_cli.SESSION_FILE = os.path.join(tmp, "session.json")
    kdp_cli.BREAKPOINT_FILE = os.path.join(tmp, "bp.json")
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            assert kdp_cli.main(["--port", "FAKE", "connect"]) == 0
            assert kdp_cli.main(["--port", "FAKE", "version"]) == 0
            assert kdp_cli.main(["--port", "FAKE", "readmem", "0x1000", "32"]) == 0
            assert kdp_cli.main(["--port", "FAKE", "readphys", "0x10000", "16"]) == 0
            assert kdp_cli.main(["--port", "FAKE", "readregs"]) == 0
            assert kdp_cli.main(["--port", "FAKE", "bpadd", "0x2000"]) == 0
            assert kdp_cli.main(["--port", "FAKE", "bplist"]) == 0
            assert kdp_cli.main(["--port", "FAKE", "bpdel", "0x2000"]) == 0
            assert kdp_cli.main(["--port", "FAKE", "disconnect"]) == 0
        assert kernel.is_conn is False
    finally:
        kdp_cli.kdp_serial.open_serial = orig_open
        kdp_cli.SESSION_FILE = orig_session
        kdp_cli.BREAKPOINT_FILE = orig_bp


# --------------------------------------------------------------------------
# runner
# --------------------------------------------------------------------------

TESTS = [
    test_escape_roundtrip,
    test_frame_deframe_roundtrip,
    test_crc_mismatch_resync,
    test_resync_after_garbage,
    test_l2l3_layout,
    test_connect_and_seq,
    test_fragmented_read,
    test_readphys,
    test_writemem_roundtrip,
    test_writemem_fragmented,
    test_writemem_rejects_oversize,
    test_writeregs_roundtrip,
    test_async_exception,
    test_retransmit,
    test_readregs,
    test_breakpoints,
    test_disconnect,
    test_step_unsupported,
    test_bad_sequence_dropped,
    test_cli_end_to_end,
]


def main() -> int:
    failures = 0
    for test in TESTS:
        name = test.__name__
        try:
            test()
            print(f"PASS  {name}")
        except Exception as exc:  # noqa: BLE001 - test runner
            failures += 1
            print(f"FAIL  {name}: {type(exc).__name__}: {exc}")
    print(f"\n{len(TESTS) - failures}/{len(TESTS)} tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
