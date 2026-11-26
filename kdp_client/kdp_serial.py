"""kdp_serial.py -- XNU KDP-over-serial framing / transport layer.

This module implements the *serial* half of Apple's KDP as it exists in
XNU 17.7.11 (osfmk/kdp/kdp_serial.c).  It is deliberately independent of the
Ethernet/IP/UDP + KDP protocol layers (see kdp_proto.py).

Wire format (kdp_serialize_packet(), kdp_serial.c):

    frame = 0xFA | escape(CRC32_LE(4 bytes)) | 0xFA | escape(packet) | 0xFB

    * CRC32 is the standard zlib CRC-32 (z_crc32(0, packet, len)) and is
      written LITTLE-ENDIAN on the wire (crc>>0 first).  Note that the four
      CRC bytes go through the same escaping as the payload.
    * Escaping: a byte in {0xFA, 0xFB, 0xFE, '\\n'} is emitted as
      0xFE followed by (~byte & 0xFF).  '\\n' is escaped because the kernel
      serial console would otherwise turn it into CRLF.
    * The second 0xFA is a delimiter that also marks "CRC present".

Receiver state machine (kdp_unserialize_packet()):

    DS_WAITSTART --0xFA--> DS_READING (pos=0, have_crc=False)
    DS_READING  --0xFE--> DS_ESCAPED
    DS_READING  --0xFA--> if pos>=4: have_crc=True, crc=LE32(buf[0:4]); pos=0
    DS_READING  --0xFB--> finish; if have_crc and crc mismatch: drop + resync
    DS_ESCAPED  --b-->    buf[pos++] = ~b; back to DS_READING
    overflow (pos == 1518): drop + resync

IMPORTANT: this module only deals with the serial framing.  The `packet`
handed to/from the transport is the *full* Ethernet+IP+UDP+KDP frame body,
i.e. the thing kdp_send()/kdp_reply() hand to kdp_serialize_packet().
"""

from __future__ import annotations

import struct
import time
import zlib

SKDP_START_CHAR = 0xFA
SKDP_END_CHAR = 0xFB
SKDP_ESC_CHAR = 0xFE

# Bytes that must be escaped.  '\n' is included on purpose (see module doc).
ESCAPE_SET = (SKDP_START_CHAR, SKDP_END_CHAR, SKDP_ESC_CHAR, 0x0A)

# kdp_serial.c: static unsigned char dsBuffer[1518];
MAX_FRAME_BUFFER = 1518

# Serial receive state machine states.
DS_WAITSTART = 0
DS_READING = 1
DS_ESCAPED = 2


def escape(data: bytes) -> bytes:
    """Escape `data` using the KDP serial escaping rules.

    Every byte in ESCAPE_SET is replaced by 0xFE followed by its bitwise
    complement.  All other bytes are copied verbatim.
    """
    out = bytearray()
    for b in data:
        if b in ESCAPE_SET:
            out.append(SKDP_ESC_CHAR)
            out.append((~b) & 0xFF)
        else:
            out.append(b)
    return bytes(out)


def compute_crc(data: bytes) -> int:
    """Standard zlib CRC-32, matching z_crc32(0, data, len)."""
    return zlib.crc32(data) & 0xFFFFFFFF


def frame_packet(packet: bytes) -> bytes:
    """Wrap `packet` in the KDP serial frame.

    The CRC is computed over the *unescaped* packet, then both the CRC field
    and the packet are escaped.  (kdp_serial.c runs the CRC bytes through
    kdp_serial_out() exactly like the payload bytes.)
    """
    crc = compute_crc(packet)
    crc_bytes = struct.pack("<I", crc)  # little-endian on the wire
    return (
        bytes((SKDP_START_CHAR,))
        + escape(crc_bytes)
        + bytes((SKDP_START_CHAR,))
        + escape(packet)
        + bytes((SKDP_END_CHAR,))
    )


class SerialStats:
    """Counters for the serial receiver (optional, purely observational)."""

    __slots__ = ("frames_tx", "frames_rx", "bytes_tx", "bytes_rx",
                 "crc_errors", "overflow_drops")

    def __init__(self) -> None:
        self.frames_tx = 0
        self.frames_rx = 0
        self.bytes_tx = 0
        self.bytes_rx = 0
        self.crc_errors = 0
        self.overflow_drops = 0

    def as_dict(self) -> dict:
        return {name: getattr(self, name) for name in self.__slots__}


class SerialDeframer:
    """Streaming deframer matching kdp_unserialize_packet().

    Feed bytes with :meth:`feed`; a complete, CRC-valid packet is returned
    as a ``bytes`` object, otherwise ``None`` is returned.
    """

    def __init__(self, max_buffer: int = MAX_FRAME_BUFFER,
                 stats: "SerialStats | None" = None) -> None:
        self.max_buffer = max_buffer
        self.stats = stats if stats is not None else SerialStats()
        self.reset()

    def reset(self) -> None:
        """Drop any partial frame and return to DS_WAITSTART (resync)."""
        self.state = DS_WAITSTART
        self.buf = bytearray()
        self.crc = 0
        self.have_crc = False

    def feed(self, byte: int) -> bytes | None:
        """Consume one byte; return a finished packet or None."""
        byte &= 0xFF

        if self.state == DS_WAITSTART:
            if byte == SKDP_START_CHAR:
                self.state = DS_READING
                self.buf = bytearray()
                self.have_crc = False
            return None

        if self.state == DS_READING:
            if byte == SKDP_ESC_CHAR:
                self.state = DS_ESCAPED
                return None
            if byte == SKDP_START_CHAR:
                # Second start char: the first 4 bytes were the CRC.
                if len(self.buf) >= 4:
                    self.have_crc = True
                    self.crc = (
                        self.buf[0]
                        | (self.buf[1] << 8)
                        | (self.buf[2] << 16)
                        | (self.buf[3] << 24)
                    )
                self.buf = bytearray()
                return None
            if byte == SKDP_END_CHAR:
                self.state = DS_WAITSTART
                if self.have_crc:
                    if compute_crc(bytes(self.buf)) != self.crc:
                        # Bad CRC: drop the whole frame and resync.
                        self.buf = bytearray()
                        self.have_crc = False
                        self.stats.crc_errors += 1
                        return None
                out = bytes(self.buf)
                self.buf = bytearray()
                self.have_crc = False
                self.stats.frames_rx += 1
                self.stats.bytes_rx += len(out)
                return out
            self.buf.append(byte)
            if len(self.buf) >= self.max_buffer:
                # Too much data: forget this packet and resync.
                self.stats.overflow_drops += 1
                self.reset()
            return None

        # DS_ESCAPED
        self.buf.append((~byte) & 0xFF)
        self.state = DS_READING
        if len(self.buf) >= self.max_buffer:
            self.stats.overflow_drops += 1
            self.reset()
        return None

    def feed_bytes(self, data: bytes) -> list[bytes]:
        """Consume a chunk; return every packet completed by this chunk."""
        out = []
        for byte in data:
            packet = self.feed(byte)
            if packet is not None:
                out.append(packet)
        return out


class SerialTransport:
    """Byte transport that frames outgoing / deframes incoming packets.

    The underlying `port` only needs the pyserial surface used here:
    ``write(bytes)``, ``read(size) -> bytes``, ``reset_input_buffer()`` and
    ``close()``.  A dummy implementation is provided in selftest.py, so this
    class is unit-testable without hardware.
    """

    def __init__(self, port, poll_timeout: float = 0.05, verbose: bool = False) -> None:
        self.port = port
        self.poll_timeout = poll_timeout
        self.verbose = verbose
        self.deframer = SerialDeframer()
        self.frames_tx = 0
        self.frames_rx = 0
        self.bytes_tx = 0
        self.bytes_rx = 0

    # -- sending ---------------------------------------------------------
    def send_packet(self, packet: bytes) -> None:
        """Frame and transmit one packet."""
        frame = frame_packet(packet)
        self.port.write(frame)
        self.frames_tx += 1
        self.bytes_tx += len(frame)
        if self.verbose:
            print(f"[serial] TX {len(packet)}B payload, {len(frame)}B frame")

    # -- receiving -------------------------------------------------------
    def receive_packet(self, timeout: float = 1.0) -> bytes | None:
        """Read bytes until one complete frame is reassembled.

        Returns the deframed packet, or None if `timeout` seconds elapse
        without a valid frame.  Invalid-CRC frames are dropped and the
        deframer resynchronises on the next 0xFA.
        """
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            # pyserial returns as soon as at least one byte is available, or
            # b'' after `poll_timeout`.  This is a bounded poll, not a sleep.
            data = self.port.read(4096)
            if data:
                self.bytes_rx += len(data)
                for b in data:
                    pkt = self.deframer.feed(b)
                    if pkt is not None:
                        self.frames_rx += 1
                        if self.verbose:
                            print(f"[serial] RX {len(pkt)}B payload")
                        return pkt
            # No data: loop again (port.read already applied its timeout).

    def resync(self) -> None:
        """Flush the OS input buffer and reset the deframer."""
        try:
            self.port.reset_input_buffer()
        except Exception:
            pass
        self.deframer.reset()

    def close(self) -> None:
        try:
            self.port.close()
        except Exception:
            pass


def open_serial(url: str, baudrate: int = 115200, poll_timeout: float = 0.05,
                verbose: bool = False) -> SerialTransport:
    """Open a serial port with pyserial and wrap it in a SerialTransport.

    `url` is a pyserial URL: "COM21" on Windows, "/dev/ttyACM0" on Linux/WSL,
    or "socket://host:port" for a TCP bridge.
    """
    try:
        import serial  # type: ignore
    except ImportError as exc:  # pragma: no cover - environment dependent
        raise RuntimeError(
            "pyserial is required for real hardware. "
            "Install it with: python -m pip install pyserial"
        ) from exc

    port = serial.serial_for_url(url, baudrate=baudrate, timeout=poll_timeout)
    # T143: usbser.sys forwards nothing until DTR is asserted, so a KDP port that
    # is opened without it looks dead.  setRtsDtr is best-effort: socket://
    # bridges have no modem lines and pyserial raises there.
    try:
        port.dtr = True
        port.rts = True
    except Exception:
        pass
    return SerialTransport(port, poll_timeout=poll_timeout, verbose=verbose)
