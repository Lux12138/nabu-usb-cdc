#!/usr/bin/env python3
"""Self-test for nabu_mux.py -- no hardware, no pyserial needed.

Checks the two pieces that can silently break the wire:
  * KDPF v1 framing round-trips, splits across reads, tolerates text between
    frames, and rejects a corrupted CRC;
  * the Ethernet/IPv4/UDP wrapper lldb's bare KDP packets need matches what
    the standalone bridge builds, and unwraps back.
"""
import os
import sys
import struct

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nabu_mux as M


def test_kdpf_roundtrip():
    payload = bytes(range(64))
    wire = M.kdpf_encode(payload)
    d = M.KdpfDeframer()
    frames, text = d.feed(wire)
    assert frames == [payload], frames
    assert text == b"", text
    print("PASS  KDPF single frame round-trip (%d B -> %d B wire)" % (len(payload), len(wire)))


def test_kdpf_split():
    payload = b"A" * 300
    wire = M.kdpf_encode(payload)
    d = M.KdpfDeframer()
    got = []
    txt = b""
    for i in range(0, len(wire), 7):
        f, t = d.feed(wire[i:i + 7])
        got.extend(f)
        txt += t
    assert got == [payload], "split reassembly failed"
    assert txt == b"", txt
    print("PASS  KDPF reassembles across 7-byte reads")


def test_kdpf_text_between_frames():
    a = M.kdpf_encode(b"first")
    b = M.kdpf_encode(b"second")
    d = M.KdpfDeframer()
    frames, text = d.feed(a + b"SXNU1R abc 00000001 PING OK\n" + b)
    assert frames == [b"first", b"second"], frames
    assert b"PING OK" in text, text
    print("PASS  text between two KDPF frames is separated out")


def test_kdpf_bad_crc():
    wire = bytearray(M.kdpf_encode(b"payload"))
    wire[-1] ^= 0xFF
    d = M.KdpfDeframer()
    frames, _ = d.feed(bytes(wire))
    assert frames == [], "corrupted frame must be dropped"
    assert d.crc_errors == 1, d.stats()
    print("PASS  corrupted CRC dropped, counted")


def test_eth_wrap():
    kdp = b"\x81\x00\x00\x00" + b"\x00" * 8
    frame = M.build_eth_frame(kdp)
    assert len(frame) == 14 + 20 + 8 + len(kdp), len(frame)
    back = M.extract_kdp_payload(frame)
    assert back == kdp, back
    # Verify the IPv4 header checksum the way a router does: summing the
    # header including the checksum field folds to 0xFFFF.
    ip = frame[14:34]
    assert len(ip) == 20, len(ip)
    s = 0
    for i in range(0, 20, 2):
        s += (ip[i] << 8) | ip[i + 1]
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    assert s == 0xFFFF, "IP checksum invalid: 0x%04x" % s
    print("PASS  Ethernet wrap/unwrap, IP checksum verifies")


def test_sxnu1_seq():
    s = M.Sxnu1()
    # The line must end with the CRC-32 of the body, otherwise the device drops
    # it without a word. Check the shape and that the CRC field is present.
    hello = s.build("HELLO")
    assert hello.startswith(b"SXNU1 0000000000000000 00000000 HELLO "), hello
    assert hello.endswith(b"\n"), hello
    body, _, crc_hex = hello.rstrip(b"\n").rpartition(b" ")
    assert len(crc_hex) == 8 and int(crc_hex, 16) == M.crc32(body), hello
    s.note_reply("SXNU1R 1122334455667788 00000000 HELLO OK session=1122334455667788")
    assert s.hello_done and s.session == 0x1122334455667788 and s.seq == 0, s.seq
    ping = s.build("PING")
    # build() advances first, so the first command after HELLO is seq 1.
    assert ping.startswith(b"SXNU1 1122334455667788 00000001 PING "), ping
    pbody, _, pcrc = ping.rstrip(b"\n").rpartition(b" ")
    assert int(pcrc, 16) == M.crc32(pbody), ping
    s.note_reply("SXNU1R 1122334455667788 00000001 PING OK")
    assert s.seq == 2, s.seq
    # A later HELLO on the same session must adopt the device's counter verbatim:
    # last_seq is "the last one that committed" and the next request reuses it.
    s.note_reply("SXNU1R 1122334455667788 00000000 HELLO OK session=1122334455667788 last_seq=0000000a")
    assert s.seq == 0x0a, "HELLO must adopt last_seq verbatim, got %d" % s.seq
    print("PASS  SXNU1 session/seq tracking, CRC field present, HELLO adopts last_seq")


if __name__ == "__main__":
    test_kdpf_roundtrip()
    test_kdpf_split()
    test_kdpf_text_between_frames()
    test_kdpf_bad_crc()
    test_eth_wrap()
    test_sxnu1_seq()
    print("\n6/6 passed")
