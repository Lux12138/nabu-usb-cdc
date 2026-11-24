"""kdp_cdc_frame.py -- CDC wire framing for raw Ethernet frames over USB CDC.

This module defines the *device-side* byte format used by the nabu
"KDP over CDC" bridge.  It is intentionally tiny and dependency-free so that
the device-side agent can reimplement it in C verbatim.

Motivation
----------
XNU's KDP core (osfmk/kdp/kdp_udp.c) always hands the transport a *complete
Ethernet + IPv4 + UDP* frame, even when the transport is a serial console
(`kdp_send()` / `kdp_reply()` prepend 14 + 28 bytes unconditionally).  On a
USB CDC bulk pipe there are no frame boundaries, so we wrap each Ethernet
frame in a small header with a CRC:

    offset  size  field
    ------  ----  -------------------------------------------------------
    0       4     magic   = b"KDPF"  (bytes 4B 44 50 46)
    4       1     version = 0x01
    5       1     flags   = 0x00  (reserved, must be 0)
    6       2     length  = payload length, little-endian uint16
    8       4     crc32   = zlib CRC-32 over the payload, little-endian
    12      N     payload = raw Ethernet frame (14 + 20 + 8 + KDP)

Design notes
------------
* No escaping.  The KDP serial path needs 0xFA/0xFB/0xFE/'\\n' escaping; this
  format does not, which keeps the device-side state machine trivial and makes
  the worst-case overhead a fixed 12 bytes.
* Resynchronisation: scan for the 4-byte magic, then require
  ``version == 1`` and ``length <= MAX_PAYLOAD``.  A CRC mismatch drops the
  whole frame and the scanner restarts at the next magic candidate.  A false
  positive is bounded by 1/2^32 per candidate.
* ``MAX_PAYLOAD`` mirrors XNU's ``KDP_MAXPACKET`` (1518 = 14 + 1500 + 4,
  osfmk/kdp/kdp_udp.c:219-222).  Real KDP packets never exceed 1200 bytes
  (MAX_KDP_PKT_SIZE), i.e. 1242 bytes as an Ethernet frame.
"""

from __future__ import annotations

import struct
import time
import zlib

MAGIC = b"KDPF"
MAGIC_UINT32 = 0x4650444B          # little-endian value of b"KDPF"
VERSION = 1
FLAGS_RESERVED = 0x00

HEADER_LEN = 12                    # magic(4) + version(1) + flags(1) + len(2) + crc(4)
MAX_PAYLOAD = 1518                 # KDP_MAXPACKET (kdp_udp.c:219-222)
MIN_FRAME_LEN = HEADER_LEN + 1     # a frame always carries at least one byte

# Receiver states.
RS_SCAN = 0                        # looking for MAGIC
RS_HEADER = 1                      # magic found, collecting the rest of the header
RS_PAYLOAD = 2                     # header validated, collecting payload


class FrameError(ValueError):
    """Raised for programmer errors (e.g. oversize payload)."""


def compute_crc(payload: bytes) -> int:
    """Standard zlib CRC-32 (identical to XNU's z_crc32(0, data, len))."""
    return zlib.crc32(payload) & 0xFFFFFFFF


def frame_packet(payload: bytes, *, flags: int = FLAGS_RESERVED) -> bytes:
    """Wrap one raw Ethernet frame in the CDC framing header.

    Raises :class:`FrameError` if the payload is empty or too large.
    """
    if not payload:
        raise FrameError("refusing to frame an empty payload")
    if len(payload) > MAX_PAYLOAD:
        raise FrameError(
            f"payload {len(payload)} B exceeds MAX_PAYLOAD {MAX_PAYLOAD} B; "
            "fragment the KDP packet before sending it over CDC"
        )
    crc = compute_crc(payload)
    header = struct.pack(
        "<4sBBHI", MAGIC, VERSION, flags & 0xFF, len(payload) & 0xFFFF, crc
    )
    return header + payload


def parse_header(buf: bytes) -> tuple[int, int, int] | None:
    """Validate an 12-byte header.

    Returns ``(flags, length, crc)`` or ``None`` if the header is not
    acceptable (bad magic/version/flags or implausible length).
    """
    if len(buf) < HEADER_LEN:
        return None
    magic, version, flags, length, crc = struct.unpack_from("<4sBBHI", buf, 0)
    if magic != MAGIC:
        return None
    if version != VERSION:
        return None
    if flags != FLAGS_RESERVED:
        return None
    if length == 0 or length > MAX_PAYLOAD:
        return None
    return flags, length, crc


class FrameStats:
    """Counters shared between the framer and the deframer."""

    __slots__ = (
        "frames_tx", "frames_rx", "bytes_tx", "bytes_rx",
        "crc_errors", "bad_headers", "oversize_drops", "resyncs",
    )

    def __init__(self) -> None:
        self.frames_tx = 0
        self.frames_rx = 0
        self.bytes_tx = 0
        self.bytes_rx = 0
        self.crc_errors = 0
        self.bad_headers = 0
        self.oversize_drops = 0
        self.resyncs = 0

    def as_dict(self) -> dict:
        return {name: getattr(self, name) for name in self.__slots__}

    def __str__(self) -> str:
        d = self.as_dict()
        return (
            f"tx_frames={d['frames_tx']} rx_frames={d['frames_rx']} "
            f"tx_bytes={d['bytes_tx']} rx_bytes={d['bytes_rx']} "
            f"crc_errors={d['crc_errors']} bad_headers={d['bad_headers']} "
            f"oversize={d['oversize_drops']} resyncs={d['resyncs']}"
        )


class CdcDeframer:
    """Streaming deframer for the KDPF format.

    Feed bytes with :meth:`feed` (one byte) or :meth:`feed_bytes`; every
    complete, CRC-valid payload is returned.  Invalid frames are dropped and
    counted in :attr:`stats`.
    """

    def __init__(self, stats: FrameStats | None = None,
                 max_payload: int = MAX_PAYLOAD) -> None:
        self.stats = stats if stats is not None else FrameStats()
        self.max_payload = max_payload
        self.state = RS_SCAN
        self.buf = bytearray()       # scan tail (<=3 B) or payload accumulator
        self.hdr = bytearray()       # magic + header bytes while in RS_HEADER
        self.want = 0
        self.length = 0
        self.crc = 0
        self.magic_pos = -1
        self.last_byte_at = time.monotonic()

    def reset(self) -> None:
        self.state = RS_SCAN
        self.buf = bytearray()
        self.hdr = bytearray()
        self.want = 0
        self.length = 0
        self.crc = 0
        self.magic_pos = -1

    def maybe_resync(self, idle_seconds: float) -> bool:
        """Reset a half-received frame after ``idle_seconds`` without bytes.

        A truncated frame (e.g. the device dropped a chunk) would otherwise
        swallow the beginning of the next frame, because its declared length
        keeps pulling bytes in.  Returns True if a reset happened.
        """
        if idle_seconds <= 0 or self.state == RS_SCAN:
            return False
        if time.monotonic() - self.last_byte_at >= idle_seconds:
            self.reset()
            self.stats.resyncs += 1
            return True
        return False

    # -- public API --------------------------------------------------------
    def feed(self, byte: int) -> bytes | None:
        """Consume one byte; return a finished payload or ``None``."""
        out = self.feed_bytes(bytes((byte & 0xFF,)))
        return out[0] if out else None

    def feed_bytes(self, data: bytes) -> list[bytes]:
        """Consume a chunk; return every payload completed by this chunk.

        Implemented as an index-based scan so that resynchronisation after a
        bogus magic can resume at *any* byte position, including positions that
        overlap the rejected magic.  Bytes that may be the prefix of a magic are
        carried over to the next call.
        """
        self.last_byte_at = time.monotonic()
        pending = bytes(self.buf) + bytes(data)
        self.buf = bytearray()
        data = pending
        n = len(data)
        i = 0
        out: list[bytes] = []

        while i < n:
            if self.state == RS_SCAN:
                idx = data.find(MAGIC, i)
                if idx < 0:
                    keep = min(len(MAGIC) - 1, n - i)
                    self.buf = bytearray(data[n - keep:])
                    self.stats.resyncs += (n - i) - keep
                    i = n
                    continue
                self.stats.resyncs += idx - i
                self.magic_pos = idx
                self.hdr = bytearray(MAGIC)
                self.state = RS_HEADER
                i = idx + len(MAGIC)
                continue

            if self.state == RS_HEADER:
                need = HEADER_LEN - len(self.hdr)
                take = min(need, n - i)
                self.hdr.extend(data[i:i + take])
                i += take
                if len(self.hdr) < HEADER_LEN:
                    break
                parsed = parse_header(bytes(self.hdr))
                if parsed is None or parsed[1] > self.max_payload:
                    if parsed is None:
                        self.stats.bad_headers += 1
                    else:
                        self.stats.oversize_drops += 1
                    # Restart the scan just after the first byte of the bogus
                    # magic, so an overlapping real magic is still found.
                    self.state = RS_SCAN
                    self.hdr = bytearray()
                    i = self.magic_pos + 1
                    continue
                _, self.length, self.crc = parsed
                self.buf = bytearray()
                self.want = self.length
                self.state = RS_PAYLOAD
                continue

            # RS_PAYLOAD
            need = self.want - len(self.buf)
            take = min(need, n - i)
            self.buf.extend(data[i:i + take])
            i += take
            if len(self.buf) < self.want:
                break
            payload = bytes(self.buf)
            self.buf = bytearray()
            self.hdr = bytearray()
            self.state = RS_SCAN
            if compute_crc(payload) != self.crc:
                self.stats.crc_errors += 1
                continue
            self.stats.frames_rx += 1
            self.stats.bytes_rx += len(payload)
            out.append(payload)

        return out


def looks_like_frame(data: bytes) -> bool:
    """Return True if `data` starts with a plausible KDPF header."""
    return parse_header(data) is not None


def hexdump_frame(frame: bytes, width: int = 16) -> str:
    """Small helper used by the self-test to print frames readably."""
    lines = []
    for off in range(0, len(frame), width):
        chunk = frame[off:off + width]
        hexpart = " ".join(f"{b:02x}" for b in chunk)
        lines.append(f"{off:04x}  {hexpart:<{width * 3}}")
    return "\n".join(lines)


if __name__ == "__main__":  # pragma: no cover - manual smoke test
    import os

    for _ in range(200):
        payload = os.urandom(1 + (os.getpid() * 7) % 200)
        wire = frame_packet(payload)
        d = CdcDeframer()
        got = d.feed_bytes(wire)
        assert got == [payload], (len(payload), got)
    print("kdp_cdc_frame: 200 random round-trips OK")
