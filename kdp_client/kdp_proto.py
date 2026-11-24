"""kdp_proto.py -- KDP protocol layer (Ethernet + IP/UDP + kdp_hdr_t).

This module implements the *protocol* half of Apple's KDP as it exists in
XNU 17.7.11.  The serial framing lives in kdp_serial.py; this file builds and
parses the L2/L3 wrapper and the KDP packets themselves.

Why the L2/L3 wrapper?
----------------------
Even on a serial console, XNU's KDP is not a bare KDP protocol.  kdp_init()
(config_serial_kdp) registers kdp_serial_send/kdp_serial_receive, but
kdp_send()/kdp_reply() unconditionally prepend a 14-byte Ethernet header and
a 28-byte IP/UDP header, and kdp_poll() validates them:

    * ether_type == ETHERTYPE_IP (0x0800)
    * ip.proto  == IPPROTO_UDP (17)
    * ip.ip_hl   <= 5
    * udp.dport  == KDP_REMOTE_PORT (41139)

Therefore the KDP payload sits at offset 14 + 20 + 8 = 42 bytes inside the
serial frame body.

Serial KDP fakes its addresses (kdp_udp.c kdp_init):

    mac  = "serial"          (73 65 72 69 61 6c)
    ip   = 0xABADBABE        (KDP_SERIAL_IPADDR)

The kernel adopts the destination MAC/IP of the first received packet as its
own address and the source as the remote address, so the exact values are not
checked; the defaults below follow the reference client sketch in T29.

KDP header (kdp_protocol.h kdp_hdr_t, 8 bytes, little-endian bitfields):

    byte 0 : request (bits 0..6) | is_reply (bit 7)
    byte 1 : seq
    bytes 2..3 : len   (total packet length, header included)
    bytes 4..7 : key   (session key)

Sequence rules (kdp_udp.c kdp_connection_wait / kdp_handler):

    * the first CONNECT must carry seq == 0
    * every subsequent request must carry seq == the kernel's exception_seq
    * the kernel increments exception_seq after each reply
    * re-sending the previous seq makes the kernel re-send the saved reply
"""

from __future__ import annotations

import socket
import struct
import time
from dataclasses import dataclass, field

# --------------------------------------------------------------------------
# Protocol constants (osfmk/kdp/kdp_protocol.h)
# --------------------------------------------------------------------------

KDP_VERSION = 12
KDP_REMOTE_PORT = 41139
UDP_HOST_COMM_BASE = 41140
UDP_HOST_EXCEP_BASE = 41145

MAX_KDP_DATA_SIZE = 1024
MAX_KDP_PKT_SIZE = 1200
KDP_REXMIT_SECS = 3
KDP_REXMIT_TRIES = 8

KDP_FEATURE_BP = 0x1

# kdp_req_t
KDP_CONNECT = 0
KDP_DISCONNECT = 1
KDP_HOSTINFO = 2
KDP_VERSION_VERB = 3
KDP_MAXBYTES = 4
KDP_READMEM = 5
KDP_WRITEMEM = 6
KDP_READREGS = 7
KDP_WRITEREGS = 8
KDP_LOAD = 9
KDP_IMAGEPATH = 10
KDP_SUSPEND = 11
KDP_RESUMECPUS = 12
KDP_EXCEPTION = 13
KDP_TERMINATION = 14
KDP_BREAKPOINT_SET = 15
KDP_BREAKPOINT_REMOVE = 16
KDP_REGIONS = 17
KDP_REATTACH = 18
KDP_HOSTREBOOT = 19
KDP_READMEM64 = 20
KDP_WRITEMEM64 = 21
KDP_BREAKPOINT64_SET = 22
KDP_BREAKPOINT64_REMOVE = 23
KDP_KERNELVERSION = 24
KDP_READPHYSMEM64 = 25
KDP_WRITEPHYSMEM64 = 26
KDP_READIOPORT = 27
KDP_WRITEIOPORT = 28
KDP_READMSR64 = 29
KDP_WRITEMSR64 = 30
KDP_DUMPINFO = 31
KDP_INVALID_REQUEST = 32

REQUEST_NAMES = {
    KDP_CONNECT: "CONNECT",
    KDP_DISCONNECT: "DISCONNECT",
    KDP_HOSTINFO: "HOSTINFO",
    KDP_VERSION_VERB: "VERSION",
    KDP_MAXBYTES: "MAXBYTES",
    KDP_READMEM: "READMEM",
    KDP_WRITEMEM: "WRITEMEM",
    KDP_READREGS: "READREGS",
    KDP_WRITEREGS: "WRITEREGS",
    KDP_LOAD: "LOAD",
    KDP_IMAGEPATH: "IMAGEPATH",
    KDP_SUSPEND: "SUSPEND",
    KDP_RESUMECPUS: "RESUMECPUS",
    KDP_EXCEPTION: "EXCEPTION",
    KDP_TERMINATION: "TERMINATION",
    KDP_BREAKPOINT_SET: "BREAKPOINT_SET",
    KDP_BREAKPOINT_REMOVE: "BREAKPOINT_REMOVE",
    KDP_REGIONS: "REGIONS",
    KDP_REATTACH: "REATTACH",
    KDP_HOSTREBOOT: "HOSTREBOOT",
    KDP_READMEM64: "READMEM64",
    KDP_WRITEMEM64: "WRITEMEM64",
    KDP_BREAKPOINT64_SET: "BREAKPOINT64_SET",
    KDP_BREAKPOINT64_REMOVE: "BREAKPOINT64_REMOVE",
    KDP_KERNELVERSION: "KERNELVERSION",
    KDP_READPHYSMEM64: "READPHYSMEM64",
    KDP_WRITEPHYSMEM64: "WRITEPHYSMEM64",
    KDP_READIOPORT: "READIOPORT",
    KDP_WRITEIOPORT: "WRITEIOPORT",
    KDP_READMSR64: "READMSR64",
    KDP_WRITEMSR64: "WRITEMSR64",
    KDP_DUMPINFO: "DUMPINFO",
}

# kdp_error_t
KDPERR_NO_ERROR = 0
KDPERR_ALREADY_CONNECTED = 1
KDPERR_BAD_NBYTES = 2
KDPERR_BADFLAVOR = 3
KDPERR_BAD_ACCESS = 4
KDPERR_MAX_BREAKPOINTS = 100
KDPERR_BREAKPOINT_NOT_FOUND = 101
KDPERR_BREAKPOINT_ALREADY_SET = 102

ERROR_NAMES = {
    KDPERR_NO_ERROR: "NO_ERROR",
    KDPERR_ALREADY_CONNECTED: "ALREADY_CONNECTED",
    KDPERR_BAD_NBYTES: "BAD_NBYTES",
    KDPERR_BADFLAVOR: "BADFLAVOR",
    KDPERR_BAD_ACCESS: "BAD_ACCESS",
    KDPERR_MAX_BREAKPOINTS: "MAX_BREAKPOINTS",
    KDPERR_BREAKPOINT_NOT_FOUND: "BREAKPOINT_NOT_FOUND",
    KDPERR_BREAKPOINT_ALREADY_SET: "BREAKPOINT_ALREADY_SET",
}

# ARM thread-state flavors (osfmk/mach/arm/thread_status.h)
ARM_THREAD_STATE = 1
ARM_VFP_STATE = 2
ARM_THREAD_STATE64 = 6
ARM_EXCEPTION_STATE64 = 7

# arm_thread_state64_t: x[29], fp, lr, sp, pc, cpsr, flags
ARM_THREAD_STATE64_SIZE = 272
ARM_THREAD_STATE64_COUNT = ARM_THREAD_STATE64_SIZE // 4  # 68

# L2/L3 constants
ETHERTYPE_IP = 0x0800
IPPROTO_UDP = 17
IPVERSION = 4
SERIAL_MAC = b"serial"
SERIAL_IP = 0xABADBABE

KDP_HDR_LEN = 8
ETH_HDR_LEN = 14
IP_HDR_LEN = 20
UDP_HDR_LEN = 8
KDP_PAYLOAD_OFFSET = ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN  # 42


# --------------------------------------------------------------------------
# Exceptions
# --------------------------------------------------------------------------

class KdpError(Exception):
    """Base class for KDP protocol errors."""


class KdpTimeout(KdpError):
    """No valid reply within the allocated time/retries."""


class KdpProtocolError(KdpError):
    """A reply was received but it is not usable."""


class KdpUnsupported(KdpError):
    """The requested operation is not supported by this KDP implementation."""


# --------------------------------------------------------------------------
# kdp_hdr_t
# --------------------------------------------------------------------------

@dataclass
class KdpPacket:
    request: int
    is_reply: bool
    seq: int
    length: int
    key: int
    body: bytes = b""

    @property
    def name(self) -> str:
        return REQUEST_NAMES.get(self.request, f"<{self.request}>")


def pack_hdr(request: int, is_reply: bool, seq: int, length: int, key: int) -> bytes:
    """Pack an 8-byte kdp_hdr_t (little-endian bitfield layout)."""
    byte0 = (request & 0x7F) | ((0x80 if is_reply else 0) & 0x80)
    return struct.pack("<BBHI", byte0, seq & 0xFF, length & 0xFFFF, key & 0xFFFFFFFF)


def unpack_hdr(data: bytes) -> KdpPacket:
    """Unpack an 8-byte kdp_hdr_t plus whatever body bytes follow it."""
    if len(data) < KDP_HDR_LEN:
        raise KdpProtocolError(f"short KDP header: {len(data)} bytes")
    byte0, seq, length, key = struct.unpack_from("<BBHI", data, 0)
    return KdpPacket(
        request=byte0 & 0x7F,
        is_reply=bool(byte0 & 0x80),
        seq=seq,
        length=length,
        key=key,
        body=bytes(data[KDP_HDR_LEN:]),
    )


# --------------------------------------------------------------------------
# Ethernet + IP + UDP wrapper
# --------------------------------------------------------------------------

def _ip_checksum(header: bytes) -> int:
    """RFC 1071 one's-complement checksum over the IP header."""
    if len(header) % 2:
        header = header + b"\x00"
    total = 0
    for i in range(0, len(header), 2):
        total += (header[i] << 8) | header[i + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def _ip_to_bytes(value: int) -> bytes:
    """Pack a 32-bit IPv4 address in network byte order."""
    return struct.pack(">I", value & 0xFFFFFFFF)


def build_l2l3(kdp_packet: bytes, *,
              eth_src: bytes = SERIAL_MAC,
              eth_dst: bytes = b"\xff\xff\xff\xff\xff\xff",
              ip_src: int = 0,
              ip_dst: int = SERIAL_IP,
              udp_sport: int = UDP_HOST_COMM_BASE,
              udp_dport: int = KDP_REMOTE_PORT,
              ip_id: int = 0) -> bytes:
    """Wrap a KDP packet in Ethernet + IPv4 + UDP the way kdp_send() does."""
    kdp_len = len(kdp_packet)
    if kdp_len > MAX_KDP_PKT_SIZE:
        raise KdpProtocolError(f"KDP packet too large: {kdp_len} > {MAX_KDP_PKT_SIZE}")

    eth = eth_dst + eth_src + struct.pack(">H", ETHERTYPE_IP)

    udp_len = UDP_HDR_LEN + kdp_len
    udp = struct.pack(">HHHH", udp_sport & 0xFFFF, udp_dport & 0xFFFF,
                      udp_len, 0)  # UDP checksum 0 is legal for IPv4

    total_len = IP_HDR_LEN + udp_len
    ip = struct.pack(
        ">BBHHHBBH4s4s",
        (IPVERSION << 4) | 5,   # version + ihl
        0,                      # tos
        total_len,
        ip_id & 0xFFFF,
        0,                      # flags / fragment offset
        60,                     # ttl
        IPPROTO_UDP,
        0,                      # checksum placeholder
        _ip_to_bytes(ip_src),
        _ip_to_bytes(ip_dst),
    )
    csum = _ip_checksum(ip)
    ip = ip[:10] + struct.pack(">H", csum) + ip[12:]

    return eth + ip + udp + kdp_packet


def parse_l2l3(frame: bytes, reply_port: int = UDP_HOST_COMM_BASE) -> bytes | None:
    """Validate an Ethernet+IP+UDP frame and return the KDP payload.

    Returns None if the frame is not a KDP UDP packet (mirrors the checks in
    kdp_poll(): ether_type, ip.proto, ip_hl, udp.dport).
    """
    if len(frame) < KDP_PAYLOAD_OFFSET:
        return None

    ether_type = struct.unpack_from(">H", frame, 12)[0]
    if ether_type != ETHERTYPE_IP:
        return None

    ip_start = ETH_HDR_LEN
    byte0 = frame[ip_start]
    version = byte0 >> 4
    ip_hl = byte0 & 0x0F
    if version != IPVERSION or ip_hl != 5:
        return None
    if frame[ip_start + 9] != IPPROTO_UDP:
        return None

    udp_start = ip_start + IP_HDR_LEN
    sport, dport, ulen, _ucsum = struct.unpack_from(">HHHH", frame, udp_start)
    # kdp_poll() requires dport == KDP_REMOTE_PORT on packets sent *to* the
    # kernel; the kernel's replies come *from* sport == KDP_REMOTE_PORT.
    # Accept either direction (the client's reply_port is not enforced).
    if dport != KDP_REMOTE_PORT and sport != KDP_REMOTE_PORT:
        return None

    kdp_len = ulen - UDP_HDR_LEN
    if kdp_len < KDP_HDR_LEN:
        return None
    end = udp_start + UDP_HDR_LEN + kdp_len
    if end > len(frame):
        return None
    return frame[udp_start + UDP_HDR_LEN:end]


# --------------------------------------------------------------------------
# Session
# --------------------------------------------------------------------------

@dataclass
class KdpReply:
    packet: KdpPacket
    error: int = KDPERR_NO_ERROR

    @property
    def error_name(self) -> str:
        return ERROR_NAMES.get(self.error, f"<{self.error}>")

    @property
    def data(self) -> bytes:
        """Reply body with the 4-byte error field removed (if present)."""
        return self.packet.body[4:] if len(self.packet.body) >= 4 else b""


class KdpSession:
    """A KDP session over a SerialTransport.

    The session owns the sequence number and the session key.  It implements
    timeout/retransmit, asynchronous exception ACKing and reply validation.
    """

    def __init__(self, transport, *,
                 reply_port: int = UDP_HOST_COMM_BASE,
                 exception_port: int = UDP_HOST_EXCEP_BASE,
                 key: int = 0x1337C0DE,
                 eth_src: bytes = SERIAL_MAC,
                 eth_dst: bytes = b"\xff\xff\xff\xff\xff\xff",
                 ip_src: int = 0,
                 ip_dst: int = SERIAL_IP,
                 udp_sport: int = UDP_HOST_COMM_BASE,
                 timeout: float = KDP_REXMIT_SECS,
                 retries: int = KDP_REXMIT_TRIES,
                 connect_timeout: float = 10.0,
                 verbose: bool = False) -> None:
        self.transport = transport
        self.reply_port = reply_port
        self.exception_port = exception_port
        self.key = key & 0xFFFFFFFF
        self.eth_src = eth_src
        self.eth_dst = eth_dst
        self.ip_src = ip_src
        self.ip_dst = ip_dst
        self.udp_sport = udp_sport
        self.timeout = timeout
        self.retries = retries
        self.connect_timeout = connect_timeout
        self.verbose = verbose

        self.seq = 0
        self.connected = False
        self.remote_version: int | None = None
        self.remote_feature: int | None = None
        self._ip_id = 0

    # -- low level -------------------------------------------------------
    def _next_ip_id(self) -> int:
        self._ip_id = (self._ip_id + 1) & 0xFFFF
        return self._ip_id

    def _send_kdp(self, request: int, body: bytes, seq: int, *,
                  is_reply: bool = False, key: int | None = None) -> None:
        kdp = pack_hdr(request, is_reply, seq, KDP_HDR_LEN + len(body),
                       self.key if key is None else key)
        kdp += body
        # kdp_poll() requires udp.dport == KDP_REMOTE_PORT on requests, and
        # kdp_send()/kdp_reply() put KDP_REMOTE_PORT in udp.sport.  So the
        # client's requests use sport = reply_port, dport = KDP_REMOTE_PORT.
        frame = build_l2l3(kdp, eth_src=self.eth_src, eth_dst=self.eth_dst,
                           ip_src=self.ip_src, ip_dst=self.ip_dst,
                           udp_sport=self.reply_port, udp_dport=KDP_REMOTE_PORT,
                           ip_id=self._next_ip_id())
        self.transport.send_packet(frame)

    def _recv_kdp(self, timeout: float) -> KdpPacket | None:
        """Read frames until a valid KDP packet arrives or `timeout` expires."""
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            raw = self.transport.receive_packet(remaining)
            if raw is None:
                return None
            payload = parse_l2l3(raw, reply_port=self.reply_port)
            if payload is None:
                continue
            pkt = unpack_hdr(payload)
            if pkt.length != len(payload):
                if self.verbose:
                    print(f"[kdp] length mismatch: hdr={pkt.length} wire={len(payload)}")
                continue
            return pkt

    def _handle_async(self, pkt: KdpPacket) -> None:
        """Handle a non-reply packet (exception / termination notification)."""
        if pkt.request == KDP_EXCEPTION:
            n = struct.unpack_from("<I", pkt.body, 0)[0] if len(pkt.body) >= 4 else 0
            infos = []
            off = 4
            for _ in range(min(n, 8)):
                if off + 12 > len(pkt.body):
                    break
                cpu, exc, code, sub = struct.unpack_from("<IIII", pkt.body, off)
                infos.append((cpu, exc, code, sub))
                off += 12
            print(f"[kdp] asynchronous EXCEPTION seq={pkt.seq} "
                  f"n={n} info={infos}")
            # Acknowledge: request=EXCEPTION, is_reply=1, same seq.
            self._send_kdp(KDP_EXCEPTION, b"", pkt.seq, is_reply=True, key=0)
            self.seq = (pkt.seq + 1) & 0xFF
        elif pkt.request == KDP_TERMINATION:
            term_code, exit_code = (struct.unpack_from("<II", pkt.body, 0)
                                    if len(pkt.body) >= 8 else (0, 0))
            print(f"[kdp] TERMINATION seq={pkt.seq} code={term_code} exit={exit_code}")
            self._send_kdp(KDP_TERMINATION, b"", pkt.seq, is_reply=True, key=0)
            self.seq = (pkt.seq + 1) & 0xFF
        else:
            if self.verbose:
                print(f"[kdp] ignoring async request {pkt.name} seq={pkt.seq}")

    # -- request/response -----------------------------------------------
    def request(self, request: int, body: bytes = b"", *,
                timeout: float | None = None,
                retries: int | None = None,
                expect_reply: bool = True) -> KdpPacket:
        """Send a request and wait for its reply.

        On timeout the *same* sequence number is retransmitted, which is
        exactly what makes the kernel re-send its saved reply
        (kdp_handler: seq == exception_seq - 1 -> retransmit saved reply).
        """
        timeout = self.timeout if timeout is None else timeout
        retries = self.retries if retries is None else retries

        last_reason = "timeout"
        for attempt in range(retries):
            seq = self.seq
            self._send_kdp(request, body, seq)
            if self.verbose:
                print(f"[kdp] TX {REQUEST_NAMES.get(request, request)} "
                      f"seq={seq} len={KDP_HDR_LEN + len(body)} "
                      f"(attempt {attempt + 1}/{retries})")

            if not expect_reply:
                self.seq = (seq + 1) & 0xFF
                return KdpPacket(request, False, seq, KDP_HDR_LEN + len(body), self.key, b"")

            deadline = time.monotonic() + timeout
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    break
                pkt = self._recv_kdp(remaining)
                if pkt is None:
                    break
                if pkt.is_reply:
                    if pkt.request == request and pkt.seq == seq:
                        if self.verbose:
                            print(f"[kdp] RX {pkt.name} seq={pkt.seq} "
                                  f"len={pkt.length}")
                        self.seq = (seq + 1) & 0xFF
                        return pkt
                    if self.verbose:
                        print(f"[kdp] ignoring stale reply {pkt.name} "
                              f"seq={pkt.seq} (want {REQUEST_NAMES.get(request, request)}/{seq})")
                else:
                    self._handle_async(pkt)

            last_reason = "timeout"
            if self.verbose:
                print(f"[kdp] retransmit {REQUEST_NAMES.get(request, request)} "
                      f"seq={self.seq}")

        raise KdpTimeout(
            f"{REQUEST_NAMES.get(request, request)} seq={self.seq}: "
            f"{last_reason} after {retries} attempt(s)"
        )

    def _reply(self, request: int, body: bytes = b"", **kw) -> KdpReply:
        pkt = self.request(request, body, **kw)
        error = KDPERR_NO_ERROR
        if len(pkt.body) >= 4:
            error = struct.unpack_from("<I", pkt.body, 0)[0]
        return KdpReply(pkt, error)

    # -- session control -------------------------------------------------
    def connect(self, greeting: bytes = b"greetings\x00") -> KdpReply:
        """KDP_CONNECT with seq=0.  Records the session key."""
        self.seq = 0
        body = struct.pack("<HH", self.reply_port, self.exception_port) + greeting
        reply = self._reply(KDP_CONNECT, body, timeout=self.connect_timeout)
        if reply.error != KDPERR_NO_ERROR:
            raise KdpProtocolError(
                f"CONNECT refused: {reply.error_name} ({reply.error})"
            )
        self.connected = True
        # The kernel echoes our key in the reply header.
        self.key = reply.packet.key
        return reply

    def reattach(self) -> KdpReply:
        """KDP_REATTACH (used to re-enter a session that is already up)."""
        body = struct.pack("<H", self.reply_port)
        reply = self._reply(KDP_REATTACH, body, timeout=self.connect_timeout)
        self.connected = True
        return reply

    def host_reboot(self) -> None:
        """KDP_HOSTREBOOT: reset the board from the debugger.

        kdp_udp.c handles this request inside kdp_connection_wait() by calling
        kdp_machine_reboot() directly, with the comment "should not return!". So
        this is the one way out of a KDP session on arm64, where DISCONNECT only
        clears the session and the debugger loop goes round again (T39).

        Sent as a bare header. No reply is expected and none arrives: the machine
        is on its way down. The caller should treat "no reply" as success and
        confirm with adb/COM3 rather than waiting for one.
        """
        # seq is irrelevant: kdp_udp.c checks request == KDP_HOSTREBOOT before
        # it looks at anything else, then reboots without replying.
        self._send_kdp(KDP_HOSTREBOOT, b"", self.seq)

    def disconnect(self) -> KdpReply:
        """KDP_DISCONNECT.  The kernel returns to kdp_connection_wait()."""
        reply = self._reply(KDP_DISCONNECT, b"")
        self.connected = False
        self.seq = 0
        return reply

    # -- information -----------------------------------------------------
    def version(self) -> dict:
        # kdp_version_reply_t has no error field: version/feature/pad0/pad1.
        pkt = self.request(KDP_VERSION_VERB, b"")
        if len(pkt.body) < 16:
            raise KdpProtocolError(f"short VERSION reply: {len(pkt.body)} bytes")
        vals = struct.unpack_from("<IIII", pkt.body, 0)
        self.remote_version = vals[0]
        self.remote_feature = vals[1]
        return {
            "version": vals[0],
            "feature": vals[1],
            "pad0": vals[2],
            "pad1": vals[3],
            "bp_supported": bool(vals[1] & KDP_FEATURE_BP),
        }

    def hostinfo(self) -> dict:
        # kdp_hostinfo_reply_t has no error field.
        pkt = self.request(KDP_HOSTINFO, b"")
        if len(pkt.body) < 12:
            raise KdpProtocolError(f"short HOSTINFO reply: {len(pkt.body)} bytes")
        cpus_mask, cpu_type, cpu_subtype = struct.unpack_from("<III", pkt.body, 0)
        return {
            "cpus_mask": cpus_mask,
            "cpu_count": bin(cpus_mask).count("1"),
            "cpu_type": cpu_type,
            "cpu_subtype": cpu_subtype,
        }

    def maxbytes(self) -> int:
        pkt = self.request(KDP_MAXBYTES, b"")
        if len(pkt.body) < 4:
            raise KdpProtocolError(f"short MAXBYTES reply: {len(pkt.body)} bytes")
        return struct.unpack_from("<I", pkt.body, 0)[0]

    def kernelversion(self) -> str:
        # kdp_kernelversion_reply_t is hdr + NUL-terminated version string.
        pkt = self.request(KDP_KERNELVERSION, b"")
        return pkt.body.split(b"\x00", 1)[0].decode("utf-8", "replace")

    # -- memory ----------------------------------------------------------
    def _read_chunked(self, verb: int, address: int, length: int,
                      tail: bytes = b"") -> bytes:
        """Read `length` bytes in MAX_KDP_DATA_SIZE chunks."""
        out = bytearray()
        offset = 0
        while offset < length:
            n = min(MAX_KDP_DATA_SIZE, length - offset)
            body = (struct.pack("<Q", (address + offset) & 0xFFFFFFFFFFFFFFFF)
                    + struct.pack("<I", n) + tail)
            reply = self._reply(verb, body)
            if reply.error != KDPERR_NO_ERROR:
                raise KdpProtocolError(
                    f"{REQUEST_NAMES.get(verb, verb)} error at "
                    f"0x{address + offset:x}: {reply.error_name}"
                )
            data = reply.data
            if not data:
                # arm64's KDPERR_ACCESS is hard-wired to NO_ERROR, so a
                # failure can surface as a zero-length reply.
                raise KdpProtocolError(
                    f"{REQUEST_NAMES.get(verb, verb)} returned no data at "
                    f"0x{address + offset:x} (len={n})"
                )
            out += data[:n]
            offset += len(data)
            if len(data) < n:
                # Short read: the kernel could not supply the whole chunk.
                break
        return bytes(out)

    def readmem64(self, address: int, length: int) -> bytes:
        """KDP_READMEM64 (64-bit virtual address)."""
        return self._read_chunked(KDP_READMEM64, address, length)

    def readphys64(self, address: int, length: int, lcpu: int = 0) -> bytes:
        """KDP_READPHYSMEM64 (64-bit physical address)."""
        return self._read_chunked(KDP_READPHYSMEM64, address, length,
                                  tail=struct.pack("<H", lcpu & 0xFFFF))

    # -- registers -------------------------------------------------------
    def readregs(self, cpu: int = 0, flavor: int = ARM_THREAD_STATE64) -> dict:
        """KDP_READREGS.  Returns a dict for AArch64 thread state."""
        body = struct.pack("<II", cpu & 0xFFFFFFFF, flavor & 0xFFFFFFFF)
        reply = self._reply(KDP_READREGS, body)
        if reply.error != KDPERR_NO_ERROR:
            raise KdpProtocolError(f"READREGS error: {reply.error_name}")
        data = reply.data
        if flavor == ARM_THREAD_STATE64:
            if len(data) < ARM_THREAD_STATE64_SIZE:
                raise KdpProtocolError(
                    f"short THREAD_STATE64: {len(data)} < {ARM_THREAD_STATE64_SIZE}"
                )
            x = list(struct.unpack_from("<29Q", data, 0))
            fp, lr, sp, pc = struct.unpack_from("<4Q", data, 29 * 8)
            cpsr, flags = struct.unpack_from("<2I", data, 29 * 8 + 32)
            regs = {"x": x, "fp": fp, "lr": lr, "sp": sp, "pc": pc,
                    "cpsr": cpsr, "flags": flags, "flavor": flavor}
            regs.update({f"x{i}": x[i] for i in range(29)})
            return regs
        return {"flavor": flavor, "raw": data}

    # -- execution control ----------------------------------------------
    def resume(self, cpu_mask: int = 0xFFFFFFFF) -> KdpReply:
        """KDP_RESUMECPUS."""
        return self._reply(KDP_RESUMECPUS, struct.pack("<I", cpu_mask & 0xFFFFFFFF))

    def suspend(self) -> KdpReply:
        """KDP_SUSPEND."""
        return self._reply(KDP_SUSPEND, b"")

    # -- breakpoints -----------------------------------------------------
    def bp_add(self, address: int) -> KdpReply:
        """KDP_BREAKPOINT64_SET (software breakpoint, 64-bit address)."""
        return self._reply(KDP_BREAKPOINT64_SET,
                           struct.pack("<Q", address & 0xFFFFFFFFFFFFFFFF))

    def bp_remove(self, address: int) -> KdpReply:
        """KDP_BREAKPOINT64_REMOVE."""
        return self._reply(KDP_BREAKPOINT64_REMOVE,
                           struct.pack("<Q", address & 0xFFFFFFFFFFFFFFFF))

    def step(self, *_args, **_kw) -> None:
        """Single-step is NOT available in this KDP implementation.

        XNU 17.7.11's kdp_req_t has no KDP_SW_STEP verb (the only step-related
        code in the tree is the AArch64 ESR_EL1 software-step exception
        handling in osfmk/arm64/sleh.c, which is unrelated to KDP).  Hardware
        single-step would have to be driven through the ARM debug registers,
        which KDP does not expose.
        """
        raise KdpUnsupported(
            "KDP_SW_STEP does not exist in XNU 17.7.11; "
            "single-step is not available over this KDP"
        )
