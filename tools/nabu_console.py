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
"""
from __future__ import annotations
import argparse, binascii, json, os, queue, re, sys, threading, time
from datetime import datetime, timezone, timedelta

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("需要 pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(2)

CST = timezone(timedelta(hours=8))
VID, PID = "1209", "316D"
DEV_TIME_RE = re.compile(rb"^nabu: (?:spawn|exec)[^\n]*?\bt=(\d{6,})\b")


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

    def send(self, payload):
        self.s.write(payload); self.s.flush()

    def wait_reply(self, verb, timeout):
        deadline = time.time() + timeout
        held = []
        try:
            while time.time() < deadline:
                try:
                    line = self.replies.get(timeout=max(0.01, deadline - time.time()))
                except queue.Empty:
                    break
                parts = line.split()
                rverb = parts[3] if len(parts) > 3 else ""
                if rverb == verb.upper():
                    return line
                held.append(line)
        finally:
            for l in held:
                self.replies.put(l)
        return None

    def close(self):
        self.running = False
        try: self.t.join(timeout=2)
        except Exception: pass
        try: self.out.close(); self.s.close()
        except Exception: pass


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
    ap.add_argument("--script", default=None, help="预置命令文件：open 后立即紧凑发完")
    ap.add_argument("--arm-at", type=float, default=0, help="设备 uptime 到该秒数自动发 REBOOT（0=不武装）")
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
        if ctl.is_hello(verb):
            ctl.session, ctl.seq = 0, 0
        elif not ctl.hello_done:
            log("! skip %s: HELLO 尚未成功" % verb)
            return None
        else:
            ctl.seq += 1
        frame = ctl.build(verb, arg)
        log("TX %s %s (seq=%d)" % (verb, arg, ctl.seq))
        sess.send(frame)
        reply = sess.wait_reply(verb, timeout or args.cmd_timeout)
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
            log("! no reply within %.1fs for %s" % (timeout or args.cmd_timeout, verb))
            event(kind="cmd", verb=verb, ok=False)
        return reply

    # 端口：显式直接用；否则等 COM3 出现（先起控制台→再刷机）
    port = args.port
    if not port:
        log("waiting for CDC (VID=%s PID=%s), up to %ds ..." % (VID, PID, args.wait))
        deadline = time.time() + args.wait
        while time.time() < deadline:
            port = find_port()
            if port:
                log("found %s" % port); break
            time.sleep(2)
        if not port:
            log("PORT ABSENT after %ds" % args.wait); return 3
    else:
        log("using explicit port %s" % port)

    sess = Session(port, raw_path, log)
    ctl = Control()
    log("open %s ok; cmd file: %s" % (port, cmd_path))
    event(kind="open", port=port)

    cmd_pos = os.path.getsize(cmd_path)
    t0 = time.time()
    last_total = 0
    last_idle_check = time.time()
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
                send_cmd(parts[0], parts[1] if len(parts) > 1 else "")
        log("=== auto script done ===")

    while sess.alive:
        el = time.time() - t0
        if args.deadline and el > args.deadline:
            log("deadline reached"); break

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
                    send_cmd(parts[0], parts[1] if len(parts) > 1 else "")
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
