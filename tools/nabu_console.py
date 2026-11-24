#!/usr/bin/env python3
"""nabu 一体化串口控制台（单一脚本 / 单一句柄）。

一个进程同时完成：
  1) 采集：reader 线程把所有 CDC 字节写到 <tag>-com.bin（完整日志，含早期）
  2) 控制：命令文件 append 一行即发一条 SXNU1 帧（HELLO/READ/READPA/WRITEPA/
     BOOTARGS/VERSION/DUMPUFS/DUMLIST/REBOOT...），回复写 <tag>-ctl.txt
  3) 自动脚本：--script FILE 预置命令，open COM3 后立即紧凑发完（抓短窗口）
  4) 武装复位：--arm-at SEC，设备 uptime 到 SEC 自动发 REBOOT（回 recovery）
  5) 判据：被动记录字节增量/命令成败到 <tag>-events.jsonl（不主动发判据帧）

不再需要 cdc-console.py / linkwatch.py / capture.py —— 全在这里。

用法：
  # 标准一轮（先起，再刷机；COM3 出现即采集；到点自动复位）：
  python -u nabu_console.py --tag R --arm-at 240 --deadline 400
  # 抓短窗口（预置命令自动发）：
  python -u nabu_console.py --tag R --script cmds.txt --arm-at 240 --deadline 400
  # 设备已在系统态、显式端口：
  python -u nabu_console.py --port COM3 --tag R --deadline 120
  # 发命令（另一个终端，append 一行）：
  echo hello >> E:/nabu/logs/R-cmd.txt
  echo readpa 0xae01000 40 >> E:/nabu/logs/R-cmd.txt

主机侧命令（同一个句柄，不需要 cdc-console.py）：
  sym <name>                解析内核符号地址（需 --kernel）
  syms <子串>               列出匹配符号（需 --kernel）
  var <name> [字节数]       按符号读内存（需 --kernel，默认 8 字节）
  dump <地址> <长度> <文件>  流式 dump 虚拟地址（自动分块 + CRC 校验）
  dumppa <物理地址> <长度> <文件>  同上，走 READPA
  stats                     打印采集统计
  mark <文字>               在日志里打主机时间锚点
  mute <行前缀> / unmute <行前缀>  日志回显过滤（只影响屏幕，不影响 -com.bin）

KDP（单句柄下的自动握手，不需要另开脚本）：
  echo kdp >> E:/nabu/logs/R-cmd.txt          # 进调试器 + connect
  echo kdp enter 63 >> E:/nabu/logs/R-cmd.txt # 进调试器并预置 'c'（自动 Continuing）
  echo kdppush fa04e4eccafa00000800dec03713fb >> ...   # 预置一个完整 KDP 帧（不进调试器）
  echo kdp enter >> ...                                # 之后再进，帧已在环里
  echo kdp version >> E:/nabu/logs/R-cmd.txt  # 后续任意 kdp_cli 子命令
  echo kdp leave >> E:/nabu/logs/R-cmd.txt    # disconnect，内核从 Debugger() 返回
  echo kdp resume >> E:/nabu/logs/R-cmd.txt   # 发 'c' 救出调试器（Debugger() 屏蔽了 WDT）
控制台会自动：发 DEBUGGER -> 排空日志 -> 关闭句柄 -> 运行 kdp_cli -> 重新打开句柄。
"""
from __future__ import annotations
import argparse, binascii, json, os, queue, re, struct, subprocess, sys, threading, time
from datetime import datetime, timezone, timedelta

try:
    import symtab as _symtab
except ImportError:
    _symtab = None

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("需要 pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(2)

CST = timezone(timedelta(hours=8))
VID, PID = "1209", "316D"
DEV_TIME_RE = re.compile(rb"^nabu: (?:spawn|exec)[^\n]*?\bt=(\d{6,})\b")

# The GPU core clock the job rows' cp/rb tick at. The driver prints the value it
# programmed at boot ("NabuGPU: ... freq=675000000"); 675 MHz is what this board
# comes up with. Only the per-job exec time depends on it -- queue_us stays on
# the 19.2 MHz always-on counter.
GPU_CLOCK_HZ = 675000000


def gpu_clock_hz():
    """The clock RBBM_TIMESTAMP counts at, in Hz.

    Read from this script's environment so a different OPP can be tried without
    editing the file:  NABU_GPU_CLOCK_HZ=587000000 python tools/nabu_console.py
    """
    try:
        hz = int(os.environ.get("NABU_GPU_CLOCK_HZ", "") or 0)
    except ValueError:
        hz = 0
    return hz if hz > 0 else GPU_CLOCK_HZ


def now_hms():
    return datetime.now(CST).strftime("%H:%M:%S")


def crc32(body: bytes) -> int:
    return binascii.crc32(body) & 0xffffffff


def find_port():
    for p in list_ports.comports():
        try:
            if p.vid is not None and p.pid is not None and \
               f"{p.vid:04X}" == VID and f"{p.pid:04X}" == PID:
                return p.device
        except Exception:
            pass
    return None


def open_with_retry(port, raw_path, log, tries=25, gap=0.2):
    """Open the CDC without losing the enumeration race.

    Observed on hardware: list_ports() can report the port a few hundred ms
    before the driver has finished binding, so an immediate open raises
    FileNotFoundError and the whole round is wasted. Retry briefly; only give
    up if the port really never becomes openable.
    """
    last = None
    for i in range(tries):
        try:
            s = Session(port, raw_path, log)
            log(">>> CDC OPEN OK on %s (try %d)" % (port, i + 1))
            return s
        except Exception as e:
            last = e
            if i == 0:
                log(">>> open raced the driver (%s); retrying" % e.__class__.__name__)
            time.sleep(gap)
    log(">>> CDC OPEN FAILED on %s: %s" % (port, last))
    return None


class Session:
    """单句柄：reader 线程落全部字节；SXNU1R 进 reply 队列；解析设备 uptime。"""
    def __init__(self, port, raw_path, log):
        self.s = serial.Serial(port, 115200, timeout=0.05, write_timeout=2.0)
        self.out = open(raw_path, "ab", buffering=0)
        self.replies = queue.Queue()
        self.partial = b""
        self.running = True
        self.alive = True
        self.total = 0
        self.dev_time = None       # 最近设备 uptime（秒）
        self.dev_time_at = 0.0
        self.log = log
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()
        # Writes go through their own thread.
        #
        # serial.write() blocks inside the Windows driver when the kernel stops
        # servicing the CDC (measured: the process became unkillable and held
        # COM3 forever, because a thread parked in that kernel wait cannot be
        # terminated). With a writer thread the main loop keeps running, so the
        # stall watchdog can still fire and close the port cleanly.
        self.wq = queue.Queue()
        self.w = threading.Thread(target=self._writer, daemon=True)
        self.w.start()
        self.write_stuck = False

    def _reader(self):
        while self.running:
            try:
                n = self.s.in_waiting
            except Exception as e:
                self.log("READ ERR %s" % e); self.alive = False; break
            if not n:
                time.sleep(0.02); continue
            try:
                chunk = self.s.read(n)
            except Exception:
                self.alive = False; break
            if not chunk:
                continue
            self.out.write(chunk)
            self.total += len(chunk)
            self.partial += chunk
            while b"\n" in self.partial:
                line, self.partial = self.partial.split(b"\n", 1)
                line = line.strip(b"\r")
                m = DEV_TIME_RE.match(line)
                if m:
                    self.dev_time = int(m.group(1)) / 19200000.0
                    self.dev_time_at = time.time()
                if line.startswith(b"SXNU1R"):
                    self.replies.put(line.decode("utf-8", "replace"))
        self.alive = False

    def uptime(self):
        if self.dev_time is None:
            return None
        return self.dev_time + (time.time() - self.dev_time_at)

    def _writer(self):
        while self.running:
            try:
                payload = self.wq.get(timeout=0.2)
            except queue.Empty:
                continue
            if payload is None:
                break
            try:
                self.s.write(payload)
                self.s.flush()
            except Exception as e:
                # A write failure means the kernel is not reading; flag it and
                # let the main loop decide (the watchdog will close the port).
                self.write_stuck = True
                self.log("WRITE ERR %s" % e)
                break

    def send(self, payload):
        """Enqueue a frame. Never blocks on the device."""
        self.wq.put(payload)

    def wait_reply(self, verb, timeout, seq=None):
        """Wait for the reply that belongs to `verb`, ideally by sequence number.

        Matching on the verb name alone is wrong: the kernel echoes its own
        canonical name from sxnu_control_verb_name(), which need not equal what
        the host typed. Measured on kb-40: `dumplist` was parsed, handled,
        committed (last_seq advanced to 6) and its reply did reach the capture as
        "SXNU1R <session> 00000006 DUMLIST OK ...", but this function was waiting
        for the literal string DUMPLIST and reported a timeout. The kernel side
        was correct the whole time.

        The reply carries session and seq, so prefer those: they cannot be
        ambiguous across aliases. Fall back to the verb comparison only when the
        caller did not supply a seq.
        """
        deadline = time.time() + timeout
        held = []
        want_seq = None
        if seq is not None:
            want_seq = "%08x" % (seq & 0xFFFFFFFF)
        try:
            while time.time() < deadline:
                try:
                    line = self.replies.get(timeout=max(0.01, deadline - time.time()))
                except queue.Empty:
                    break
                parts = line.split()
                if want_seq is not None:
                    rseq = parts[2] if len(parts) > 2 else ""
                    if rseq == want_seq:
                        return line
                else:
                    rverb = parts[3] if len(parts) > 3 else ""
                    if rverb == verb.upper():
                        return line
                held.append(line)
        finally:
            for l in held:
                self.replies.put(l)
        return None

    def close(self):
        """Release the handle for real.

        The reader thread blocks inside in_waiting()/read(), so a bare join can
        time out and leave the OS handle owned by the dying thread -- that is
        what produced the un-killable process holding COM3. cancel_read()
        interrupts the pending read so the thread always exits first.
        """
        self.running = False
        try:
            self.wq.put(None)
        except Exception:
            pass
        writer_stuck = False
        try:
            self.w.join(timeout=2)
            writer_stuck = self.w.is_alive()
        except Exception:
            writer_stuck = True
        try:
            self.s.cancel_read()
        except Exception:
            pass
        try:
            self.t.join(timeout=5)
        except Exception:
            pass
        if self.t.is_alive():
            self.log("! reader thread still alive after close")
        try:
            self.out.close()
        except Exception:
            pass
        if writer_stuck:
            # Closing a port whose writer thread is parked in a kernel wait would
            # block this thread too, re-creating the unkillable-process problem.
            # Leaving it to process exit is the least-bad option: the OS reclaims
            # the handle when the process dies.
            self.log("! writer thread stuck; skipping port close (handle freed on exit)")
            return
        try:
            self.s.close()
        except Exception:
            pass


class Control:
    """SXNU1 状态机。HELLO 固定 seq=0/session=0；其后命令 seq 严格递进。"""
    def __init__(self):
        self.session = 0
        self.seq = 0
        self.hello_done = False

    def build(self, verb, arg=""):
        body = "SXNU1 %016x %08x %s" % (self.session, self.seq, verb)
        if arg:
            body += " " + arg
        return ("%s %08x\n" % (body, crc32(body.encode()))).encode()

    def is_hello(self, verb):
        return verb.upper() == "HELLO"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tag", default="console")
    ap.add_argument("--out", default=r"E:\nabu\logs")
    ap.add_argument("--port", default=None, help="显式端口（跳过等待；设备已在系统态时用）")
    ap.add_argument("--wait", type=int, default=300, help="等 COM3 出现的最长秒数")
    ap.add_argument("--deadline", type=int, default=0, help="总运行时限，0=不限")
    ap.add_argument("--cmd-timeout", type=float, default=10.0)
    ap.add_argument("--stall-exit", type=float, default=20.0,
                    help="设备 N 秒无任何输出就主动退出并释放端口（0=不启用，默认 20）")
    ap.add_argument("--script", default=None, help="预置命令文件：open 后立即紧凑发完")
    ap.add_argument("--arm-at", type=float, default=0, help="设备 uptime 到该秒数自动发 REBOOT（0=不武装）")
    ap.add_argument("--kdp-dir", default=r"E:\nabu\state\bridge\answers\kdp_client",
                    help="kdp_cli.py 所在目录（kdp 命令用）")
    ap.add_argument("--kernel", default="", help="kernel.macho，sym/syms/var 用（可与 symtab.py 同目录）")
    ap.add_argument("--reboot-window", type=float, default=90, help="武装窗口秒数")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    base = os.path.join(args.out, args.tag)
    cmd_path = base + "-cmd.txt"
    ctl_path = base + "-ctl.txt"
    raw_path = base + "-com.bin"
    evt_path = base + "-events.jsonl"
    if not os.path.exists(cmd_path):
        open(cmd_path, "w", encoding="utf-8").close()

    def log(msg):
        line = "%s %s" % (now_hms(), msg)
        print(line, flush=True)
        with open(ctl_path, "a", encoding="utf-8", buffering=1) as f:
            f.write(line + "\n")

    def event(**kw):
        kw["ts"] = now_hms()
        with open(evt_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(kw, ensure_ascii=False) + "\n")

    sess = None
    ctl = None

    def send_cmd(verb, arg, timeout=None):
        """发一条 SXNU1 帧并等 reply，同步 seq/session。返回 reply 或 None。"""
        verb = verb.upper()
        if timeout is None:
            # 慢命令给更长的默认超时：READPA 要经 worker 的 5 ms tick 做真实 MMIO 读取，
            # 64 字节窗口实测偶尔超过 10 s 默认值（kb-38 上连续两次超时后才成功）。
            # READ/DUMP/RDBLK 会走内存或存储，可能卡在缺页或介质访问上，同样放宽。
            slow = verb.startswith(("READPA", "READ", "DUMP", "RDBLK"))
            timeout = args.cmd_timeout * (3.0 if slow else 1.0)
        if ctl.is_hello(verb):
            ctl.session, ctl.seq = 0, 0
        elif not ctl.hello_done:
            log("! skip %s: HELLO 尚未成功" % verb)
            return None
        else:
            ctl.seq += 1

        def attempt(seq_already_bumped):
            if not seq_already_bumped:
                ctl.seq += 1
            frame = ctl.build(verb, arg)
            log("TX %s %s (seq=%d)" % (verb, arg, ctl.seq))
            this_seq = ctl.seq
            sess.send(frame)
            return sess.wait_reply(verb, timeout, seq=this_seq)

        reply = attempt(True)
        if not reply and not ctl.is_hello(verb):
            # One silent failure poisons the whole session, so recover instead of
            # just reporting the timeout.
            #
            # sxnu_control_commit() only advances state->last_seq when a request
            # succeeds, while this side increments ctl.seq for every attempt. One
            # rejected or lost frame therefore leaves the two permanently out of
            # step and every later command is refused -- measured on kb-40: after
            # VERSION (seq=10) got no reply, three further commands failed while
            # the kernel kept answering HELLO, whose commit path ignores
            # last_seq.
            #
            # HELLO re-syncs both session and seq from the kernel's own counter,
            # so replay it and try the command once more.
            log("! retrying %s after HELLO resync" % verb)
            if send_cmd("HELLO", "", timeout) is not None:
                reply = attempt(False)
        if reply:
            log("RX %s" % reply)
            event(kind="cmd", verb=verb, ok=True, reply=reply)
            if ctl.is_hello(verb):
                for t in reply.split():
                    if t.startswith("session="):
                        try: ctl.session = int(t.split("=", 1)[1], 16)
                        except ValueError: pass
                    if t.startswith("last_seq="):
                        try: ctl.seq = int(t.split("=", 1)[1], 16)
                        except ValueError: pass
                ctl.hello_done = True
        else:
            log("! no reply within %.1fs for %s" % (timeout, verb))
            event(kind="cmd", verb=verb, ok=False)
        return reply

    kdp_state = {"entered": False, "port": None}
    syms = {"tab": None, "tried": False}

    def symtab_get():
        """Load the kernel symbol table once, on first use."""
        if syms["tried"]:
            return syms["tab"]
        syms["tried"] = True
        if not args.kernel:
            log("! 无符号表：加 --kernel <kernel.macho>")
            return None
        if _symtab is None:
            log("! 找不到 symtab.py（应与本脚本同目录）")
            return None
        try:
            syms["tab"] = _symtab.Symtab(args.kernel)
            log("符号表已加载：%s (%d 个符号)" % (args.kernel, len(syms["tab"].entries())))
        except Exception as e:
            log("! 符号表加载失败: %s" % e)
            syms["tab"] = None
        return syms["tab"]

    def resolve_addr(name):
        """symtab.lookup returns [(name, value_as_str)]; normalise to int."""
        tab = symtab_get()
        if tab is None:
            return None, "no kernel (add --kernel <path>)"
        hits = tab.lookup(name)
        if not hits:
            return None, "not found: %s" % name
        if len(hits) > 1:
            log("! %d 个匹配，用第一个：%s" % (len(hits), hits[0][0]))
        v = hits[0][1]
        if isinstance(v, int):
            return v, None
        try:
            return int(v, 16), None
        except (TypeError, ValueError):
            return None, "bad symbol value: %r" % (v,)

    def cmd_sym(name):
        if not name:
            log("用法: sym <符号名>，如 sym gPhysBase")
            return
        addr, err = resolve_addr(name)
        log(("0x%016x" % addr) if addr is not None else ("! " + err))

    def cmd_syms(sub):
        tab = symtab_get()
        if tab is None:
            return
        if not sub:
            log("用法: syms <子串>，如 syms native_adreno")
            return
        hits = tab.grep(sub)
        log("%d 个匹配:" % len(hits))
        for n, v in hits[:200]:
            log("  0x%016x  %s" % (v, n) if isinstance(v, int)
                else "  0x%-16s  %s" % (v, n))

    def cmd_var(arg):
        parts = arg.split()
        if not parts:
            log("用法: var <符号名> [字节数，默认 8]")
            return
        addr, err = resolve_addr(parts[0])
        if addr is None:
            log("! " + err)
            return
        nbytes = 8
        if len(parts) > 1:
            try:
                nbytes = int(parts[1], 0)
            except ValueError:
                pass
        nbytes = max(1, min(nbytes, 4096))
        reply = send_cmd("READ", "0x%x %x" % (addr, nbytes))
        if reply:
            log("var %s @0x%016x: %s" % (parts[0], addr, reply))

    # READ's reply is capped at SXNU_CONTROL_REPLY_BYTES (512) and the frame
    # overhead leaves ~192 bytes of data, so anything larger has to be read in
    # chunks. Arrays here run to 320 bytes; silently truncating them made the
    # first batch report "read failed" while the board had answered fine.
    READ_CHUNK = 192

    def read_bytes(addr, nbytes):
        """Read nbytes from a kernel VA in <=READ_CHUNK pieces; None on failure."""
        out = bytearray()
        while len(out) < nbytes:
            want = min(READ_CHUNK, nbytes - len(out))
            reply = send_cmd("READ", "0x%x %x" % (addr + len(out), want))
            if not reply or "state=ready" not in reply:
                return None
            kv = dict(x.split("=", 1) for x in reply.split() if "=" in x)
            raw = kv.get("data")
            if not raw:
                return None
            piece = binascii.unhexlify(raw)
            if not piece:
                return None
            out += piece
            if len(piece) < want:
                break
        return bytes(out) if len(out) >= nbytes else None

    def read_words(addr, nwords):
        d = read_bytes(addr, nwords * 4)
        if d is None:
            return None
        return [int.from_bytes(d[i:i + 4], "little") for i in range(0, len(d) - 3, 4)]

    def read_qwords(addr, n):
        d = read_bytes(addr, n * 8)
        if d is None:
            return None
        return [int.from_bytes(d[i:i + 8], "little") for i in range(0, len(d) - 7, 8)]

    def sym_addr(name):
        a, err = resolve_addr(name)
        if a is None:
            log("! 符号 %s 解析失败: %s" % (name, err))
        return a

    # ---- 方案第 3 组：内存管理（全部读现有数组，零内核改动）----
    # 槽位语义来自驱动源码，见 docs/MEMORY-STATS.md。
    def cmd_vmstat(arg):
        a = sym_addr("_native_adreno_vm_phase_us")
        if a is None:
            return
        rows = read_qwords(a, 16)
        if not rows or len(rows) < 16:
            log("! vmstat 读取失败")
            return
        names = ["calls", "metadata_us", "cache_us", "tables_us",
                 "index_us", None, "max_us", "bytes"]
        for r in (0, 1):
            ph = rows[r * 8:(r + 1) * 8]
            extra = "release_us" if r else "preflight_us"
            parts = []
            for i in range(8):
                parts.append("%s=%d" % (names[i] or extra, ph[i]))
            log("VMSTAT %s %s" % ("unmap" if r else "map", " ".join(parts)))

    def cmd_memstat(arg):
        a = sym_addr("_native_adreno_backing_cache_stats")
        if a is None:
            return
        v = read_qwords(a, 12)
        if not v or len(v) < 12:
            log("! memstat 读取失败")
            return
        log("MEMSTAT backing_cache %s" % " ".join("s%d=%d" % (i, v[i]) for i in range(12)))

    def cmd_poolstat(arg):
        a = sym_addr("_native_adreno_pool_stats")
        if a is None:
            return
        v = read_words(a, 8)
        if not v or len(v) < 8:
            log("! poolstat 读取失败")
            return
        log("POOLSTAT pool_mib=%d pool_bos=%d fallback_bos=%d used_mib=%d "
            "peak_mib=%d scattered_bos=%d scattered_spans=%d imported_spans=%d"
            % tuple(v[:8]))

    def cmd_memstage(arg):
        a = sym_addr("_native_adreno_memory_stage_stats")
        if a is None:
            return
        rows = read_qwords(a, 40)
        if not rows or len(rows) < 40:
            log("! memstage 读取失败")
            return
        stages = ["allocation", "prepare", "spans", "zero", "initial_clean",
                  "set_wc", "reassert_wc", "restore_attributes", "complete",
                  "descriptor_release"]
        printed = 0
        for i, nm in enumerate(stages):
            r = rows[i * 4:(i + 1) * 4]
            if any(r):
                log("MEMSTAGE %-18s a=%d b=%d c=%d d=%d" % (nm, r[0], r[1], r[2], r[3]))
                printed += 1
        if not printed:
            # All zero: say so instead of printing nothing. Silence is
            # indistinguishable from a broken verb.
            log("MEMSTAGE all ten stages are zero (array still at its initial "
                "value on this boot)")

    def cmd_aliasstat(arg):
        a = sym_addr("_nabu_alias_stats")
        if a is None:
            return
        v = read_qwords(a, 6)
        if not v or len(v) < 6:
            log("! aliasstat 读取失败")
            return
        log("ALIASSTAT builds=%d max_hits=%d sum_hits=%d s3=%d s4=%d s5=%d" % tuple(v[:6]))

    # kb-159: the IB verb is gone with native_adreno_ib_log. It was written only
    # by the legacy submit() path, which the compositor never uses, so it always
    # read count=0. SUBMIT reads the replacement on the path that runs.

    # ---- 方案第 4/7 组：其余可零改动读取的数组 ----
    def cmd_methods(arg):
        """METHODS -- the 20 gpu method slots (calls/failures/status/total_us)."""
        a = sym_addr("_native_adreno_method_stats")
        if a is None:
            return
        v = read_words(a, 80)
        if not v or len(v) < 80:
            log("! methods 读取失败")
            return
        printed = 0
        for m in range(20):
            r = v[m * 4:(m + 1) * 4]
            if r[0]:
                log("METHOD %2d calls=%d failures=%d status=%08x total_us=%d"
                    % (m, r[0], r[1], r[2], r[3]))
                printed += 1
        if not printed:
            log("METHODS all 20 slots are zero on this boot")

    def cmd_clients(arg):
        """CLIENTS -- the 5 client slots."""
        a = sym_addr("_native_adreno_client_stats")
        if a is None:
            return
        v = read_words(a, 30)
        if not v or len(v) < 30:
            log("! clients 读取失败")
            return
        printed = 0
        for i in range(5):
            r = v[i * 6:(i + 1) * 6]
            if any(r):
                log("CLIENT slot=%d %s" % (i, " ".join("s%d=%d" % (j, r[j]) for j in range(6))))
                printed += 1
        if not printed:
            log("CLIENTS all 5 slots are zero on this boot")

    def cmd_waitouter(arg):
        """WAITOUTER -- the four external-method wait counters."""
        a = sym_addr("_native_adreno_wait_outer")
        if a is None:
            return
        v = read_qwords(a, 4)
        if not v or len(v) < 4:
            log("! waitouter 读取失败")
            return
        log("WAITOUTER s0=%d s1=%d s2=%d s3=%d" % tuple(v[:4]))

    def cmd_tsens(arg):
        """TSENS -- thermal readings exported by NabuCpuFreq (kb-82 onward)."""
        t = sym_addr("_nabu_cpufreq_tsens_tenths")
        lvl = sym_addr("_nabu_cpufreq_thermal_level")
        sw = sym_addr("_nabu_cpufreq_switches")
        votes = sym_addr("_nabu_cpufreq_vote_khz")
        if t is None:
            return
        tv = read_words(t, 1)
        if not tv:
            log("! tsens 读取失败")
            return
        tenths = tv[0]
        if tenths >= 0x80000000:          # signed
            tenths -= 0x100000000
        log("TSENS hottest=%.1f C (%d tenths)" % (tenths / 10.0, tenths))
        if lvl is not None:
            lv = read_words(lvl, 1)
            if lv:
                log("TSENS thermal_level=%d  (0=normal 1=warm 2=hot)" % lv[0])
        if sw is not None:
            s = read_words(sw, 1)
            if s:
                log("TSENS switches=%d" % s[0])
        if votes is not None:
            v = read_words(votes, 4)
            if v:
                log("TSENS vote l3/small/big/prime=%d/%d/%d/%d kHz"
                    % (v[0], v[1], v[2], v[3]))
        raw = sym_addr("_nabu_cpufreq_tsens_raw")
        if raw is not None:
            r = read_words(raw, 14)
            if r:
                sensors = []
                for i, s in enumerate(r):
                    if s & (1 << 21):
                        t10 = s & 0xfff
                        if t10 & 0x800:
                            t10 -= 0x1000
                        sensors.append("%d:%.1f" % (i + 1, t10 / 10.0))
                    else:
                        sensors.append("%d:-" % (i + 1))
                log("TSENS sensors %s" % " ".join(sensors))

    def cmd_framestat(arg):
        """FRAMESTAT -- the whole frame-pipeline shape from ONE READ (kb-156).

        The latency distribution used to be recovered by pulling every
        NativeAdrenoProfile S/A/R line out of the serial log and doing the
        arithmetic on the host: 8770 rows across hundreds of round trips, each
        capped at ~192 bytes of data. That is slow enough to distort what it
        measures, and it is how a three-second window got mistaken for the run's
        p90 (kb-152: reported 15.8 ms, real 284 ms).

        The kernel now keeps an always-on histogram (native_adreno_frame_hist,
        26 words = 104 bytes) so the entire shape arrives in one reply: bucket
        masses, min/max, the GPU-clock share, and a pipeline-overlap count.

        Two readings are useful:
          * mass in b0..b4 (< 40 ms)   -> frames keep up
          * mass in b7+ (>= 100 ms)    -> frames queue or the GPU is slow
          * gpu_us/wall_us near 1.0    -> the GPU really ran for that time
            gpu_us/wall_us near 0.0    -> it was waiting (queueing / gating)
        """
        a = sym_addr("_native_adreno_frame_hist")
        if a is None:
            log("! framestat: kernel has no native_adreno_frame_hist "
                "(needs the kb-156+ export)")
            return
        v = read_words(a, 26)
        if not v or len(v) < 26:
            log("! framestat 读取失败")
            return
        edges = ["<5ms", "<10ms", "<20ms", "<30ms", "<40ms", "<60ms",
                 "<80ms", "<100ms", "<150ms", "<200ms", "<300ms", ">=300ms"]
        total = v[1]
        log("FRAMESTAT n=%d issued=%d min=%dus max=%dus last=%dus"
            % (total, v[16], v[2], v[3], v[23]))
        if not total:
            log("FRAMESTAT no completions since boot")
        else:
            parts = []
            for i, name in enumerate(edges):
                c = v[4 + i]
                if c:
                    parts.append("%s=%d(%.1f%%)" % (name, c, 100.0 * c / total))
            log("FRAMESTAT hist %s" % " ".join(parts))
        if v[18] and v[20]:
            log("FRAMESTAT last gpu_us=%d wall_us=%d ratio=%.3f"
                % (v[19], v[20], float(v[19]) / float(v[20])))
        elif v[18] == 0:
            log("FRAMESTAT gpu_us=none (GMU AO counter unreadable from this path)")
        # kb-157: the running totals are 64-bit globals beside the array, because
        # the 32-bit h[19]/h[20] wrap after 4.29e9 us (71 minutes of summed wall
        # time).
        #
        # What the ratio actually is, and is not: gpu_us comes from
        # frameHistAlwaysOn(), the GMU always-on counter. That is a free-running
        # 19.2 MHz clock, not a busy/idle signal, so it advances by exactly the
        # wall time between the submit and the completion being noticed. The
        # ratio therefore sits at 1.0 by construction -- it cannot report that
        # the GPU waited, and the old "GPU mostly EXECUTING/WAITING" verdict read
        # into it was wrong. It still has one use: a ratio well below 1.0 means
        # the two samples straddled an AO counter reset, i.e. the number is junk.
        # Real GPU execution time comes from GPUSTATE's per-job exec_ms.
        gt = sym_addr("_native_adreno_frame_gpu_us_total")
        wt = sym_addr("_native_adreno_frame_wall_us_total")
        gs = sym_addr("_native_adreno_frame_gpu_samples")
        if gt is not None and wt is not None and gs is not None:
            tot = read_qwords(gt, 2)
            cnt = read_words(gs, 1)
            if tot and cnt:
                gpu_total, wall_total = tot[0], tot[1]
                log("FRAMESTAT total ao_us=%d wall_us=%d ao_ratio=%.4f samples=%d"
                    % (gpu_total, wall_total,
                       (float(gpu_total) / float(wall_total)) if wall_total else 0.0,
                       cnt[0]))
                if gpu_total and wall_total and gpu_total * 100 < wall_total * 90:
                    log("FRAMESTAT note: ao_ratio < 0.9 -- the always-on counter "
                        "was reset mid-run, ignore it")
        else:
            log("FRAMESTAT total: kernel has no kb-157 64-bit totals")
        log("FRAMESTAT overlap=%d (completions that shared the pipeline) last_gpu=%dus"
            % (v[22], v[24]))

    def cmd_gpustate(arg):
        """GPUSTATE -- the GPU's own counters and the newest job ticks (kb-158).

        Two questions this answers, both from one or two READs:

        1. What is the GPU doing during a slow frame? FRAMESTAT can already say a
           frame took 160 ms with the GPU clock running for 160 ms. It cannot say
           whether the shaders were computing or waiting. These counters can:

               sp_busy / sp_alu            shaders executing  -> real work
               sp_stall_uche/_rb/_tp       shaders waiting    -> starved
               uche_vbif_latency_cycles    memory read latency
               rb_busy / rb_stall_ccu      raster state
               cp_busy / cp_wfi_stall      command processor

           When sp_stall_* dominates, the GPU is busy waiting on memory and the
           fix is on the memory path. When sp_busy dominates, the work itself is
           the problem.

        2. Per-fence clock domains. A job row carries issue, cp and rb, and they
           do NOT share a time base. `issue` is sampled host-side from the GMU
           always-on counter at 19.2 MHz. `cp` and `rb` are written by the GPU:
           cp by CP_REG_TO_MEM (0x3e) and rb by CP_EVENT_WRITE7 (0x46) with bit
           30 set, both of which read RBBM_TIMESTAMP, i.e. the GPU core clock
           (675 MHz on this board, printed by the driver at boot as
           "freq=675000000"). Converting all three at 19.2 MHz inflated rb-cp by
           675/19.2 = 35.2x: a real 4.18 ms read as 147 ms, which is where the
           "GPU runs 147 ms per frame" reading came from. queue_us is a genuine
           19.2 MHz figure; exec_us is converted with the GPU clock.
        """
        names = ["core_cycles", "sp_busy", "sp_alu", "vs_alu", "fs_alu",
                 "fs_tex", "vs_tex", "sp_stall_tp", "sp_stall_uche", "sp_stall_rb",
                 "tp_busy", "tp_stall_uche", "uche_busy", "uche_stall_arb",
                 "uche_read_tp", "uche_read_sp", "uche_write_sp", "uche_gmem_read",
                 "uche_vbif_lat", "uche_vbif_n", "rb_busy", "rb_stall_ccu",
                 "ccu_gmem_rd", "ccu_gmem_wr", "cp_busy", "cp_wfi_stall",
                 "cp_sync_stall", "fs_stage", "vs_stage"]

        a = sym_addr("_native_adreno_gpu_counter_now")
        seq = sym_addr("_native_adreno_gpu_counter_seq")
        if a is None or seq is None:
            log("! gpustate: kernel has no kb-158 counter export")
            return
        v = read_qwords(a, 29)
        sq = read_words(seq, 1)
        if not v or not sq:
            log("! gpustate 读取失败")
            return
        log("GPUSTATE samples=%d (continuous capture)" % sq[0])
        if sq[0] == 0:
            log("GPUSTATE no sample yet")
        else:
            # The exported words are raw register readings, not deltas, so a
            # single read says nothing about rate. Take two samples one recorder
            # period apart and difference them: that is what the counters mean.
            import time as _t
            _t.sleep(1.0)
            v2 = read_qwords(a, 29)
            sq2 = read_words(seq, 1)
            if not v2 or not sq2:
                log("! gpustate 第二次读数失败")
                return
            d = [max(0, v2[i] - v[i]) for i in range(min(len(v), len(v2)))]
            log("GPUSTATE delta over samples %d -> %d" % (sq[0], sq2[0]))
            for i, name in enumerate(names):
                if i < len(d) and (d[i] or i < 12):
                    log("GPUSTATE %-16s %d" % (name, d[i]))
            # The numbers that answer the question: what was the command
            # processor doing, and were the shaders computing or waiting.
            cp_busy = d[24]
            cp_wfi = d[25]
            cp_sync = d[26]
            cp_total = cp_busy + cp_wfi + cp_sync
            if cp_total:
                log("GPUSTATE cp: busy=%d(%.1f%%) wfi_stall=%d(%.1f%%) sync_stall=%d(%.1f%%)"
                    % (cp_busy, 100.0 * cp_busy / cp_total,
                       cp_wfi, 100.0 * cp_wfi / cp_total,
                       cp_sync, 100.0 * cp_sync / cp_total))
                if cp_wfi + cp_sync > cp_busy:
                    log("GPUSTATE verdict: CP is mostly STALLED "
                        "(waiting on work/sync, not executing)")
                else:
                    log("GPUSTATE verdict: CP is mostly BUSY (executing commands)")
            busy = d[1]
            stall = d[7] + d[8] + d[9]
            sp_total = busy + stall
            if sp_total:
                log("GPUSTATE sp: busy=%d(%.1f%%) stall_tp=%d stall_uche=%d stall_rb=%d"
                    % (busy, 100.0 * busy / sp_total, d[7], d[8], d[9]))
            else:
                log("GPUSTATE sp: all zero (shader counters not accumulating on this "
                    "configuration; cp_* and rb_busy still valid)")
            if d[20]:
                log("GPUSTATE rb_busy=%d rb_stall_ccu=%d" % (d[20], d[21]))

        ja = sym_addr("_native_adreno_gpu_job_now")
        js = sym_addr("_native_adreno_gpu_job_seq")
        if ja is not None and js is not None:
            jn = read_words(js, 1)
            if jn and jn[0]:
                log("GPUSTATE jobs=%d (newest 8; queue_us at 19.2MHz, exec_us at GPU clock)"
                    % jn[0])
                seen = min(8, jn[0])
                for k in range(seen):
                    row = read_words(ja + ((jn[0] - seen + k) & 7) * 6 * 4, 6)
                    if not row or len(row) < 6:
                        continue
                    fence, queue, issue, cp, rb, valid = row[:6]
                    qticks = (cp - issue) & 0xffffffff if (valid & 1) else 0
                    rticks = (rb - cp) & 0xffffffff if (valid & 2) else 0
                    # issue is the GMU always-on counter (19.2 MHz); cp/rb come
                    # from RBBM_TIMESTAMP, so they tick at the GPU core clock.
                    log("GPUSTATE job fence=%-5d queue=%-4d queue_us=%-8d "
                        "exec_us=%-8d exec_ms=%.3f valid=%d"
                        % (fence, queue, qticks * 625 // 12000,
                           rticks * 625 // 12000,
                           rticks / float(gpu_clock_hz()) * 1e3, valid))

    def cmd_ibclass(arg):
        """IBCLASS -- TYPE7 opcode histogram of the largest IB (kb-162).

        The latency model from kb-159/161 is lat = 206 ms + 0.409 ms/KiB of
        command stream, i.e. 67 GPU clocks per dword. Every other candidate is
        measured and excluded, so the cost is inside the stream. This says which
        packets fill it.

        Reading it:
          * one opcode dominating -> that packet is the expense
          * flat, full of register writes (TYPE4 / CP_SET_BIN / CP_REG_RMW)
            -> the stream is redundant, fix it upstream of the GPU driver
        """
        cls = sym_addr("_native_adreno_ib_class")
        top = sym_addr("_native_adreno_ib_class_top")
        scanned = sym_addr("_native_adreno_ib_scanned")
        order = sym_addr("_native_adreno_ib_order")
        if cls is None or top is None:
            log("! ibclass: kernel has no kb-162 export")
            return
        c = read_words(cls, 32)
        t = read_words(top, 8)
        sc = read_words(scanned, 1)
        od = read_words(order, 8)
        if not c or not t or not sc:
            log("! ibclass 读取失败")
            return
        log("IBCLASS scanned_dwords=%d" % sc[0])
        if not sc[0]:
            log("IBCLASS nothing scanned yet")
            return
        names = {0x46: "CP_EVENT_WRITE7", 0x26: "CP_WAIT_FOR_IDLE",
                 0x14: "CP_WAIT_TIMESTAMP", 0x17: "CP_THREAD_CONTROL",
                 0x65: "CP_SET_MARKER", 0x3e: "CP_REG_TO_MEM",
                 0x3f: "CP_INDIRECT_BUFFER", 0x32: "CACHE_FLUSH7",
                 0x33: "CACHE_INVALIDATE7", 0x16: "RB_DONE_TS",
                 0x1d: "CP_SET_DRAW_STATE", 0x2c: "CP_DRAW_INDX_OFFSET",
                 0x2f: "CP_SET_BIN_DATA5", 0x36: "CP_SET_BIN",
                 0x38: "CP_DRAW_INDIRECT", 0x21: "CP_SET_VISIBILITY_OVERRIDE",
                 0x1f: "CP_RESET_CONTEXT_STATE", 0x0e: "CP_WAIT_REG_MEM"}
        total = sum(c)
        if total:
            log("IBCLASS total_packets=%d  (buckets are opcode & 31)" % total)
        for i in range(8):
            v = t[i]
            if not v:
                continue
            cnt = v >> 8
            op = v & 0xff
            log("IBCLASS top%d: op=0x%03x %-24s count=%d (%.1f%%)"
                % (i + 1, op, names.get(op, "?"), cnt,
                   100.0 * cnt / total if total else 0.0))
        log("IBCLASS stream order: %s"
            % " ".join("0x%03x(%s)" % (x, names.get(x, "?")) for x in od if x))

    def cmd_submit(arg):
        """SUBMIT -- what one compositor submission asks the GPU to do (kb-159).

        Replaces JOBSTAT, whose mirror was written only from the publish path and
        therefore froze at boot traffic (count=6 on board).

        Why this is the next measurement: FRAMESTAT established that frames are
        not queued (queue 2 us) and not gated (GPU clock runs 99.99% of wall
        time), so the GPU is executing for the whole 149-340 ms. The remaining
        question is what it executes. A single full-screen render at 675 MHz is
        ~0.38 ms of ROP work, so the board is doing 400x that somewhere.

          ibs        indirect buffers referenced -- how many separate command
                     blocks this submission chains
          packages   TYPE7 command headers assembled into the ring -- how many
                     GPU commands in total
          resources  memory objects the submission declares
          ib_bytes   total bytes of indirect buffer it points at

        Many small packages => the compositor issues a long command stream.
        Few large ones => each command covers far more than the visible frame.
        The first IB's leading dwords name the command stream itself.
        """
        a = sym_addr("_native_adreno_submit_observe")
        if a is None:
            log("! submit: kernel has no native_adreno_submit_observe "
                "(needs the kb-159+ export)")
            return
        v = read_words(a, 24)
        if not v or len(v) < 24:
            log("! submit 读取失败")
            return
        n = v[0]
        log("SUBMIT observed=%d   last: ibs=%d packages=%d resources=%d ib_bytes=%d"
            % (n, v[1], v[10], v[2], v[3]))
        if not n:
            log("SUBMIT no submission since boot")
            return
        log("SUBMIT max:    ibs=%d packages=%d resources=%d ib_bytes=%d"
            % (v[4], v[11], v[5], v[6]))
        if n:
            log("SUBMIT mean:   ibs=%.2f resources=%.2f ib_bytes=%.0f"
                % (float(v[7]) / n, float(v[8]) / n, float(v[9]) / n))
        head = " ".join("%08x" % v[16 + i] for i in range(8))
        log("SUBMIT first IB head: %s" % head)

        # kb-160: decode the first four IBs' leading packets. A CP_WAIT_FOR_IDLE
        # at the head of each IB would drain the pipeline per IB and serialise a
        # 32..58 IB submission -- the candidate for the 5.96 ms/IB slope.
        harr = sym_addr("_native_adreno_submit_ib_head")
        hcnt = sym_addr("_native_adreno_submit_ib_head_count")
        if harr is not None and hcnt is not None:
            hc = read_words(hcnt, 1)
            if hc and hc[0]:
                pkt_names = {0x46: "CP_EVENT_WRITE7", 0x26: "CP_WAIT_FOR_IDLE",
                             0x14: "CP_WAIT_TIMESTAMP", 0x17: "CP_THREAD_CONTROL",
                             0x65: "CP_SET_MARKER", 0x3e: "CP_REG_TO_MEM",
                             0x3f: "CP_INDIRECT_BUFFER", 0x32: "CACHE_FLUSH7",
                             0x33: "CACHE_INVALIDATE7", 0x16: "RB_DONE_TS",
                             0x36: "CP_SET_BIN", 0x2c: "CP_DRAW_INDX_OFFSET",
                             0x2f: "CP_SET_BIN_DATA5", 0x1d: "CP_SET_DRAW_STATE",
                             0x38: "CP_DRAW_INDIRECT"}
                waits = 0
                iovas = v[12:16] if len(v) >= 16 else [0, 0, 0, 0]
                for k in range(min(4, hc[0])):
                    row = read_words(harr + k * 9 * 4, 9)
                    if row and len(row) >= 9:
                        log("SUBMIT ib%d iova=%08x bytes=%d%s"
                            % (k, iovas[k], row[8],
                               "  <-- SAME IOVA AS PREVIOUS" if k and iovas[k] == iovas[k-1] else ""))
                        row = row[:8]
                    if not row or len(row) < 8:
                        continue
                    toks = []
                    i = 0
                    while i < len(row):
                        w = row[i]
                        typ = (w >> 28) & 0xf
                        if typ == 7:
                            op = (w >> 16) & 0xfff
                            cnt = w & 0x3fff
                            toks.append(pkt_names.get(op, "0x%03x" % op))
                            if op == 0x26:
                                waits += 1
                            i += 1 + cnt
                        elif typ == 4:
                            toks.append("REG%05x=%08x" % ((w >> 8) & 0xffff,
                                                          row[i + 1] if i + 1 < len(row) else 0))
                            i += 2
                        else:
                            i += 1
                    log("SUBMIT ib%d (%s): %s"
                        % (k, " ".join("%08x" % x for x in row[:4]),
                           " | ".join(toks[:5])))
                log("SUBMIT IBs with CP_WAIT_FOR_IDLE in head: %d of %d"
                    % (waits, min(4, hc[0])))
        # Name the first package if it matches a known PM4 shape.
        if v[16]:
            opcode = v[16] & 0xff
            typ = (v[16] >> 30) & 3
            log("SUBMIT first dword: type=%d opcode=0x%02x" % (typ, opcode))

    def cmd_slow(arg):
        """SLOW -- memory operations that took >= 8000 us (kb-95 onward)."""
        tot = sym_addr("_nabu_console_diag_totals")
        flat = sym_addr("_nabu_console_slow_flat")
        cnt = sym_addr("_nabu_console_slow_count")
        if tot is None or flat is None or cnt is None:
            log("! slow: kernel has no flat diag arrays (needs the kb-95+ export)")
            return
        tv = read_qwords(tot, 8)
        if not tv:
            log("! slow 读取失败（totals）")
            return
        log("SLOW insert_attempts=%d insertions=%d cached_obj=%d cached_bytes=%d"
            % (tv[0], tv[1], tv[2], tv[3]))
        log("SLOW peak_obj=%d peak_bytes=%d slow_total=%d slow_kept=%d"
            % (tv[4], tv[5], tv[6], tv[7]))

        # Why insertions were refused. insert_attempts - insertions says how many,
        # this says why; without it a miss looks identical whether the object was
        # Oversized, Imported, or lost to memory pressure.
        rej = sym_addr("_nabu_console_reject")
        if rej is not None:
            # 104 bytes is 13 qwords; the reply budget is 192 bytes of data, so
            # this fits, but the first read after a HELLO resync can still come
            # back short. Retry once before believing a zero.
            r = read_qwords(rej, 13)
            if not r or not any(r):
                r2 = read_qwords(rej, 13)
                if r2 and any(r2):
                    r = r2
            if r and any(r):
                names = ["not_owned", "not_prepared", "not_write_combined",
                         "quarantined", "no_spans", "zero_size", "OVERSIZED",
                         "no_evictable", "no_slot", "imported", "pooled",
                         "pressure", "busy"]
                parts = ["%s=%d" % (names[i], r[i])
                         for i in range(min(len(names), len(r))) if r[i]]
                log("SLOW reject %s" % " ".join(parts))
            else:
                log("SLOW reject all zero (nothing has been refused yet)")
        ev = sym_addr("_nabu_console_evict")
        if ev is not None:
            e = read_qwords(ev, 5)
            if e and any(e):
                en = ["object_limit", "byte_limit", "client_close",
                      "memory_pressure", "allocation_retry"]
                log("SLOW evict %s" % " ".join(
                    "%s=%d" % (en[i], e[i]) for i in range(min(len(en), len(e))) if e[i]))

        # kb-103: "busy" alone spans five different causes. This says which.
        bd = sym_addr("_nabu_console_busy_detail")
        if bd is not None:
            b = read_qwords(bd, 16)
            if b and any(b):
                hist = ["refs%d=%d" % (i, b[i]) for i in range(8) if b[i]]
                total = b[13]
                mean = (b[14] / total) if total else 0.0
                log("BUSY total=%d hist[%s] imported=%d pooled=%d no_spans=%d "
                    "single=%d oversat=%d mean=%.1f max=%d"
                    % (total, " ".join(hist) if hist else "empty",
                       b[8], b[9], b[10], b[11], b[12], mean, b[15]))
                if total and b[11] * 10 >= total * 9:
                    log("BUSY verdict: every refusal held exactly one reference -- "
                        "one holdForDevice() was never retired")
                elif b[15] > 8:
                    log("BUSY verdict: references accumulate (>8 on one object) -- "
                        "holdJob() ran more often than retireJobAfterCompletion()")
                elif b[10]:
                    log("BUSY verdict: objects with no spans were held -- "
                        "check the early map() path")

        cv = read_words(cnt, 1)
        have = cv[0] if cv else 0
        show = have if not arg.strip() else min(have, int(arg.strip(), 0))
        if show == 0:
            log("SLOW no operation exceeded 8000 us since boot")
            return
        # NativeAdrenoMemorySlowOperation has exactly four values
        # (NativeAdrenoMemory.h:57-62). The ten-name list that used to be here
        # belongs to MemoryStage, a different enum, and it made every
        # OwnedDescriptorRelease print as "prepare" -- which is how a 13 ms
        # descriptor release got mistaken for allocation work.
        names = ["allocation", "owned_descriptor_release",
                 "imported_descriptor_release", "import"]
        for i in range(show):
            row = read_words(flat + i * 12 * 4, 12)
            if not row or len(row) < 12:
                log("! slow row %d 读取失败" % i)
                return
            seq = row[0] | (row[1] << 32)
            elapsed = row[2] | (row[3] << 32)
            nbytes = row[4] | (row[5] << 32)
            op = row[7]
            opname = names[op] if op < len(names) else ("op%d" % op)
            log("SLOW seq=%d %s elapsed_us=%d bytes=%d spans=%d "
                "source=%d cache_result=%d flags=%08x avail_pages=%d"
                % (seq, opname, elapsed, nbytes, row[6], row[8], row[9],
                   row[10], row[11]))

    def cmd_cpstat(arg):
        """CPSTAT -- CP/ring status, mostly already in GMUSTATE."""
        a = sym_addr("_native_adreno_a6xx_stats")
        if a is None:
            log("! cpstat: _native_adreno_a6xx_stats not found")
            return
        v = read_words(a, 24)
        if not v or len(v) < 24:
            log("! cpstat 读取失败")
            return
        log("CPSTAT step=%d failed=%d ctx_step=%d ctx_prog=%d "
            "fence=%08x completed=%08x chip=%08x"
            % (v[0], v[1], v[6], v[7], v[8], v[9], v[10]))
        log("CPSTAT cp_status=%08x protect_status=%08x "
            "hfi_q=%d hfi_hdr=%08x hfi_msg=%08x hfi_err=%08x hfi_ack=%d"
            % (v[12], v[13], v[2], v[3], v[14], v[15], v[19]))
        log("CPSTAT zap phase=%d transport=%d result=%d stopped=%d"
            % (v[16], v[17], v[18], v[20]))

    def cmd_irqstat(arg):
        """IRQSTAT -- interrupt counters the kernel exports as data.

        nabu_irq_count() and nabu_ipi_count() are functions, so READ cannot
        reach them; their running values already appear in every
        `nabu: spawn` line as irq=N ipi=M. What IS reachable through READ is
        nabu_rtclock_intr_count, a plain counter.
        """
        rt = sym_addr("_nabu_rtclock_intr_count")
        if rt is None:
            return
        v = read_qwords(rt, 1)
        if not v:
            log("! irqstat 读取失败")
            return
        log("IRQSTAT rtclock_intr=%d" % v[0])
        try:
            import re as _re
            last = None
            with open(ctl_path, "r", encoding="utf-8", errors="replace") as f:
                for line in f.readlines()[-400:]:
                    m = _re.search(r"irq=(\d+)", line)
                    if m:
                        last = m.group(1)
            if last:
                log("IRQSTAT irq_total=%s (newest nabu: spawn line)" % last)
        except Exception:
            pass

    def cmd_dump(arg):
        """dump <addr> <len> <file> | dumppa <pa> <len> <file>"""
        parts = arg.split()
        if len(parts) != 4:
            log("用法: dump|dumppa <地址> <长度> <输出文件>")
            return
        which, addr_s, len_s, outpath = parts
        phys = (which.lower() == "dumppa")
        verb = "READPA" if phys else "READ"
        nxt = "READPANEXT" if phys else "READNEXT"
        try:
            start_addr = int(addr_s, 0)
            total = int(len_s, 0)
        except ValueError:
            log("! 地址/长度必须是整数（可 0x 前缀）")
            return
        if total <= 0 or total > 64 * 1024 * 1024:
            log("! 长度须在 1..64MiB（更大请用内核侧 DUMPUFS）")
            return
        reply = send_cmd(verb, "0x%x %x" % (start_addr, total), timeout=20.0)
        if not reply or "state=ready" not in reply:
            log("! dump 启动失败: %s" % (reply or "无回复"))
            return
        kv = dict(x.split("=", 1) for x in reply.split() if "=" in x)
        want_crc = int(kv.get("crc32", "0"), 16)
        data = bytearray()
        if kv.get("data"):
            data += binascii.unhexlify(kv["data"])
        chunks = 1
        while len(data) < total:
            more = send_cmd(nxt, "80", timeout=20.0)
            if not more or "state=data" not in more:
                log("! dump 中断: %s" % (more or "无回复"))
                return
            mk = dict(x.split("=", 1) for x in more.split() if "=" in x)
            data += binascii.unhexlify(mk.get("data", ""))
            chunks += 1
        if len(data) != total:
            log("! dump 长度不符: %d/%d" % (len(data), total))
            return
        got_crc = binascii.crc32(bytes(data)) & 0xffffffff
        if want_crc and got_crc != want_crc:
            log("! CRC 不符 want=%08x got=%08x（文件不写）" % (want_crc, got_crc))
            return
        with open(outpath, "wb") as f:
            f.write(data)
        log("DUMP OK %s: %d B / %d chunks crc=%08x" % (outpath, len(data), chunks, got_crc))

    def cmd_ptdump(arg):
        """ptdump <va> [<va> ...]  -- walk stage-1 page tables for each VA.

        The kernel answers with the translation it derived from TTBR1_EL1 plus a
        16-byte probe read through that PA, so a single command proves the walk:
        'ptdump <va>' then 'readpa <pa> 16' must return identical bytes. Accepts
        a symbol name as well, because typing 0xfffffff007004000 by hand is how
        you end up probing the wrong address. Every token is resolved
        independently so one bad name does not abort the rest."""
        for token in arg.split():
            if token.lower().startswith("0x") or (token and token[0].isdigit()):
                try:
                    addr = int(token, 16)
                except ValueError:
                    log("! ptdump: 非法地址 %r" % token)
                    continue
            else:
                addr, err = resolve_addr(token)
                if addr is None:
                    log("! ptdump: " + err)
                    continue
            reply = send_cmd("PTDUMP", "0x%x" % addr)
            if reply:
                log("PTDUMP %s -> %s" % (token, reply))

    def cmd_stackshot(arg):
        """stackshot  -- capture every thread's stack into the kernel buffer.

        Fills kernel_stackshot_buf (STACKSHOT_SAVE_IN_KERNEL_BUFFER) and reports
        buf/size. The call refuses to overwrite an existing snapshot, so a second
        run reports state=present with the old buffer still valid."""
        reply = send_cmd("STACKSHOT", "", timeout=60.0)
        if not reply:
            log("! stackshot: 无回复（抓栈可能耗时，内核可能仍在跑）")
            return
        log("STACKSHOT -> %s" % reply)
        kv = dict(x.split("=", 1) for x in reply.split() if "=" in x)
        buf = kv.get("buf", "")
        size = kv.get("size", "")
        if buf and size and size != "0":
            log("  快照在 0x%s，%s 字节。读回：dump 0x%s %s out.stackshot"
                % (buf, size, buf, size))
            log("  （或 res/UFS 导出；注意 %s 字节走 CDC 约需 %.0f 秒）"
                % (size, int(size) / 62000.0 if size.isdigit() else 0))

    def cmd_panictrig(arg):
        """panictrig  -- panic on purpose, then read the panic tail after reboot.

        Requires BOTH the nabu-panic-trig=1 boot-arg and the literal key
        PANIC-NOW, so it cannot fire from a typo. The kernel answers OK first and
        then panics; expect the CDC link to die and the board to come back in
        recovery."""
        if arg.strip() != "PANIC-NOW":
            log("用法: panictrig   (会立即让内核 panic，且必须先在 boot-args 加 nabu-panic-trig=1)")
            log("     安全阀：必须原样带 PANIC-NOW，否则内核回 ERR state=args")
            return
        log(">>> 即将触发 panic：内核会立即死，CDC 会掉。这是预期行为。")
        reply = send_cmd("PANICTRIG", "PANIC-NOW", timeout=15.0)
        log("PANICTRIG -> %s" % (reply or "无回复（内核可能在回复前就死了，也是预期）"))
        log(">>> 现在等设备回 recovery，然后启动新内核，用 readufs 8166..8190 读 panic 槽")

    def cmd_readufs(arg):
        """readufs <relblock> [<relblock> ...]  -- read the diagnostic log window.

        Reads one 4 KiB block per offset straight off the medium and decodes the
        NABULOG1 header, so a landing can be verified without trusting any kernel
        counter. rel is a block offset from the start of the log window, not an
        absolute LBA."""
        words = arg.split()
        if not words:
            log("用法: readufs <relblock> [<relblock> ...]   (块偏移，0 = 日志窗开头)")
            return
        for w in words:
            inner_off = 0
            if "@" in w:
                w, off_s = w.split("@", 1)
                try:
                    inner_off = int(off_s, 0)
                except ValueError:
                    log("! readufs: 非法块内偏移 %r" % off_s)
                    continue
            # Named offsets, so nobody has to remember the slot numbers.
            named = {"panic": 8190, "panicheader": 8190,
                     "panictail": 8166, "header": 0, "tail": 1}
            if w.lower() in named:
                rel = named[w.lower()]
            else:
                try:
                    rel = int(w, 0)
                except ValueError:
                    log("! readufs: 非法偏移 %r（可用 header/tail/panic/panictail）" % w)
                    continue
            reply = send_cmd("READUFS", "0x%x 0x%x" % (rel, inner_off), timeout=30.0)
            if not reply:
                log("! readufs %d: 无回复" % rel)
                continue
            kv = dict(x.split("=", 1) for x in reply.split() if "=" in x)
            data = kv.get("data", "")
            if not data:
                log("READUFS rel=%d -> %s" % (rel, reply))
                continue
            blob = binascii.unhexlify(data)
            magic = blob[0:8]
            if magic == b"NABULOG1":
                # Header layout from serial_general.c:412-417 -- seven u32s at +8,
                # with total split into lo/hi because it is 64-bit.
                ver, is_panic, avail, dropped, t_lo, t_hi = struct.unpack_from("<IIIIII", blob, 8)
                total = t_lo | (t_hi << 32)
                log("READUFS rel=%d  crc32=%s  NABULOG1 ver=%d is_panic=%d available=%d total=%d dropped=%d"
                    % (rel, kv.get("crc32", "?"), ver, is_panic, avail, total, dropped))
            else:
                log("READUFS rel=%d  crc32=%s  magic=%r  head=%s"
                    % (rel, kv.get("crc32", "?"), magic, data[:48]))
                if len(blob) >= 24:
                    log("        words: %s" % " ".join("%08x" % v for v in struct.unpack_from("<IIIIII", blob, 0)))

    def cmd_panicdump(arg):
        """panicdump [info | log <off> <len>]  -- read the embedded panic region.

        'info' (the default) reports the header and peeks 16 bytes of the log;
        'log' streams a slice of the log text. Never writes: the region is left
        exactly as found so iBoot/astris can still extract it."""
        words = arg.split()
        if not words:
            reply = send_cmd("PANICDUMP", "")
        elif words[0].lower() == "info":
            reply = send_cmd("PANICDUMP", "info")
        elif words[0].lower() == "log" and len(words) == 3:
            try:
                off = int(words[1], 0)
                n = int(words[2], 0)
            except ValueError:
                log("用法: panicdump log <offset> <len>")
                return
            reply = send_cmd("PANICDUMP", "log %x %x" % (off, n))
        else:
            log("用法: panicdump [info | log <offset> <len>]")
            return
        log("PANICDUMP -> %s" % (reply or "(no reply)"))

    def cmd_kdpraw(hexstr):
        """Write raw bytes straight to the serial port, bypassing SXNU1.

        This is the live path into KDP once the kernel is in the debugger: the
        control channel stops answering there, so KDPPUSH (an SXNU1 verb) cannot
        be used. kb-32 makes the module feed wire RX into kdp_rx while armed, so
        a frame written here is picked up by kdp_poll().

        A KDP serial frame is self-delimiting (0xFA ... 0xFB), so unlike
        kdppush there is no need to chunk: write the whole thing at once.
        """
        hexstr = "".join(hexstr.split())
        if not hexstr or len(hexstr) % 2:
            log("用法: kdpraw <hex>（偶数个 hex 字符）")
            return False
        try:
            raw = bytes.fromhex(hexstr)
        except ValueError:
            log("! kdpraw: 非法 hex")
            return False
        sess.send(raw)
        log("kdpraw TX %d B: %s%s" % (len(raw), raw[:16].hex(),
                                      "..." if len(raw) > 16 else ""))
        return True

    # ---- live KDP session helpers -------------------------------------------
    # Once the kernel is in the debugger the SXNU1 control channel stops
    # answering, so these build KDP packets and hand them to cmd_kdpraw. The
    # kernel side is unchanged: kdp_writemem64() has always been there, it just
    # had no way to be driven from this console.
    kdp_live = {"seq": 1, "key": 0x1337C0DE}

    def kdp_send_packet(pkt, label):
        """Frame one KDP packet, write it, and report what went out."""
        try:
            import kdp_frame as kf
        except ImportError:
            log("! kdp_frame.py 不在 tools/ 下")
            return False
        fr = kf.serial_frame(kf.net_packet(pkt))
        sess.send(fr)
        log("kdplive TX %s req=0x%02x seq=%d len=%d frame=%d B"
            % (label, pkt[0], pkt[1], int.from_bytes(pkt[2:4], "little"), len(fr)))
        return True

    def cmd_kdpwrite(args):
        """kdpwrite <hexva> <hexbytes>  -- KDP_WRITEMEM64 while in the debugger.

        Needs the kernel to be inside kdp_handler already (see `kdp enter`), and
        the sequence number must match kdp's exception_seq exactly, which is why
        kdp_live["seq"] is threaded through every packet.
        """
        w = args.split()
        if len(w) != 2:
            log("用法: kdpwrite <hexva> <hexbytes>")
            return False
        try:
            import kdp_frame as kf
        except ImportError:
            log("! kdp_frame.py 不在 tools/ 下")
            return False
        try:
            addr = int(w[0], 16)
            data = bytes.fromhex(w[1])
        except ValueError:
            log("! kdpwrite: 非法 hex")
            return False
        if not data:
            log("! kdpwrite: 空数据")
            return False
        pkt = kf.writemem64_packet(kdp_live["key"], kdp_live["seq"], addr, data)
        ok = kdp_send_packet(pkt, "WRITEMEM64")
        if ok:
            kdp_live["seq"] = (kdp_live["seq"] + 1) & 0xFF
        return ok

    def cmd_kdpread(args):
        """kdpread <hexva> <nbytes>  -- KDP_READMEM64 while in the debugger."""
        w = args.split()
        if len(w) != 2:
            log("用法: kdpread <hexva> <nbytes>")
            return False
        try:
            import kdp_frame as kf
        except ImportError:
            log("! kdp_frame.py 不在 tools/ 下")
            return False
        try:
            addr = int(w[0], 16)
            n = int(w[1], 0)
        except ValueError:
            log("! kdpread: 非法参数")
            return False
        if not (0 < n <= 1024):
            log("! kdpread: nbytes 必须 1..1024")
            return False
        pkt = kf.readmem64_packet(kdp_live["key"], kdp_live["seq"], addr, n)
        ok = kdp_send_packet(pkt, "READMEM64")
        if ok:
            kdp_live["seq"] = (kdp_live["seq"] + 1) & 0xFF
        return ok

    def cmd_kdpreboot():
        """KDP_REBOOT (0x13) -- PSCI reset back to recovery, no power cycle."""
        try:
            import kdp_frame as kf
        except ImportError:
            log("! kdp_frame.py 不在 tools/ 下")
            return False
        return kdp_send_packet(kf.reboot_packet(kdp_live["key"], kdp_live["seq"]),
                               "REBOOT")

    def cmd_kdppush(hexstr):
        """Queue raw bytes into the KDP ring, splitting to fit the 127 B line.

        A SXNU1 line is capped at SXNU_CONTROL_LINE_BYTES = 127, and the fixed
        part (session, seq, verb, crc) eats about 51 of them, so at most ~24
        raw bytes fit per command. kdp_unserialize_packet() keeps its state in
        static variables, so splitting a frame across several calls is safe as
        long as the order is preserved.
        """
        hexstr = "".join(hexstr.split())
        if not hexstr or len(hexstr) % 2:
            log("用法: kdppush <hex>（偶数个 hex 字符）")
            return
        try:
            raw = bytes.fromhex(hexstr)
        except ValueError:
            log("! kdppush: 非法 hex")
            return
        CHUNK = 24
        ok = True
        for off in range(0, len(raw), CHUNK):
            part = raw[off:off + CHUNK]
            reply = send_cmd("KDPPUSH", part.hex())
            if not reply or "queued=" not in reply:
                log("! kdppush 第 %d 段失败: %s" % (off // CHUNK + 1, reply or "无回复"))
                ok = False
                break
        if ok:
            log("kdppush OK: %d B in %d chunks" % (len(raw), (len(raw) + CHUNK - 1) // CHUNK))

    def cmd_stats():
        up = sess.uptime()
        log("bytes=%d uptime=%s session=%016x seq=%d entered_kdp=%s"
            % (sess.total, ("%.1fs" % up) if up is not None else "-",
               ctl.session, ctl.seq, kdp_state["entered"]))

    def cmd_mark(text):
        log("[mark] %s" % text)
        try:
            with open(raw_path, "ab", buffering=0) as f:
                f.write(("\n%s [mark] %s\n" % (now_hms(), text)).encode())
        except Exception:
            pass

    def send_frame_only(verb, arg=""):
        """Send a frame and wait a SHORT time for its reply.

        DEBUGGER does reply -- the module builds and queues the reply before it
        arms the ring and calls Debugger() (QcomXnuUsbDebug.c:2397-2412), and
        that reply carries `queued=` and `pending=`, which is the only way to
        see whether the staged bytes actually reached kdp_rx. The trap follows,
        so the reply must be collected before the log floods; a 2 s wait is
        enough and never blocks on the trap itself.
        """
        verb = verb.upper()
        if not ctl.hello_done:
            log("! skip %s: HELLO 尚未成功" % verb)
            return False
        ctl.seq += 1
        seq = ctl.seq
        frame = ctl.build(verb, arg)
        log("TX %s %s (seq=%d)" % (verb, arg, seq))
        sess.send(frame)
        reply = sess.wait_reply(verb, 2.0, seq=seq)
        if reply:
            log("RX %s" % reply)
            event(kind="cmd", verb=verb, ok=True, reply=reply)
        else:
            # Expected for a verb that traps before answering; not an error.
            event(kind="cmd", verb=verb, ok=True, reply="(no reply)")
        return True

    def wait_port_gone(timeout=12.0):
        """After closing, wait until the OS stops reporting the port as ours."""
        end = time.time() + timeout
        while time.time() < end:
            try:
                import serial as _s
                p = _s.Serial(kdp_state["port"], 115200, timeout=0.05)
                p.close()
                return True
            except Exception:
                time.sleep(0.5)
        return False

    def reopen(retries=20, gap=1.5):
        """重新拿到句柄并重建 SXNU1 会话（HELLO 允许 session=0/seq=0）。"""
        nonlocal sess, ctl
        log(">>> reopening the CDC (up to %d tries) ..." % retries)
        for _try in range(retries):
            p = find_port() or kdp_state["port"]
            if p:
                try:
                    sess = Session(p, raw_path, log)
                    ctl = Control()
                    kdp_state["port"] = p
                    log(">>> CDC REOPENED on %s" % p)
                    event(kind="reopen", port=p)
                    send_cmd("HELLO", "")
                    return True
                except Exception as e:
                    last = e
            time.sleep(gap)
        log(">>> CDC REOPEN FAILED after %d tries" % retries)
        return False

    def do_kdp(sub):
        """把句柄交给 kdp_cli：关闭 -> 运行 -> 重开。"""
        words = (sub or "").split()
        action = words[0].lower() if words else "connect"

        if action == "status":
            log("[kdp] entered=%s port=%s" % (kdp_state["entered"], kdp_state["port"]))
            return True

        if action == "resume":
            # Rescue path for a Debugger() entry.
            #
            # MEASURED (kb-94): sending bytes AFTER the trap does not work. The
            # loop in kdp_connection_wait() reads through kdp_getc() ->
            # console_try_read_char() -> our serial_getc(), which with
            # kdp_rx_used == 0 calls serial_rx_drain(); and serial_rx_drain()
            # starts with acquire(), whose first line is
            #
            #     if (cpu_number() != master_cpu) return 0;
            #
            # The trapping thread is not on the master CPU, so the drain never
            # runs, nothing reaches kdp_rx, and kdp_getc() keeps returning 0.
            # Twelve bytes sent this way changed nothing (kb-93).
            #
            # What does work is staging the bytes BEFORE the trap:
            #
            #     DEBUGGER 6363636363636363
            #
            # kb-94 measured queued=8 pending=8 ovf=0 and then "Continuing..."
            # followed by the kernel carrying on with GPU init. Eight bytes, not
            # two: the pre-loop check and the loop itself both consume, and two
            # was not enough in practice.
            log("[kdp] resume-after-trap does not work: serial_rx_drain() "
                "cannot acquire on a non-master CPU")
            log("[kdp] stage the bytes before the trap instead:")
            log("[kdp]   kdp enter 6363636363636363")
            log("[kdp] (queued=8/pending=8 measured good on kb-94)")
            return False

        if action == "reset":
            # T46-Q1 exit: send a bare 'r'. kdp_connection_wait() answers it with
            # kdp_machine_reboot() -> kdp_machine_reboot_type(kPEPanicRestartCPU)
            # -> nabu_panic_restart() -> PSCI reset. This is the *reset* exit for
            # a panicked kernel, which is what we need; returning to a panicked
            # kernel is not supported (its return path needs DBOP_DEBUGGER).
            # Requires the module's serial_pending() to drain while not armed.
            log("[kdp] sending 'r' to reboot a panicked kernel")
            try:
                sess.send(b"r")
                sess.s.flush()
            except Exception as e:
                log("! write failed: %s" % e)
                return False
            time.sleep(2.0)
            log("[kdp] sent 'r'; expect a PSCI reset -> recovery in ~16 s")
            return True

        if action == "enter":
            if kdp_state["entered"]:
                log("[kdp] already in the debugger")
                return True
            # NO default payload. This used to send "6363", on the theory that
            # kdp_getc() needs a spare 'c' to reach its loop. That is only true
            # when the point of the round is to enter and immediately continue.
            # When a KDP frame is staged ahead of the trap, any extra byte lands
            # in the ring AFTER the frame and can be consumed by the pre-loop
            # kdp_getc() -- which is exactly the byte that must be the dummy
            # 0x00, not a 'c'. Sending one here corrupted the connect attempt.
            #
            # So: bare `kdp enter` stages nothing. Ask for the rescue explicitly
            # with `kdp enter 6363`, which queues two 'c' bytes: the pre-loop
            # check eats one (printing the Options banner) and the loop eats the
            # other, printing "Continuing..." (verified on kb-26).
            #
            # `kdp enter raw` is accepted too, as an explicit spelling of
            # "stage nothing", for scripts that would rather be unambiguous.
            payload = ""
            if len(words) > 1 and words[1].lower() != "raw":
                payload = words[1]
            if not send_frame_only("DEBUGGER", payload):
                return False
            drain_until = time.time() + 6
            while time.time() < drain_until:
                time.sleep(0.3)
            kdp_state["entered"] = True
            log("[kdp] entered the debugger (look for 'Waiting for remote debugger connection')")
            return True

        # Every remaining action needs the port released.
        need_enter = action in ("connect",) and not kdp_state["entered"]
        if need_enter:
            if not send_frame_only("DEBUGGER"):
                return False
            drain_until = time.time() + 6
            while time.time() < drain_until:
                time.sleep(0.3)
            kdp_state["entered"] = True
            log("[kdp] entered the debugger")

        port_now = kdp_state["port"] or port
        log("[kdp] releasing %s for kdp_cli" % port_now)
        sess.close()
        if not wait_port_gone():
            log("! %s still busy after close; kdp_cli may fail to open it"
                % (kdp_state["port"] or port))

        if action == "leave":
            argv = ["disconnect"]
        elif words:
            argv = list(words)
        else:
            argv = ["connect"]     # bare `kdp` means connect
        cmd = [sys.executable, "-u", "kdp_cli.py", "--port", port_now] + argv
        log("[kdp] $ " + " ".join(argv))
        rc = subprocess.call(cmd, cwd=args.kdp_dir)
        log("[kdp] kdp_cli rc=%d" % rc)
        event(kind="kdp", action=action, rc=rc)

        if action in ("connect", "leave") and rc == 0:
            kdp_state["entered"] = False
            log("[kdp] kernel should have returned from Debugger()")

        if not reopen():
            log("! port did not come back; stopping")
            sess.alive = False
            return False
        return True

    # 端口：显式直接用；否则等 COM3 出现（先起控制台→再刷机）
    port = args.port
    if not port:
        log(">>> waiting for CDC (VID=%s PID=%s), up to %ds ..." % (VID, PID, args.wait))
        t_wait = time.time()
        deadline = t_wait + args.wait
        attempt = 0
        last_note = -999.0
        while time.time() < deadline:
            attempt += 1
            port = find_port()
            el = time.time() - t_wait
            if port:
                log(">>> CDC FOUND after %.1fs (attempt %d): %s" % (el, attempt, port))
                break
            # Heartbeat every 2 s only; the instant a port matches we print
            # ">>> CDC FOUND" above and break, so the found path is never delayed.
            if el - last_note >= 2.0:
                last_note = el
                log(">>> scanning %.1fs, %d attempts, no CDC" % (el, attempt))
            time.sleep(0.2)
        if not port:
            log(">>> CDC NOT FOUND after %ds (%d attempts); giving up" % (args.wait, attempt))
            return 3
        log(">>> open attempt on %s ..." % port)
    else:
        log("using explicit port %s" % port)

    sess = open_with_retry(port, raw_path, log)
    if sess is None:
        log(">>> giving up: the CDC never became openable")
        return 4
    ctl = Control()
    kdp_state["port"] = port
    log("open %s ok; cmd file: %s" % (port, cmd_path))
    event(kind="open", port=port)

    cmd_pos = os.path.getsize(cmd_path)
    t0 = time.time()
    last_total = 0
    last_idle_check = time.time()
    last_output_at = time.time()
    last_output_total = 0
    last_arm_note = 0
    arm_sent = False
    if args.arm_at > 0:
        log("[arm] 已武装：设备 uptime >= %.0fs 自动发 REBOOT（窗口 %ds）"
            % (args.arm_at, args.reboot_window))

    # 预置脚本：open 后立即 HELLO + 脚本全部命令（紧凑，抓短窗口）
    if args.script and os.path.exists(args.script):
        log("=== auto script: %s ===" % args.script)
        send_cmd("HELLO", "")
        with open(args.script, encoding="utf-8") as f:
            for raw in f:
                raw = raw.strip()
                if not raw or raw.startswith("#"):
                    continue
                parts = raw.split(None, 1)
                verb = parts[0].upper()
                rest = parts[1] if len(parts) > 1 else ""
                if verb == "KDPRAW":
                    cmd_kdpraw(rest)
                elif verb == "KDPWRITE":
                    cmd_kdpwrite(rest)
                elif verb == "KDPREAD":
                    cmd_kdpread(rest)
                elif verb == "KDPREBOOT":
                    cmd_kdpreboot()
                elif verb == "KDPPUSH":
                    cmd_kdppush(rest)
                elif verb == "KDP":
                    do_kdp(rest)
                elif verb == "PTDUMP":
                    cmd_ptdump(rest)
                elif verb == "PANICDUMP":
                    cmd_panicdump(rest)
                elif verb == "SYM":
                    cmd_sym(rest)
                elif verb == "SYMS":
                    cmd_syms(rest)
                elif verb == "VAR":
                    cmd_var(rest)
                elif verb == "VMSTAT":
                    cmd_vmstat(rest)
                elif verb == "MEMSTAT":
                    cmd_memstat(rest)
                elif verb == "POOLSTAT":
                    cmd_poolstat(rest)
                elif verb == "MEMSTAGE":
                    cmd_memstage(rest)
                elif verb == "ALIASSTAT":
                    cmd_aliasstat(rest)
                elif verb == "METHODS":
                    cmd_methods(rest)
                elif verb == "CLIENTS":
                    cmd_clients(rest)
                elif verb == "WAITOUTER":
                    cmd_waitouter(rest)
                elif verb == "TSENS":
                    cmd_tsens(rest)
                elif verb == "FRAMESTAT":
                    cmd_framestat(rest)
                elif verb == "GPUSTATE":
                    cmd_gpustate(rest)
                elif verb == "SUBMIT":
                    cmd_submit(rest)
                elif verb == "IBCLASS":
                    cmd_ibclass(rest)
                elif verb == "SLOW":
                    cmd_slow(rest)
                elif verb == "CPSTAT":
                    cmd_cpstat(rest)
                elif verb == "IRQSTAT":
                    cmd_irqstat(rest)
                elif verb in ("DUMP", "DUMPPA"):
                    cmd_dump(raw)
                elif verb == "STATS":
                    cmd_stats()
                elif verb == "MARK":
                    cmd_mark(rest)
                else:
                    send_cmd(parts[0], rest)
        log("=== auto script done ===")

    while sess.alive:
        el = time.time() - t0
        if args.deadline and el > args.deadline:
            log("deadline reached"); break

        # stall_watchdog: if the device stops producing bytes for too long the
        # kernel is almost certainly wedged (e.g. inside the debugger, where it
        # no longer services the CDC). Exiting here is what actually releases
        # the Windows handle: a blocked serial.read() keeps the OS handle alive
        # and Stop-Process cannot kill a thread stuck in that kernel wait.
        if sess.write_stuck:
            log(">>> write stuck (kernel not servicing the CDC); releasing the port")
            event(kind="write_stuck")
            break
        if sess.total != last_output_total:
            last_output_total = sess.total
            last_output_at = time.time()
        elif args.stall_exit > 0 and (time.time() - last_output_at) > args.stall_exit:
            log(">>> no device output for %.0fs (stall watchdog); releasing the port"
                % (time.time() - last_output_at))
            event(kind="stall_exit", idle=round(time.time() - last_output_at, 1))
            break

        # --- 命令文件 ---
        try:
            size = os.path.getsize(cmd_path)
            if size > cmd_pos:
                with open(cmd_path, "rb") as f:
                    f.seek(cmd_pos)
                    data = f.read().decode("utf-8", "replace")
                cmd_pos = size
                for raw in data.splitlines():
                    raw = raw.strip()
                    if not raw or raw.startswith("#"):
                        continue
                    parts = raw.split(None, 1)
                    verb = parts[0].upper()
                    rest = parts[1] if len(parts) > 1 else ""
                    if verb == "KDPRAW":
                        cmd_kdpraw(rest)
                    elif verb == "KDPWRITE":
                        cmd_kdpwrite(rest)
                    elif verb == "KDPREAD":
                        cmd_kdpread(rest)
                    elif verb == "KDPREBOOT":
                        cmd_kdpreboot()
                    elif verb == "KDPPUSH":
                        cmd_kdppush(rest)
                    elif verb == "KDP":
                        do_kdp(rest)
                    elif verb == "PTDUMP":
                        cmd_ptdump(rest)
                    elif verb == "PANICDUMP":
                        cmd_panicdump(rest)
                    elif verb == "READUFS":
                        cmd_readufs(rest)
                    elif verb == "PANICTRIG":
                        cmd_panictrig(rest)
                    elif verb == "STACKSHOT":
                        cmd_stackshot(rest)
                    elif verb == "KDBG":
                        send_cmd("KDBG", "")
                    elif verb == "SYM":
                        cmd_sym(rest)
                    elif verb == "SYMS":
                        cmd_syms(rest)
                    elif verb == "VAR":
                        cmd_var(rest)
                    elif verb == "VMSTAT":
                        cmd_vmstat(rest)
                    elif verb == "MEMSTAT":
                        cmd_memstat(rest)
                    elif verb == "POOLSTAT":
                        cmd_poolstat(rest)
                    elif verb == "MEMSTAGE":
                        cmd_memstage(rest)
                    elif verb == "ALIASSTAT":
                        cmd_aliasstat(rest)
                    elif verb == "METHODS":
                        cmd_methods(rest)
                    elif verb == "CLIENTS":
                        cmd_clients(rest)
                    elif verb == "WAITOUTER":
                        cmd_waitouter(rest)
                    elif verb == "TSENS":
                        cmd_tsens(rest)
                    elif verb == "FRAMESTAT":
                        cmd_framestat(rest)
                    elif verb == "GPUSTATE":
                        cmd_gpustate(rest)
                    elif verb == "SUBMIT":
                        cmd_submit(rest)
                    elif verb == "IBCLASS":
                        cmd_ibclass(rest)
                    elif verb == "SLOW":
                        cmd_slow(rest)
                    elif verb == "CPSTAT":
                        cmd_cpstat(rest)
                    elif verb == "IRQSTAT":
                        cmd_irqstat(rest)
                    elif verb in ("DUMP", "DUMPPA"):
                        cmd_dump(raw)
                    elif verb == "STATS":
                        cmd_stats()
                    elif verb == "MARK":
                        cmd_mark(rest)
                    else:
                        send_cmd(parts[0], rest)
        except Exception as e:
            log("CMD ERR %s" % e)

        # --- 武装复位（单一句柄内发 REBOOT）---
        if args.arm_at > 0 and not arm_sent:
            up = sess.uptime()
            if up is None:
                if el - last_arm_note > 20:
                    last_arm_note = el
                    log("[arm] 未读到设备 uptime（等 nabu: spawn/exec t=），暂不发")
            elif up > args.arm_at + args.reboot_window:
                log("[arm] 超过窗口 (%.1f > %.0f+%ds)，放弃" % (up, args.arm_at, args.reboot_window))
                arm_sent = True
            elif up >= args.arm_at:
                log("[arm] 设备时间 %.1fs >= %.0fs，发 REBOOT" % (up, args.arm_at))
                send_cmd("REBOOT", "", timeout=3.0)
                arm_sent = True
                log("[arm] REBOOT 已发，USB 将掉")

        # --- 被动判据 ---
        if time.time() - last_idle_check >= 10:
            delta = sess.total - last_total
            event(kind="bytes", total=sess.total, delta=delta, alive=sess.alive,
                  uptime=sess.uptime())
            last_total = sess.total
            last_idle_check = time.time()
        time.sleep(0.2)

    sess.close()
    log("closed. total=%d B" % sess.total)
    event(kind="close", total=sess.total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
