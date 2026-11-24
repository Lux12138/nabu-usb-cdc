"""kdp_udp.py -- UDP transport for kdp_proto.KdpSession.

This lets the existing, unmodified ``KdpSession``/``kdp_cli.py`` talk to the
``kdp_net_bridge`` program (``../kdp_net_bridge/kdp_net_bridge.py``) instead of
a COM port.

The bridge runs in ``--udp-mode eth`` for this path, i.e. one UDP datagram
carries one complete Ethernet + IP/UDP + KDP frame, which is exactly what
``KdpSession._send_kdp()`` hands to a transport.  Nothing in ``kdp_proto.py``
had to change.

``--udp-mode kdp`` (the Apple-lldb-compatible mode, bare KDP packets) is *not*
used here: lldb has its own client and does not need this module.

Usage
-----
    python kdp_cli.py --transport udp --host 127.0.0.1 --udp-port 41139 connect
    python kdp_cli.py --transport udp --host 127.0.0.1 version

Start the bridge first:

    python ../kdp_net_bridge/kdp_net_bridge.py --port COM21 \\
        --udp-port 41139 --udp-mode eth
"""

from __future__ import annotations

import socket
import time
from collections import deque


class UdpTransport:
    """pyserial-free transport carrying raw Ethernet frames in UDP datagrams.

    Implements the same surface ``kdp_serial.SerialTransport`` exposes:
    ``send_packet(bytes)``, ``receive_packet(timeout)``, ``close()``.
    """

    def __init__(self, host: str = "127.0.0.1", port: int = 41139,
                 timeout: float = 0.2, verbose: bool = False) -> None:
        self.host = host
        self.port = int(port)
        self.timeout = float(timeout)
        self.verbose = verbose
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Bind explicitly so the bridge always knows where to send replies.
        self.sock.bind(("0.0.0.0", 0))
        self.sock.settimeout(self.timeout)
        self.local_addr = self.sock.getsockname()
        self.dest = (self.host, self.port)
        self._queue: deque[bytes] = deque()
        self.frames_tx = 0
        self.frames_rx = 0

    def send_packet(self, packet: bytes) -> None:
        """Send one complete Ethernet frame as a single UDP datagram."""
        if len(packet) > 65507:
            raise OSError(f"UDP payload too large: {len(packet)} bytes")
        self.sock.sendto(packet, self.dest)
        self.frames_tx += 1
        if self.verbose:
            print(f"[udp] TX {len(packet)} B -> {self.host}:{self.port}")

    def receive_packet(self, timeout: float = 1.0) -> bytes | None:
        """Return one datagram, waiting up to `timeout` seconds."""
        deadline = time.monotonic() + timeout
        while True:
            if self._queue:
                self.frames_rx += 1
                packet = self._queue.popleft()
                if self.verbose:
                    print(f"[udp] RX {len(packet)} B")
                return packet
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.sock.settimeout(min(remaining, self.timeout))
            try:
                data, addr = self.sock.recvfrom(65535)
            except socket.timeout:
                continue
            if self.verbose:
                print(f"[udp] RX {len(data)} B from {addr[0]}:{addr[1]}")
            self._queue.append(data)

    def resync(self) -> None:
        """Drop buffered datagrams (the UDP analogue of flushing the FIFO)."""
        self._queue.clear()

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def open_udp(host: str = "127.0.0.1", port: int = 41139,
             timeout: float = 0.2, verbose: bool = False) -> UdpTransport:
    transport = UdpTransport(host=host, port=port, timeout=timeout,
                             verbose=verbose)
    if verbose:
        print(f"[udp] local {transport.local_addr[0]}:{transport.local_addr[1]} "
              f"-> {host}:{port}")
    return transport
