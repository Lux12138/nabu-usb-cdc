#!/usr/bin/env python3
"""nabu_mux.py -- one process, one COM port, both channels.

Why this exists
---------------
Windows gives a serial port to exactly one handle ("win32 only supports
exclusive access (not: False)", measured on kb-165). Until now that meant the
console and the KDP bridge took turns: stop one, start the other, lose the CDC
open (one boot gets one open), and re-discover the port. Every switch was a
chance to lose the board.

Both channels already share the wire, and they do not overlap:

    KDP          "KDPF" | ver | flags | len(u16) | crc32(u32) | ethernet frame
    SXNU1        "SXNU1 <session> <seq> <VERB> [args]\\n"   (also "SXNU1R" replies)

So one reader can hand each byte run to the right consumer, and one writer can
serialise both. That is what this does:

  * CDC -> split.  A KDPF deframer consumes KDPF frames; everything else
    accumulates as text and is scanned for SXNU1R reply lines.
  * SXNU1 commands -> collected from a command file appended to by the shell
    (same interface as nabu_console.py: `printf 'hello\\n' >> tag-cmd.txt`).
  * KDP <-> UDP:41139, same protocol and framing the standalone bridge uses,
    so kdp_cli.py and lldb's kdp-remote plugin work unchanged.
  * Everything received is appended to <tag>-com.bin so nothing is lost, and
    the console log goes to <tag>-ctl.txt as before.

What it does NOT do
-------------------
No symbol table, no host-side verb decoding beyond echoing lines. This is the
transport, not the analysis. Use nabu_console.py when you only want the
console; use this when a session needs to move between watching logs and
driving lldb without reopening the port.

Usage
-----
    python -u tools/nabu_mux.py --tag live --out E:/nabu/logs
    # then, in another shell:
    printf 'hello\\n' >> E:/nabu/logs/live-cmd.txt
    printf 'framestat\\n' >> E:/nabu/logs/live-cmd.txt
    printf 'kdp\\n' >> E:/nabu/logs/live-cmd.txt        # enter the debugger
    # KDP is already reachable on UDP 41139 -- no restart
    python kdp_client/kdp_cli.py --transport udp --host 127.0.0.1 \\
        --udp-port 41139 --fresh connect

The port stays open across the DEBUGGER verb, which is the whole point: the
device keeps answering on KDP and the console keeps logging, and switching
between them costs nothing.
"""
from __future__ import annotations

import argparse
import binascii
import json
import os
import queue
import re
import socket
import struct
import sys
import threading
import time
from datetime import datetime, timedelta, timezone

CST = timezone(timedelta(hours=8))
KDP_UDP_PORT = 41139
KDPF_MAGIC = b"KDPF"
KDPF_HDR = 12
CDC_FRAME_MAX = 1518
SXNU1_PREFIX = b"SXNU1"
SXNU1R_PREFIX = b"SXNU1R"


def now_hms() -> str:
    return datetime.now(CST).strftime("%H:%M:%S")


def crc32(body: bytes) -> int:
    return binascii.crc32(body) & 0xFFFFFFFF


# --------------------------------------------------------------------------
# KDPF v1: the device-side framing QcomKdpNet.c emits and accepts
# --------------------------------------------------------------------------

class KdpfDeframer:
    """Byte-fed state machine for KDPF frames. Tolerant of interleaved text.

    A KDPF frame can arrive split across reads or back to back. Anything that
    is not inside a frame is passed through to the text path, so a console line
    arriving between two KDP replies is not swallowed.
    """

    def __init__(self):
        self.buf = bytearray()
        self.frames = 0
        self.crc_errors = 0
        self.bad_headers = 0
        self.resyncs = 0
        self.oversize_drops = 0

    def feed(self, data: bytes):
        """Return (frames, text) -- frames are payloads, text is everything else."""
        self.buf.extend(data)
        out_frames = []
        out_text = bytearray()
        while True:
            idx = self.buf.find(KDPF_MAGIC)
            if idx < 0:
                # No magic. Keep a tail that could still be a partial magic.
                keep = len(KDPF_MAGIC) - 1
                if len(self.buf) > keep:
                    out_text.extend(self.buf[:len(self.buf) - keep])
                    del self.buf[:len(self.buf) - keep]
                break
            if idx:
                out_text.extend(self.buf[:idx])
                del self.buf[:idx]
            if len(self.buf) < KDPF_HDR:
                break
            ver, flags, length = struct.unpack_from("<BBH", self.buf, 4)
            want_crc = struct.unpack_from("<I", self.buf, 8)[0]
            if ver != 1 or flags != 0 or not (1 <= length <= CDC_FRAME_MAX):
                # Not one of ours after all: step past the magic and keep going.
                self.bad_headers += 1
                out_text.extend(self.buf[:4])
                del self.buf[:4]
                continue
            total = KDPF_HDR + length
            if len(self.buf) < total:
                break
            payload = bytes(self.buf[KDPF_HDR:total])
            del self.buf[:total]
            if crc32(payload) != want_crc:
                self.crc_errors += 1
                continue
            self.frames += 1
            out_frames.append(payload)
        return out_frames, bytes(out_text)

    def stats(self):
        return {
            "frames": self.frames,
            "crc_errors": self.crc_errors,
            "bad_headers": self.bad_headers,
            "resyncs": self.resyncs,
            "oversize_drops": self.oversize_drops,
        }


def kdpf_encode(payload: bytes) -> bytes:
    if not (1 <= len(payload) <= CDC_FRAME_MAX):
        raise ValueError("payload %d B out of range" % len(payload))
    return KDPF_MAGIC + struct.pack("<BBHI", 1, 0, len(payload), crc32(payload)) + payload


# --------------------------------------------------------------------------
# Ethernet wrapping for KDP: lldb sends a bare KDP packet, the device wants a
# complete Ethernet/IPv4/UDP frame
#
# The wrap and the unwrap are taken from kdp_client/kdp_proto.py, which is the
# implementation verified against the board. Re-deriving them here would give a
# second copy that can drift -- and the first attempt did: it picked MACs and a
# source IP that differ from XNU's kdp_send() layout. The device only checks the
# KDPF magic/len/crc before handing the payload to the KDP unserialiser, so both
# would work, but "works by accident" is not a property to rely on.
# --------------------------------------------------------------------------

def _proto():
    here = os.path.dirname(os.path.abspath(__file__))
    for cand in (os.path.join(here, "..", "kdp_client"),
                 os.path.join(here, "..", "kdp_net_bridge")):
        cand = os.path.normpath(cand)
        if os.path.isdir(cand) and cand not in sys.path:
            sys.path.insert(0, cand)
    import kdp_proto  # type: ignore
    return kdp_proto


def build_eth_frame(kdp_pkt: bytes) -> bytes:
    """Wrap a bare KDP packet the way XNU's kdp_send() lays it out."""
    return _proto().build_l2l3(kdp_pkt)


def extract_kdp_payload(eth: bytes):
    """Pull the KDP bytes out of an Ethernet+IPv4+UDP frame, or None."""
    return _proto().parse_l2l3(eth)


# --------------------------------------------------------------------------
# SXNU1: the console's text protocol
# --------------------------------------------------------------------------

class Sxnu1:
    """Builds request lines and tracks session/seq. Mirrors nabu_console.py."""

    def __init__(self):
        self.session = 0
        self.seq = 0
        self.hello_done = False

    def build(self, verb: str, arg: str = "") -> bytes:
        """Build one request line and advance the sequence.

        Two details, both copied from nabu_console.py because the device checks
        them and says nothing when they are wrong:

          * the trailing 8 hex digits are CRC-32 of the body before them;
          * self.seq holds the last committed number, so the request uses
            seq + 1. After HELLO adopts the device's last_seq verbatim, the next
            command must be last_seq + 1 -- sending last_seq itself is treated
            as a replay and dropped, which is how this script's second version
            lost every command.
        """
        if verb.upper() == "HELLO":
            body = "SXNU1 %016x %08x %s" % (0, 0, "HELLO")
        else:
            if not self.hello_done:
                raise RuntimeError("HELLO has not succeeded yet")
            self.seq = (self.seq + 1) & 0xFFFFFFFF
            body = "SXNU1 %016x %08x %s" % (self.session, self.seq, verb.upper())
        if arg:
            body += " " + arg
        return ("%s %08x\n" % (body, crc32(body.encode()))).encode()

    def note_reply(self, line: str) -> None:
        parts = line.split()
        if len(parts) < 4 or not parts[0].startswith("SXNU1R"):
            return
        try:
            session = int(parts[1], 16)
            seq = int(parts[2], 16)
        except ValueError:
            return
        if "HELLO" in parts[3]:
            # Take the device's counter verbatim -- last_seq is "the last one
            # that committed", and the next request reuses that number. This is
            # what nabu_console.py does (ctl.seq = last_seq, no +1) and it is the
            # behaviour the device accepts; starting at last_seq+1 made every
            # command after a restart time out.
            m = re.search(r"last_seq=([0-9a-fA-F]{8})", line)
            if m:
                self.seq = int(m.group(1), 16)
            else:
                self.seq = 0
            self.session = session
            self.hello_done = True
            return
        if session != self.session:
            return
        # self.seq must hold "the last seq the device committed". build() adds
        # one, so this stores `seq` VERBATIM. An earlier version stored seq+1
        # here while build() also added one: every successful command advanced
        # the counter by two, the next request skipped a number, and the device
        # silently refused it (QcomUsbControl.h:516 -- a frame whose seq is not
        # last_seq+1 is dropped without a reply). The failure looked exactly
        # like "the device stopped answering", which cost a whole round of
        # debugging in kdr6v: PING went out as seq=4 while the device expected
        # 3, so the zero reply proved nothing about the wire.
        if seq >= self.seq:
            self.seq = seq


# --------------------------------------------------------------------------
# The mux
# --------------------------------------------------------------------------

class Mux:
    def __init__(self, port: str, tag: str, outdir: str, verbose: bool,
                 kernel_path: str | None = None):
        try:
            import serial
        except ImportError:
            raise SystemExit("pyserial is required: python -m pip install pyserial")
        self._serial_mod = serial
        self.port = port
        self.serial = None
        self.verbose = verbose
        self.tag = tag
        self.outdir = outdir
        os.makedirs(outdir, exist_ok=True)
        # Open the sinks first, then wait for the port. nabu_console.py does the
        # same thing, and the reason matters: the round's whole point is to catch
        # the boot, so the console is started BEFORE flashing and spends the
        # wait scanning for the port. Opening the port here unconditionally is
        # how this script lost a round's opening log -- COM3 does not exist yet
        # at that moment and Serial() raises instead of waiting.
        self.raw_path = os.path.join(outdir, tag + "-com.bin")
        self.ctl_path = os.path.join(outdir, tag + "-ctl.txt")
        self.cmd_path = os.path.join(outdir, tag + "-cmd.txt")
        self.raw = open(self.raw_path, "ab", buffering=0)
        self.ctl = open(self.ctl_path, "a", encoding="utf-8", buffering=1)
        if not os.path.exists(self.cmd_path):
            open(self.cmd_path, "w", encoding="utf-8").close()
        self.cmd_pos = os.path.getsize(self.cmd_path)

        self.deframer = KdpfDeframer()
        self.sxnu = Sxnu1()
        self.reply_timeout = 6.0
        self._pending = None
        self._replies = queue.Queue()
        self._held_replies = []
        self.write_lock = threading.Lock()
        self.stop_event = threading.Event()

        # Host-side pseudo-commands need symbol addresses, which live in the
        # kernel image, not on the device. Optional: without it the pseudo
        # commands report why they cannot run instead of guessing.
        self.symtab = None
        self._sym_cache = {}
        if kernel_path:
            try:
                import symtab as _symtab
                self.symtab = _symtab.Symtab(kernel_path)
                self.log(">>> symbols loaded from %s" % kernel_path)
            except Exception as exc:
                self.log("! cannot load symbols from %s: %s" % (kernel_path, exc))

        self.peer = None
        self.peer_lock = threading.Lock()

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("0.0.0.0", KDP_UDP_PORT))
        self.sock.settimeout(0.05)

        self.text_buf = bytearray()
        self._dead_since = None
        self._gave_up = False
        self._backoff = 0.0
        self.counters = {
            "kdp_rx": 0, "kdp_tx": 0, "sxnu1_tx": 0, "sxnu1_rx": 0,
            "bytes_rx": 0, "bytes_tx": 0,
        }

    def log(self, msg: str) -> None:
        line = "%s %s" % (now_hms(), msg)
        try:
            self.ctl.write(line + "\n")
        except Exception:
            pass
        print(line, flush=True)

    def close(self) -> None:
        self.stop_event.set()
        for f in (self.raw, self.ctl):
            try:
                f.close()
            except Exception:
                pass
        try:
            self.serial.close()
        except Exception:
            pass
        try:
            self.sock.close()
        except Exception:
            pass

    # -- writes ----------------------------------------------------------
    def _write_cdc(self, wire: bytes, what: str) -> None:
        with self.write_lock:
            if self.serial is None:
                self.log("! %s dropped: port is not open" % what)
                return
            try:
                self.serial.write(wire)
                self.serial.flush()
            except Exception as exc:
                self.log("! serial write failed (%s): %s" % (what, exc))
                return
        self.counters["bytes_tx"] += len(wire)
        if self.verbose:
            self.log("TX %d B (%s)" % (len(wire), what))

    def sym_addr(self, name: str):
        """Resolve a kernel symbol to its address, using the loaded image.

        Two naming conventions are in play: llvm-nm prints a leading underscore
        (`_native_adreno_gpu_busy`) while symtab.py strips it
        (`native_adreno_gpu_busy`). Accept either spelling.
        """
        if name in self._sym_cache:
            return self._sym_cache[name]
        addr = None
        if self.symtab is not None:
            bare = name[1:] if name.startswith("_") else name
            prefixed = "_" + bare
            try:
                for entry in self.symtab.entries():
                    nm = entry[0]
                    if nm == bare or nm == prefixed:
                        addr = entry[1] if len(entry) > 1 else None
                        break
            except Exception:
                addr = None
        self._sym_cache[name] = addr
        return addr

    def read_words(self, addr: int, nwords: int):
        """Issue a READ and parse the reply into words.

        Returns None when the reply does not arrive. The READ reply carries hex
        bytes; anything longer than one chunk needs READNEXT, which this does
        not attempt -- the arrays here are all under 256 bytes.
        """
        nbytes = nwords * 4
        want = (self.sxnu.seq + 1) & 0xFFFFFFFF
        if not self.send_sxnu1("READ", "0x%x %d" % (addr, nbytes)):
            return None
        rx = self._wait_reply("READ", seq=want, timeout=self.reply_timeout)
        if not rx:
            return None
        m = re.search(r"data=([0-9a-fA-F]+)", rx)
        if not m:
            return None
        raw = bytes.fromhex(m.group(1))
        if len(raw) < nbytes:
            return None
        return list(struct.unpack("<%dI" % nwords, raw[:nbytes]))

    def read_qwords(self, addr: int, n: int):
        nbytes = n * 8
        want = (self.sxnu.seq + 1) & 0xFFFFFFFF
        if not self.send_sxnu1("READ", "0x%x %d" % (addr, nbytes)):
            return None
        rx = self._wait_reply("READ", seq=want, timeout=self.reply_timeout)
        if not rx:
            return None
        m = re.search(r"data=([0-9a-fA-F]+)", rx)
        if not m:
            return None
        raw = bytes.fromhex(m.group(1))
        if len(raw) < nbytes:
            return None
        return list(struct.unpack("<%dQ" % n, raw[:nbytes]))

    def _wait_reply(self, verb: str, seq: int | None = None, timeout: float = 6.0):
        """Wait for the reply to `verb` at `seq`.

        Matching on the sequence number matters: two pseudo-commands issued back
        to back put two READs in flight, and a queue that only looked at the verb
        let the second one consume the first one's reply. The device stamps every
        reply with the seq it answers, so requiring it makes the pairing exact.
        """
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                item = self._replies.get(timeout=0.2)
            except queue.Empty:
                continue
            if not item:
                continue
            if verb.upper() not in item:
                self._stash_reply(item)
                continue
            if seq is not None:
                m = re.search(r"\s([0-9a-fA-F]{8})\s+%s" % verb.upper(), item)
                if m and int(m.group(1), 16) != (seq & 0xFFFFFFFF):
                    self._stash_reply(item)
                    continue
            # The reply arrived, so the resync watchdog must stand down. _line
            # clears this too, but a reply can also be picked up from the queue
            # here, and leaving the flag set makes the next _resync_if_stuck
            # fire a HELLO that burns a sequence number.
            self._pending = None
            return item
        return None

    def _stash_reply(self, item: str) -> None:
        """Hold a reply that belongs to a different request."""
        self._held_replies.append(item)
        if len(self._held_replies) > 32:
            del self._held_replies[:16]

    def _dispatch_host_command(self, verb: str, arg: str) -> bool:
        """Run a host-side pseudo-command. Returns False if `verb` is not one.

        These names look like device verbs but are not: the console issues READs
        and decodes the arrays itself. Forwarding them to the device gets them
        rejected, which is why FRAMESTAT appeared to stop working under this
        script.
        """
        v = verb.upper()
        if v not in ("FRAMESTAT", "GPUBUSY", "A6XXSTATS"):
            return False
        if self.symtab is None:
            self.log("! %s needs --kernel <image> to resolve symbol addresses" % v)
            return True
        import nabu_hostcmd as HC
        try:
            if v == "FRAMESTAT":
                a = self.sym_addr(HC.SYMBOLS["frame_hist"])
                if a is None:
                    self.log("! %s not in this image" % HC.SYMBOLS["frame_hist"])
                    return True
                w = self.read_words(a, HC.COUNTS["frame_hist"])
                for line in (HC.decode_frame_hist(w) if w else ["! framestat read failed"]):
                    self.log(line)
            elif v == "GPUBUSY":
                a = self.sym_addr(HC.SYMBOLS["gpu_busy"])
                if a is None:
                    self.log("! %s not in this image" % HC.SYMBOLS["gpu_busy"])
                    return True
                w = self.read_qwords(a, 8)
                for line in (HC.decode_gpu_busy(w) if w else ["! gpubusy read failed"]):
                    self.log(line)
            else:
                a = self.sym_addr(HC.SYMBOLS["a6xx_stats"])
                if a is None:
                    self.log("! %s not in this image" % HC.SYMBOLS["a6xx_stats"])
                    return True
                w = self.read_words(a, HC.COUNTS["a6xx_stats"])
                for line in (HC.decode_a6xx_stats(w) if w else ["! a6xxstats read failed"]):
                    self.log(line)
        except Exception as exc:
            self.log("! %s failed: %s" % (v, exc))
        return True

    def send_sxnu1(self, verb: str, arg: str = "") -> bool:
        # "kdp" is a host-side shorthand, matching nabu_console.py: it is not a
        # device verb, it sends DEBUGGER with an empty payload, which makes the
        # kernel call Debugger("qcom_usb_debug") and park for KDP. The port
        # stays open here, which is the point of this script -- the console
        # would have to release it.
        if verb.lower() == "kdp":
            verb, arg = "DEBUGGER", arg or ""
            self.log(">>> entering the debugger (DEBUGGER verb)")
        try:
            wire = self.sxnu.build(verb, arg)
        except RuntimeError as exc:
            self.log("! %s" % exc)
            return False
        self.counters["sxnu1_tx"] += 1
        self._pending = (verb.upper(), self.sxnu.seq, time.time())
        self.log("TX %s %s (seq=%d)" % (verb.upper(), arg, self.sxnu.seq))
        self._write_cdc(wire, "SXNU1")
        return True

    def _resync_if_stuck(self) -> None:
        """Re-sync the sequence after a command that never answered.

        The device only advances state->last_seq when a request commits, while
        this side advances for every attempt. One dropped or rejected frame
        therefore leaves the two permanently out of step and every later command
        is silently refused -- the failure looks like "commands stopped working"
        with nothing in the log. nabu_console.py solves it by replaying HELLO,
        which is read-only and re-reports both session and last_seq; this is the
        same recovery, driven by a timeout instead of a per-command retry.
        """
        if not self._pending or not self.sxnu.hello_done:
            return
        verb, seq, at = self._pending
        if verb == "HELLO":
            return
        if time.time() - at < self.reply_timeout:
            return
        self._pending = None
        self.log("! no reply for %s (seq=%d) in %.0fs; resyncing with HELLO"
                 % (verb, seq, self.reply_timeout))
        # Do NOT roll the counter back by hand: HELLO's reply carries last_seq
        # and note_reply adopts it, so the next build() is correct on its own.
        # An earlier version subtracted one here as well, which double-counted
        # and made the following command skip a number -- the device then
        # refused it and the session stayed broken.
        try:
            wire = self.sxnu.build("HELLO")
        except RuntimeError:
            return
        self._write_cdc(wire, "SXNU1")

    # -- reads -----------------------------------------------------------
    def _try_reopen(self) -> None:
        """Reopen the port after a failure. Best-effort, called only while dead.

        On Windows a device that left the bus cannot be reopened until it
        re-enumerates, so this is expected to fail for a while; that is why it
        runs with backoff and logs nothing on failure.
        """
        try:
            import serial
        except ImportError:
            return
        with self.write_lock:
            try:
                if self.serial is not None and self.serial.is_open:
                    self.serial.close()
            except Exception:
                pass
            try:
                self.serial = self._serial_mod.Serial(
                    self.port, 115200, timeout=0.02, write_timeout=2.0)
                self.log(">>> reopened %s" % self.port)
            except Exception:
                self.serial = None

    def _reader(self) -> None:
        while not self.stop_event.is_set():
            if self.serial is None:
                self.stop_event.wait(0.25)
                self._try_reopen()
                continue
            try:
                data = self.serial.read(4096)
            except Exception as exc:
                if self.stop_event.is_set():
                    return
                # The device can drop off the bus mid-session (power cycle,
                # WDT, a bad flash). pyserial then raises on every read, and a
                # naive loop turns that into a wall of ClearCommError text that
                # buries whatever came before it. Log the transition once, back
                # off, and try to reopen; say nothing while the failure repeats.
                if self._dead_since is None:
                    self._dead_since = time.time()
                    self.log("! serial read failed: %s" % exc)
                    self.log("! waiting for the device to come back (reopen loop)")
                elif self._dead_since and time.time() - self._dead_since > 60 and not self._gave_up:
                    self._gave_up = True
                    self.log("! device gone for 60 s; still retrying quietly")
                self._backoff = min(self._backoff * 2, 2.0) if self._backoff else 0.25
                self.stop_event.wait(self._backoff)
                self._try_reopen()
                continue

            if self._dead_since is not None:
                self.log(">>> device is back after %.1fs" % (time.time() - self._dead_since))
                self._dead_since = None
                self._gave_up = False
                self._backoff = 0.0

            if not data:
                continue
            self.counters["bytes_rx"] += len(data)
            try:
                self.raw.write(data)
            except Exception:
                pass
            frames, text = self.deframer.feed(data)
            for payload in frames:
                self.counters["kdp_rx"] += 1
                kdp = extract_kdp_payload(payload)
                if kdp is None:
                    self.counters["kdp_unparsed"] = self.counters.get("kdp_unparsed", 0) + 1
                    if self.verbose:
                        self.log("KDP frame %d B not IPv4/UDP, dropped: %s"
                                 % (len(payload), payload[:24].hex()))
                    continue
                with self.peer_lock:
                    peer = self.peer
                if peer is None:
                    if self.verbose:
                        self.log("KDP reply with no UDP peer yet, dropped")
                    continue
                try:
                    self.sock.sendto(kdp, peer)
                except Exception as exc:
                    self.log("! UDP send failed: %s" % exc)
                if self.verbose:
                    self.log("KDP->UDP %d B" % len(kdp))
            if text:
                self._text(text)

    def _text(self, data: bytes) -> None:
        self.text_buf.extend(data)
        while True:
            nl = self.text_buf.find(b"\n")
            if nl < 0:
                # Keep the buffer bounded when a burst has no newline.
                if len(self.text_buf) > 65536:
                    del self.text_buf[:len(self.text_buf) - 4096]
                break
            line = bytes(self.text_buf[:nl])
            del self.text_buf[:nl + 1]
            self._line(line)

    def _line(self, raw: bytes) -> None:
        text = raw.decode("utf-8", "replace").rstrip("\r")
        if not text:
            return
        if text.startswith("SXNU1R") or text.startswith("SXNU1"):
            self.counters["sxnu1_rx"] += 1
            self._pending = None
            self.sxnu.note_reply(text)
            self._replies.put(text)
            self.log("RX %s" % text)
            return
        # Everything else is kernel log. Echo it so the terminal is useful, and
        # keep it in -ctl.txt next to the protocol traffic.
        try:
            self.ctl.write("%s LOG %s\n" % (now_hms(), text))
        except Exception:
            pass
        print(text, flush=True)

    # -- UDP -------------------------------------------------------------
    def _udp(self) -> None:
        while not self.stop_event.is_set():
            try:
                data, addr = self.sock.recvfrom(65535)
            except socket.timeout:
                continue
            except Exception as exc:
                if self.stop_event.is_set():
                    return
                self.log("! UDP recv failed: %s" % exc)
                continue
            with self.peer_lock:
                self.peer = addr
            # lldb sends a bare KDP packet; kdp_cli sends a full Ethernet frame.
            if len(data) >= 14 and data[12:14] == b"\x08\x00":
                frame = data
            else:
                frame = build_eth_frame(data)
            self.counters["kdp_tx"] += 1
            try:
                wire = kdpf_encode(frame)
            except ValueError as exc:
                self.log("! cannot frame: %s" % exc)
                continue
            self._write_cdc(wire, "KDP")

    # -- command file ----------------------------------------------------
    def _commands(self) -> None:
        """Consume appended lines, advancing the offset one line at a time.

        Advancing to the file size before the loop dropped commands: a pseudo
        command can block for its reply timeout, and anything queued behind it
        was skipped while cmd_pos had already moved past it. Since the console
        interface is "append and forget", a silently skipped line looks exactly
        like a device that stopped answering.
        """
        while not self.stop_event.is_set():
            try:
                size = os.path.getsize(self.cmd_path)
            except OSError:
                size = self.cmd_pos
            while size > self.cmd_pos:
                try:
                    with open(self.cmd_path, "rb") as f:
                        f.seek(self.cmd_pos)
                        raw = f.readline()
                except OSError:
                    break
                if not raw:
                    break
                self.cmd_pos += len(raw)
                line = raw.decode("utf-8", "replace").strip()
                if not line:
                    continue
                parts = line.split()
                if self.verbose:
                    self.log(">>> dispatch: %s" % parts[0])
                # One bad line must not stop the rest: the console contract is
                # append-and-forget, and a pseudo-command can take seconds.
                try:
                    if self._dispatch_host_command(parts[0], " ".join(parts[1:])):
                        continue
                    if not self.send_sxnu1(parts[0], " ".join(parts[1:])):
                        self.log("! command dropped: %s" % line)
                except Exception as exc:
                    self.log("! %s raised: %s" % (parts[0], exc))
            self._resync_if_stuck()
            self.stop_event.wait(0.2)

    # -- run -------------------------------------------------------------
    def _open_port(self, wait_s: float, tries_gap: float = 0.2) -> bool:
        """Wait for the port to appear, then open it.

        This is the behaviour that makes the round possible: the console is
        started before flashing, so it must survive a few tens of seconds with
        no device at all. list_ports can also report a port a few hundred ms
        before the driver finishes binding, so an immediate open can raise even
        when the name is there -- hence the retry inside the window.
        """
        deadline = time.time() + wait_s
        attempt = 0
        last_note = -999.0
        while True:
            attempt += 1
            try:
                self.serial = self._serial_mod.Serial(
                    self.port, 115200, timeout=0.02, write_timeout=2.0)
                self.log(">>> CDC OPEN OK on %s (attempt %d)" % (self.port, attempt))
                return True
            except Exception as exc:
                self.serial = None
                if time.time() >= deadline:
                    self.log(">>> CDC NOT FOUND on %s after %.0fs (%d attempts): %s"
                             % (self.port, wait_s, attempt, exc))
                    return False
                el = wait_s - (deadline - time.time())
                if el - last_note >= 2.0:
                    last_note = el
                    self.log(">>> waiting for %s (%.0fs, %d attempts)"
                             % (self.port, el, attempt))
                self.stop_event.wait(tries_gap)

    def run(self, wait_s: float = 400.0) -> int:
        self.log(">>> mux starting  tag=%s  port=%s" % (self.tag, self.port))
        self.log(">>> SXNU1: printf '<verb> [args]\\n' >> %s" % self.cmd_path)
        self.log(">>> KDP:   UDP %d (framing kdpf, bare-KDP in, Ethernet out)" % KDP_UDP_PORT)
        if not self._open_port(wait_s):
            self.close()
            return 3
        threads = [
            threading.Thread(target=self._reader, name="cdc", daemon=True),
            threading.Thread(target=self._udp, name="udp", daemon=True),
            threading.Thread(target=self._commands, name="cmd", daemon=True),
        ]
        for t in threads:
            t.start()
        last_report = time.time()
        try:
            while not self.stop_event.is_set():
                time.sleep(0.5)
                if time.time() - last_report >= 30:
                    last_report = time.time()
                    self.log("STAT %s kdp_rx=%d kdp_tx=%d kdp_drop=%d sxnu1_rx=%d sxnu1_tx=%d"
                             % (self.deframer.stats(), self.counters["kdp_rx"],
                                self.counters["kdp_tx"],
                                self.counters.get("kdp_unparsed", 0),
                                self.counters["sxnu1_rx"],
                                self.counters["sxnu1_tx"]))
        except KeyboardInterrupt:
            self.log(">>> interrupted")
        finally:
            self.log(">>> stopping")
            self.close()
        return 0


def main(argv=None) -> int:
    global KDP_UDP_PORT
    p = argparse.ArgumentParser(
        prog="nabu_mux.py",
        description="One COM port, SXNU1 console and KDP/UDP at the same time.",
    )
    p.add_argument("--port", default=os.environ.get("NABU_PORT", "COM3"))
    p.add_argument("--tag", default="live")
    p.add_argument("--out", default="E:/nabu/logs")
    p.add_argument("--udp-port", type=int, default=KDP_UDP_PORT)
    p.add_argument("--wait", type=float, default=400.0,
                   help="seconds to wait for the port (default 400, same as "
                        "nabu_console.py: start the console before flashing)")
    p.add_argument("--verbose", action="store_true")
    p.add_argument("--kernel", default=None,
                   help="kernel.macho of the build on the board; enables the "
                        "host-side pseudo-commands (framestat / gpubusy / "
                        "a6xxstats), which need symbol addresses")
    a = p.parse_args(argv)

    KDP_UDP_PORT = a.udp_port
    mux = Mux(a.port, a.tag, a.out, a.verbose, a.kernel)
    return mux.run(a.wait)


if __name__ == "__main__":
    sys.exit(main())
