#!/usr/bin/env python3
"""nabu CDC 控制台：单进程、单句柄，一边落盘全文日志，一边在同一个句柄上发 SXNU1 控制帧。

为什么要单句柄
--------------
设备侧是硬约束（nabu-usb-cdc/docs/07-control-channel-and-wdt-park.md §5.1）：
一个 boot 周期内 close() 之后再 open()，要么阻塞约 120 s，要么直接
ERROR_GEN_FAILURE(31)。所以「采集进程」和「控制进程」不能是两个进程、
两个 Serial 对象。本工具把一个进程做成控制台：reader 线程只负责收，
主线程从 stdin 拿命令，两者共用同一个句柄。

用法
----
    python tools/cdc-console.py --tag r900            # 等端口出现，最多 300 s
    python tools/cdc-console.py --tag r900 --wait 600 # 开机前先起好，等 10 分钟

    进入后直接敲：
        hello            建立 session（其它动词的前置）
        status           读 usb=/er=/rx=/rxo=/tx=/bp=/bo=/poll=/ev=/gen=/rej=
        ping             rx/tx/poll 快照
        screenshot       让内核抓一帧到 UFS
        shotnext          等下一帧再抓
        reboot --confirm 让板子复位（会掉 USB）
        mark 文字        往日志里插一条主机时间锚点
        stats / help / quit

输出
----
    logs/<tag>-console.bin       原始字节（完整日志，含 SXNU1R）
    logs/<tag>-console.ctl.txt   只含控制帧的 TX/RX 与会话状态，方便事后对账

红线
----
* 默认**不重连**（`--reconnect` 才开）。掉线就报告掉线，不偷偷换句柄。
* RX 在 boot+70~90 s 之后失效是设备侧已知行为，不是本工具的 bug；
  发帧无回复时本工具会直接把这条已知边界打出来。
* 不做任何 boot/erase 操作，只写 logs/ 下的两个文件。
"""
import argparse
import binascii
import datetime
import os
import queue
import re
import sys
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover
    sys.exit("需要 pyserial：pip install pyserial")

VID, PID = 0x1209, 0x316D
REPLY_RE = re.compile(rb"SXNU1R ([0-9a-f]{16}) ([0-8][0-9a-f]{7}) ([A-Z]+) (.*) ([0-9a-f]{8})$")


def now_hms():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]


def crc32(body):
    return binascii.crc32(body) & 0xFFFFFFFF


def find_port():
    for p in list_ports.comports():
        if p.vid == VID and p.pid == PID:
            return p.device, "VID_1209:PID_316D"
    for p in list_ports.comports():
        blob = " ".join(str(x) for x in (p.description, p.hwid) if x).upper()
        if "QCOMXNUUSBDEBUG" in blob or "QUALCOMM XNU" in blob:
            return p.device, "QCOMXNUUSBDEBUG"
    return None, None


def all_ports():
    return [(p.device, p.vid, p.pid, (p.description or "")) for p in list_ports.comports()]


# --------------------------------------------------------------------------
# SXNU1 客户端状态
# --------------------------------------------------------------------------
class Control:
    def __init__(self, session=0, seq=0):
        self.session = session
        self.seq = seq
        self.pending = None      # 期望的动词，大写
        self.rejected = 0
        self.acked = 0

    def frame(self, verb):
        body = "SXNU1 %016x %08x %s" % (self.session, self.seq, verb)
        return ("%s %08x\n" % (body, crc32(body.encode()))).encode()

    def build(self, verb):
        verb = verb.upper()
        if verb == "HELLO":
            self.session, self.seq = 0, 0
        else:
            if not self.session:
                raise RuntimeError("no session: 先发 hello")
            self.seq += 1
        self.pending = verb
        return self.frame(verb)

    def on_reply(self, line):
        """返回 (verb, tail) 或 None。"""
        m = REPLY_RE.search(line)
        if not m:
            self.rejected += 1
            return None
        body = line[:m.start(5) - 1]
        want = int(m.group(5), 16)
        got = crc32(body)
        if want != got:
            self.rejected += 1
            return ("CRC", "want=%08x got=%08x" % (want, got))
        verb = m.group(3).decode()
        tail = m.group(4).decode()
        sess = int(m.group(1), 16)
        if verb == "HELLO" and sess:
            self.session = sess
            kv = dict(x.split("=", 1) for x in tail.split() if "=" in x)
            try:
                self.seq = int(kv.get("last_seq", "0"), 16)
            except ValueError:
                self.seq = 0
        self.pending = None
        self.acked += 1
        return (verb, tail)


# --------------------------------------------------------------------------
# 采集会话
# --------------------------------------------------------------------------
class Session:
    def __init__(self, port, raw_path, ctl_path, echo=True, quiet_prefixes=()):
        self.port = port
        self.raw = open(raw_path, "ab", buffering=0)
        self.ctl = open(ctl_path, "a", encoding="utf-8", buffering=1)
        self.echo = echo
        self.quiet_prefixes = tuple(quiet_prefixes)
        self.replies = queue.Queue()
        self.lock = threading.Lock()
        self.bytes = 0
        self.lines = 0
        self.last_data = None
        self.opened_at = time.time()
        self.alive = True
        self.lost_why = ""
        self.lost_at = None
        self.running = True
        self.muted = set()          # 被静音的行前缀（控制台里 `mute <前缀>`）
        self._partial = b""
        self._partial_at = 0.0
        self.s = serial.Serial(port, 115200, timeout=0.05, write_timeout=2.0)
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()

    # ---------------- reader ----------------
    def _emit(self, line, nl=True):
        try:
            sys.stdout.write(line + ("\n" if nl else ""))
            sys.stdout.flush()
        except Exception:
            pass

    def _reader(self):
        while self.running:
            try:
                n = self.s.in_waiting
            except Exception as e:
                self.lost_why = "in_waiting: %s" % e
                break
            if not n:
                if self._partial and time.time() - self._partial_at > 0.25:
                    self._flush_partial()
                time.sleep(0.02)
                continue
            try:
                chunk = self.s.read(n)
            except Exception as e:
                self.lost_why = "read: %s" % e
                break
            if not chunk:
                continue
            self.raw.write(chunk)
            with self.lock:
                self.bytes += len(chunk)
                self.last_data = time.time()
            buf = self._partial + chunk
            self._partial = b""
            while True:
                i = buf.find(b"\n")
                if i < 0:
                    break
                line, buf = buf[:i], buf[i + 1:]
                if line.endswith(b"\r"):
                    line = line[:-1]
                self._on_line(line)
            self._partial = buf[-8192:]
            self._partial_at = time.time()
        if self.running:
            self.alive = False
            self.lost_at = now_hms()
            with self.lock:
                self.lost_at = self.lost_at
            self._emit("\n[cdc] USB LOST at %s (%s), total %d B"
                       % (self.lost_at, self.lost_why, self.bytes))

    def _flush_partial(self):
        line, self._partial = self._partial, b""
        text = line.decode("utf-8", "replace")
        self._emit(text, nl=False)

    def _on_line(self, line):
        with self.lock:
            self.lines += 1
        text = line.decode("utf-8", "replace")
        if line.startswith(b"SXNU1R"):
            self.replies.put(line)
            self._emit("\x1b[36m» %s\x1b[0m" % text)
            return
        for pfx in self.quiet_prefixes:
            if text.startswith(pfx):
                return
        for pfx in self.muted:
            if text.startswith(pfx):
                return
        if self.echo:
            self._emit(text)

    # ---------------- control ----------------
    def send(self, payload):
        self.s.write(payload)
        self.s.flush()

    def wait_reply(self, timeout):
        try:
            return self.replies.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        self.running = False
        try:
            self.t.join(timeout=1.0)
        except Exception:
            pass
        for h in (self.s, self.raw, self.ctl):
            try:
                h.close()
            except Exception:
                pass


# --------------------------------------------------------------------------
# 端口等待（与 cdc-capture.py 同一套相位，但只等不抢）
# --------------------------------------------------------------------------
def wait_for_port(explicit, wait_s, log):
    if explicit:
        log("using explicit port %s" % explicit)
        return explicit
    # Phase 1：幽灵端口（TWRP/fastboot 下 COMx 还在但 USB 功能是死的）
    had = False
    deadline = time.time() + 20
    while time.time() < deadline:
        dev, how = find_port()
        if dev:
            had = True
            log("ghost CDC %s present (%s), waiting for it to go ..." % (dev, how))
            time.sleep(2)
            continue
        break
    if had:
        log("ghost CDC gone")
    # Phase 2：等新枚举
    deadline = time.time() + wait_s
    tick = 0
    while time.time() < deadline:
        dev, how = find_port()
        if dev:
            log("found %s (matched by %s)" % (dev, how))
            return dev
        if tick % 5 == 0:
            log("waiting for CDC ... %d s left; ports=%s"
                % (int(deadline - time.time()), all_ports() or "none"))
        tick += 1
        time.sleep(2)
    return None


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="nabu CDC console (single handle)")
    ap.add_argument("--tag", default="console")
    ap.add_argument("--port", default=None, help="默认按 VID_1209:PID_316D 自动匹配")
    ap.add_argument("--wait", type=int, default=300, help="等端口出现的最长秒数")
    ap.add_argument("--deadline", type=int, default=0, help="最多跑 N 秒后自动退出，0=不限")
    ap.add_argument("--reconnect", action="store_true",
                    help="掉线后尝试重开（违反单句柄约束，只在明确知道后果时用）")
    ap.add_argument("--no-echo", action="store_true", help="不把日志回显到终端")
    ap.add_argument("--drop", action="append", default=[],
                    help="启动就静音的行前缀，可重复，如 --drop NativeAdrenoBO")
    ap.add_argument("--self-test", action="store_true", help="只跑内置自测，不碰串口")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    outdir = "E:/nabu/logs"
    os.makedirs(outdir, exist_ok=True)
    raw_path = os.path.join(outdir, "%s-console.bin" % args.tag)
    ctl_path = os.path.join(outdir, "%s-console.ctl.txt" % args.tag)

    def log(msg):
        line = "%s %s" % (now_hms(), msg)
        print(line, flush=True)
        try:
            with open(ctl_path, "a", encoding="utf-8") as f:
                f.write(line + "\n")
        except Exception:
            pass

    # 不在跨轮追加：同名 tag 直接从头开始
    if os.path.exists(raw_path):
        os.remove(raw_path)
    log("console tag=%s -> %s" % (args.tag, raw_path))

    port = wait_for_port(args.port, args.wait, log)
    if not port:
        log("PORT ABSENT after %d s (find_port()=None). COM1 是主机自己的 UART，"
            "板子 CDC 没枚举。" % args.wait)
        return 3

    time.sleep(0.5)        # PnP 注册到接口就绪之间需要一点时间
    sess = None
    for attempt in range(1, 6):
        try:
            sess = Session(port, raw_path, ctl_path, echo=not args.no_echo,
                           quiet_prefixes=args.drop)
            log("open %s @115200 ok (attempt %d)" % (port, attempt))
            break
        except Exception as e:
            log("open attempt %d failed: %s winerror=%s"
                % (attempt, e, getattr(e, "winerror", None)))
            time.sleep(1)
    if sess is None:
        log("could not open %s" % port)
        return 3

    ctl = Control()
    t0 = time.time()
    last_stats = 0.0
    rc = 0

    def do_verb(verb, arg="", timeout=10.0):
        try:
            payload = ctl.build(verb)
        except RuntimeError as e:
            log("! %s" % e)
            return None
        log("TX %s" % payload.decode().rstrip())
        try:
            sess.send(payload)
        except Exception as e:
            log("! write failed: %s" % e)
            return None
        rep = sess.wait_reply(3.0 if verb.upper() == "REBOOT" else timeout)
        if rep is None:
            log("! no SXNU1R within %.0fs for %s" % (timeout, verb.upper()))
            return None
        parsed = ctl.on_reply(rep)
        if parsed is None:
            log("! unparsable reply: %r" % rep)
            return None
        v, tail = parsed
        log("RX %s %s" % (v, tail))
        return tail

    log("elapsed 0 s. 输入 help 看命令；日志正在落 %s" % raw_path)

    stdin_dead = False
    cmd_file = os.path.join(outdir, "%s-console.cmd" % args.tag)
    cmd_pos = 0
    if os.path.exists(cmd_file):
        cmd_pos = os.path.getsize(cmd_file)
    log("命令文件通道: %s（往里面 append 一行就能发命令，stdin 关了也能用）" % cmd_file)

    try:
        while True:
            # 命令来源两条：stdin（有终端时）+ 命令文件（无人值守时）
            line = None
            # 1) 命令文件优先——无人值守下这是唯一入口
            try:
                if os.path.exists(cmd_file):
                    size = os.path.getsize(cmd_file)
                    if size > cmd_pos:
                        with open(cmd_file, "rb") as f:
                            f.seek(cmd_pos)
                            blob = f.read(size - cmd_pos)
                        cmd_pos = size
                        for raw_line in blob.decode("utf-8", "replace").splitlines():
                            if raw_line.strip():
                                line = raw_line.strip()
                                break
            except Exception as e:
                log("cmd file read failed: %s" % e)
            # 2) stdin
            if line is None and not stdin_dead:
                try:
                    line = input_now(0.05)
                except (EOFError, StdinClosed):
                    stdin_dead = True
                    log("stdin 已关：改为只认命令文件 %s" % cmd_file)
                except KeyboardInterrupt:
                    break
                except NoInput:
                    line = None

            if line is None:
                # 没输入：做周期状态检查
                el = time.time() - t0
                if args.deadline and el > args.deadline:
                    log("deadline %.0fs reached" % args.deadline)
                    break
                if el - last_stats > 30:
                    last_stats = el
                    with sess.lock:
                        b, ln = sess.bytes, sess.lines
                    age = "-" if sess.last_data is None else "%.1fs" % (time.time() - sess.last_data)
                    log("[stats] %.0fs  %d B / %d 行  最后数据 %s  %s"
                        % (el, b, ln, age, "ALIVE" if sess.alive else "LOST"))
                if not sess.alive and not args.reconnect:
                    log("USB LOST，控制台退出（--reconnect 可改）")
                    rc = 4
                    break
                continue

            line = line.strip()
            if not line:
                continue
            head, _, rest = line.partition(" ")
            cmd = head.lower()
            rest = rest.strip()

            if cmd in ("quit", "exit", "q"):
                break
            elif cmd in ("help", "?"):
                print(HELP, flush=True)
            elif cmd == "stats":
                with sess.lock:
                    b, ln = sess.bytes, sess.lines
                age = "-" if sess.last_data is None else "%.1fs" % (time.time() - sess.last_data)
                print("elapsed=%.1fs bytes=%d lines=%d last_data_age=%s alive=%s "
                      "session=%016x seq=%d tx=%d rx=%d rej=%d"
                      % (time.time() - t0, b, ln, age, sess.alive,
                         ctl.session, ctl.seq, ctl.acked, ctl.acked, ctl.rejected), flush=True)
            elif cmd == "mark":
                stamp = "[mark] %s" % rest
                log(stamp)
                try:
                    # 往原始日志里也插一条（主机时间锚点，事后对账用）
                    sess.raw.write(("\n%s %s\n" % (now_hms(), stamp)).encode())
                except Exception:
                    pass
            elif cmd == "mute":
                if not rest:
                    print("mute 后面跟行前缀，如：mute NativeAdrenoBO", flush=True)
                else:
                    sess.muted.add(rest)
                    print("muted %r（当前 %d 条）" % (rest, len(sess.muted)), flush=True)
            elif cmd == "unmute":
                sess.muted.discard(rest)
                print("muted=%d" % len(sess.muted), flush=True)
            elif cmd == "hello":
                do_verb("HELLO")
            elif cmd in ("ping", "status", "screenshot", "shotnext"):
                do_verb(cmd.upper())
            elif cmd == "reboot":
                if rest != "--confirm":
                    print("reboot 需要 --confirm", flush=True)
                else:
                    do_verb("REBOOT", timeout=3.0)
                    print("板子会复位，USB 会掉；本工具不自动重连。", flush=True)
            else:
                print("unknown: %s（help 看命令）" % cmd, flush=True)

            if not sess.alive and not args.reconnect:
                log("USB LOST，控制台退出（--reconnect 可改）")
                rc = 4
                break
    finally:
        with sess.lock:
            total = sess.bytes
        log("closing: %d B, %.0f s" % (total, time.time() - t0))
        sess.close()
    return rc


HELP = """\
命令：
  hello             建立 session（HELLO 必须 session=0 seq=0）
  ping              rx / rxo / tx / poll 快照
  status            usb= er= rx= rxo= tx= bp= bo= poll= ev= gen= rej=
  screenshot        让内核抓一帧写 UFS
  shotnext          等下一个 flip 再抓
  reboot --confirm  让板子复位（走 PSCI，会掉 USB）
  mark <文字>       插一条主机时间锚点（同时写进原始日志）
  mute <前缀>       静音某类刷屏行，如 mute NativeAdrenoBO
  unmute <前缀>     取消静音
  stats             本地统计
  quit              退出（会正常关闭句柄）

注意：
  * 一个 boot 周期只有一个句柄。退出后不要立刻重开，会撞 120 s 阻塞或 ERROR_GEN_FAILURE(31)。
  * 控制帧的 RX 在 boot+70~90 s 后失效，发帧没回复是设备侧已知行为。
"""


# --------------------------------------------------------------------------
# 非阻塞 stdin
# --------------------------------------------------------------------------
class NoInput(Exception):
    pass


class StdinClosed(Exception):
    pass


_stdin_q = queue.Queue()
_stdin_started = False


def _stdin_pump():
    while True:
        try:
            s = sys.stdin.readline()
        except Exception:
            _stdin_q.put(None)
            return
        if not s:
            _stdin_q.put(None)
            return
        _stdin_q.put(s.rstrip("\r\n"))


def input_now(timeout):
    """拿到一行命令；超时抛 NoInput，stdin 已关抛 StdinClosed。

    stdin 关掉（无人值守 / 重定向自 NUL）不等于「该退出」——只是没有命令可发，
    采集必须继续。调用方把 StdinClosed 当成「永不产生命令」处理。
    """
    global _stdin_started
    if not _stdin_started:
        threading.Thread(target=_stdin_pump, daemon=True).start()
        _stdin_started = True
    try:
        v = _stdin_q.get(timeout=timeout)
    except queue.Empty:
        raise NoInput()
    if v is None:
        raise StdinClosed()
    return v


# --------------------------------------------------------------------------
# 自测：不需要板子，验证帧生成、CRC、回复解析、静音与分片
# --------------------------------------------------------------------------
def self_test():
    print("== CRC / 帧 ==")
    c = Control()
    f = c.build("HELLO")
    text = f.decode().rstrip()
    print("HELLO frame:", text)
    body, _, crc = text.rpartition(" ")
    assert "%08x" % crc32(body.encode()) == crc, "HELLO crc 不对"
    assert text.startswith("SXNU1 0000000000000000 00000000 HELLO "), "HELLO 必须 session=0 seq=0"
    assert len(f) <= 128, "帧必须 <=128 B（含换行）"

    # 没有 session 时非 HELLO 必须被拒
    try:
        c.build("STATUS")
        raise AssertionError("无 session 时 STATUS 不该生成")
    except RuntimeError:
        print("OK: 无 session 时拒绝非 HELLO")

    # 用内核侧的真实回复把 session/seq 灌进去
    sess = 0xA1A4D73A12F05416
    seq = 3
    tail = " session=%016x last_seq=%08x control=1" % (sess, seq)
    body = "SXNU1R %016x %08x HELLO OK%s" % (sess, 0, tail)
    line = ("%s %08x" % (body, crc32(body.encode()))).encode()
    print("simulated reply:", line.decode())
    got = c.on_reply(line)
    assert got and got[0] == "HELLO", got
    assert c.session == sess and c.seq == seq, (c.session, c.seq)
    print("OK: HELLO 回复解析出 session=%016x seq=%d" % (c.session, c.seq))

    f = c.build("STATUS")
    text = f.decode().rstrip()
    assert text == "SXNU1 %016x %08x STATUS %s" % (sess, seq + 1, crc32(
        ("SXNU1 %016x %08x STATUS" % (sess, seq + 1)).encode()) and
        "%08x" % crc32(("SXNU1 %016x %08x STATUS" % (sess, seq + 1)).encode())), text
    print("OK: STATUS 帧 seq 严格递进 ->", text)

    # CRC 被人为改坏必须被识别
    bad = line[:-8] + b"deadbeef"
    assert c.on_reply(bad) == ("CRC", "want=%08x got=%08x"
                               % (int("deadbeef", 16), crc32(bad[:-9]))), "坏 CRC 没抓住"
    print("OK: 坏 CRC 被识别")

    print()
    print("== 端口匹配 ==")
    print("按 VID_1209:PID_316D 匹配，当前主机端口：", all_ports())
    print()
    print("== 全部通过 ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
