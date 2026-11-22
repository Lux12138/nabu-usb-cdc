#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build a complete KDP serial frame for this kernel, from source, not guesswork.

Two layers, both verified against the tree in use:

A) Serial framing -- osfmk/kdp/kdp_serial.c
     out(0xFA)
     out(esc(crc32(packet) little-endian, 4 bytes))
     out(0xFA)
     for b in packet: out(esc(b))
     out(0xFB)
   escape set {0xFA, 0xFB, 0xFE, 0x0A}, escaped as 0xFE followed by ~byte.
   dsBuffer is static in kdp_unserialize_packet(), so the frame may be
   delivered across several KDPPUSH commands.

B) Packet layout -- osfmk/kdp/kdp_udp.c
   kdp_poll() requires, in order:
     len >= sizeof(ether_header) + sizeof(kdp_udpiphdr)   == 14 + 28
     ntohs(eh->ether_type) == ETHERTYPE_IP                (0x0800)
     ui->ui_pr == IPPROTO_UDP                             (17)
     ip->ip_hl <= sizeof(struct kdp_ip) >> 2              (5)
     ntohs(ui->ui_dport) == KDP_REMOTE_PORT               (41139 == 0xa0b3)
   It does NOT verify the IP checksum, so ip_sum can stay 0. ui_ulen drives the
   payload length: pkt.len = ntohs(ui_ulen) - sizeof(kdp_udphdr).

   struct kdp_udpiphdr is an OVERLAY: kdp_ipovly (28 B, the IP header) is
   unioned with kdp_udphdr (8 B) at its tail, so the whole thing is 28 bytes,
   not 36. Field offsets used below:

     ether_dhost  0..5    (broadcast ff:ff:ff:ff:ff:ff)
     ether_shost  6..11   ("serial" -- kdp_init fakes this MAC)
     ether_type   12..13  = 0x0800 big-endian
     ip_xhl/ip_xv 14      = 0x45  (hl=5, version=4)
     ip_xtos      15      = 0
     ip_xlen      16..17  total length, big-endian
     ip_id        18..19
     ip_off       20..21
     ip_ttl       22      = 60
     ip_p         23      = 17 (UDP)
     ip_sum       24..25
     ip_src       26..29
     ip_dst       30..33
     uh_sport     34..35
     uh_dport     36..37  = 41139
     uh_ulen      38..39  udp length = payload + 8
     uh_sum       40..41

Usage:
  python kdp_frame.py connect [--key 1337c0de] [--seq 0]
  python kdp_frame.py packet <hexpayload-of-kdp-packet>
  python kdp_frame.py hostinfo
"""
import sys
import zlib

START, END, ESC = 0xFA, 0xFB, 0xFE
ESC_SET = {START, END, ESC, 0x0A}

KDP_REMOTE_PORT = 41139
ETHERTYPE_IP = 0x0800
IPPROTO_UDP = 17
KDP_SERIAL_IPADDR = 0xABADBABE

# kdp_init() fakes the MAC as the ASCII bytes "serial"
SERIAL_MAC = b"serial"


def esc(data: bytes) -> bytes:
    out = bytearray()
    for b in data:
        if b in ESC_SET:
            out.append(ESC)
            out.append((~b) & 0xFF)
        else:
            out.append(b)
    return bytes(out)


def serial_frame(packet: bytes) -> bytes:
    crc = zlib.crc32(packet) & 0xFFFFFFFF
    out = bytearray([START])
    out += esc(bytes([crc & 0xFF, (crc >> 8) & 0xFF, (crc >> 16) & 0xFF, (crc >> 24) & 0xFF]))
    out.append(START)
    out += esc(packet)
    out.append(END)
    return bytes(out)


# Cross-check note (T38-a, 2026-11-23):
# The reference frame captured from a real kdp_client connect() is 71 B and
# matches this generator byte-for-byte EXCEPT ip_id (they use a counter, we use
# 0) and ip_sum (they compute RFC1071, we leave 0). kdp_poll() verifies neither,
# so both are accepted; only the CRC differs as a consequence. When in doubt use
# the captured frame from T38-a, it came off the wire.


def kdp_hdr(request: int, seq: int, length: int, key: int) -> bytes:
    """kdp_hdr_t: request|is_reply<<7, seq, len(LE16), key(LE32)."""
    return bytes([request & 0x7F, seq & 0xFF,
                  length & 0xFF, (length >> 8) & 0xFF]) + key.to_bytes(4, "little")


def net_packet(kdp: bytes, sport: int = 41140, dport: int = KDP_REMOTE_PORT) -> bytes:
    """Wrap a kdp packet in ether + ip/udp, exactly as kdp_send() would."""
    udp_len = len(kdp) + 8
    ip_len = 28 + len(kdp)          # overlay: ipovly(28) already covers udphdr
    eth = bytearray()
    eth += b"\xff" * 6              # ether_dhost broadcast
    eth += SERIAL_MAC               # ether_shost
    eth += ETHERTYPE_IP.to_bytes(2, "big")
    ip = bytearray(28)
    ip[0] = 0x45                    # version 4, header length 5 words
    ip[1] = 0                       # tos
    ip[2:4] = ip_len.to_bytes(2, "big")
    ip[4:6] = b"\x00\x00"           # id
    ip[6:8] = b"\x00\x00"           # off
    ip[8] = 60                      # ttl
    ip[9] = IPPROTO_UDP
    ip[10:12] = b"\x00\x00"         # checksum -- kdp_poll never verifies it
    ip[12:16] = (0).to_bytes(4, "big")             # ip_src
    ip[16:20] = KDP_SERIAL_IPADDR.to_bytes(4, "big")  # ip_dst
    ip[20:22] = sport.to_bytes(2, "big")
    ip[22:24] = dport.to_bytes(2, "big")
    ip[24:26] = udp_len.to_bytes(2, "big")
    ip[26:28] = b"\x00\x00"         # udp checksum
    return bytes(eth) + bytes(ip) + kdp


def connect_packet(key=0x1337C0DE, seq=0, reply_port=41140, exc_port=41145):
    """KDP_CONNECT as kdp_protocol.h defines it -- NOT a bare header.

        kdp_hdr_t hdr;              /* 8 B */
        uint16_t  req_reply_port;   /* 2 B, host reply port */
        uint16_t  exc_note_port;    /* 2 B, host exception port */
        char      greeting[0];      /* nul-terminated greeting */

    len = 8 + 2 + 2 + len(greeting).
    """
    greeting = b"greetings" + bytes([0])
    # LITTLE endian: the captured reference frame from T38-a carries
    # b4 a0 / b9 a0, which is 0xa0b4 / 0xa0b9 stored little-endian.
    body = reply_port.to_bytes(2, "little") + exc_port.to_bytes(2, "little") + greeting
    return kdp_hdr(0, seq, 8 + len(body), key) + body


def hostinfo_packet(key: int, seq: int) -> bytes:
    return kdp_hdr(13, seq, 8, key)  # KDP_HOSTINFO


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd = argv[1]
    key, seq = 0x1337C0DE, 0
    for i, a in enumerate(argv):
        if a == "--key" and i + 1 < len(argv):
            key = int(argv[i + 1], 0)
        if a == "--seq" and i + 1 < len(argv):
            seq = int(argv[i + 1], 0)

    if cmd == "connect":
        kdp = connect_packet(key, seq)
    elif cmd == "hostinfo":
        kdp = hostinfo_packet(key, seq)
    elif cmd == "packet" and len(argv) >= 3:
        kdp = bytes.fromhex(argv[2])
    else:
        print(__doc__)
        return 1

    pkt = net_packet(kdp)
    fr = serial_frame(pkt)
    print("kdp packet (%3d B): %s" % (len(kdp), kdp.hex()))
    print("net packet (%3d B): %s" % (len(pkt), pkt.hex()))
    print("serial frame(%3d B): %s" % (len(fr), fr.hex()))
    print("crc32(net)        : %08x" % (zlib.crc32(pkt) & 0xFFFFFFFF))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
