#!/usr/bin/env python3
"""nabu 统一控制台 v2：单进程、单句柄，收日志 + 发 SXNU1 命令帧 + 被动链路判据。

v1 bug（已修）：
  - v1 每 10s 主动发 STATUS/PING 并推进 seq，超时又盲目 seq+=1，导致 seq 与
    内核 last_seq 失步；重发 HELLO 时用了非零 seq → 内核按协议拒绝（HELLO
    必须 session=0 seq=0）→ 控制通道彻底失效。
  - v2 修复：HELLO 永远 seq=0/session=0；命令帧严格 seq 递进；**不主动发
    判据帧**，改从命令 reply 与日志字节流被动提取判据，绝不干扰控制通道。

用法：
  先起控制台（等待 COM3，一旦枚举立即 open 并开始落日志）：
    python -u nabu_console.py --tag kb06 --out E:/nabu/logs --wait 300 --deadline 400
  再刷机（另一个终端）。控制台会在 COM3 出现时自动 open。
  发命令（append 一行到 cmd 文件，同句柄发出）：
    echo hello >> E:/nabu/logs/kb06-cmd.txt
    echo version >> E:/nabu/logs/kb06-cmd.txt
  控制台在 COM3 消失（WDT 咬回）时自动结束。

产出：
  <tag>-com.bin    完整 CDC 字节流（含早期日志）
  <tag>-ctl.txt    命令 TX/RX 与事件
  <tag>-cmd.txt    命令输入（append）
  <tag>-events.jsonl  被动判据（字节增量 / 命令回复成功失败）
"""
from __future__ import annotations
import argparse, binascii, json, os, queue, sys, threading, time
from datetime import datetime, timezone, timedelta

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("需要 pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(2)

CST = timezone(timedelta(hours=8))
VID, PID = "1209", "316D"


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
    """单句柄：reader 线程落全部字节到 com.bin，SXNU1R 行进 reply 队列。"""
    def __init__(self, port, raw_path, log):
        self.s = serial.Serial(port, 115200, timeout=0.05, write_timeout=2.0)
        self.out = open(raw_path, "ab", buffering=0)
        self.replies = queue.Queue()
        self.partial = b""
        self.running = True
        self.alive = True
        self.total = 0
        self.log = log
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()

    def _reader(self):
        while self.running:
            try:
                n = self.s.in_waiting
            except Exception as e:
                self.log("READ ERR %s" % e)
                self.alive = False
                break
            if not n:
                time.sleep(0.02)
                continue
            try:
                chunk = self.s.read(n)
            except Exception:
                self.alive = False
                break
            if not chunk:
                continue
            self.out.write(chunk)
            self.total += len(chunk)
            self.partial += chunk
            while b"\n" in self.partial:
                line, self.partial = self.partial.split(b"\n", 1)
                line = line.strip(b"\r")
                if line.startswith(b"SXNU1R"):
                    self.replies.put(line.decode("utf-8", "replace"))
        self.alive = False

    def send(self, payload):
        self.s.write(payload)

    def wait_reply(self, verb, timeout):
        """取匹配 verb 的 reply；顺带把不匹配的 SXNU1R 放回队列。"""
        deadline = time.time() + timeout
        held = []
        try:
            while time.time() < deadline:
                try:
                    line = self.replies.get(timeout=max(0.01, deadline - time.time()))
                except queue.Empty:
                    break
                # SXNU1R <sess> <seq> <VERB> <status> ...
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
    """SXNU1 状态机。HELLO 固定 seq=0/session=0；其后命令 seq 从 1 严格递进。"""
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
    ap.add_argument("--port", default=None, help="显式端口（跳过等待，设备已在系统态时用）")
    ap.add_argument("--wait", type=int, default=300, help="等 COM3 出现的最长秒数")
    ap.add_argument("--deadline", type=int, default=0, help="总运行时限，0=不限")
    ap.add_argument("--cmd-timeout", type=float, default=10.0, help="每条命令等 reply 的超时")
    ap.add_argument("--script", default=None,
                    help="预置命令文件：open COM3 + HELLO 后立即紧凑发完所有命令（抓短窗口用）")
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

    def send_cmd(verb, arg):
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
        reply = sess.wait_reply(verb, args.cmd_timeout)
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
            log("! no reply within %.1fs for %s" % (args.cmd_timeout, verb))
            event(kind="cmd", verb=verb, ok=False)
        return reply

    # 端口：显式直接用；否则等 COM3 出现（先起控制台→再刷机的场景）
    port = args.port
    if not port:
        log("waiting for CDC (VID=%s PID=%s), up to %ds ..." % (VID, PID, args.wait))
        deadline = time.time() + args.wait
        while time.time() < deadline:
            port = find_port()
            if port:
                log("found %s" % port)
                break
            time.sleep(2)
        if not port:
            log("PORT ABSENT after %ds" % args.wait)
            return 3
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

    # 预置脚本模式：open 后立即发 HELLO + 脚本全部命令（紧凑，抓短窗口）
    if args.script and os.path.exists(args.script):
        log("=== auto script: %s ===" % args.script)
        send_cmd("HELLO", "")                      # 必须先建 session
        with open(args.script, encoding="utf-8") as f:
            for raw in f:
                raw = raw.strip()
                if not raw or raw.startswith("#"):
                    continue
                parts = raw.split(None, 1)
                send_cmd(parts[0], parts[1] if len(parts) > 1 else "")
        log("=== auto script done ===")

    while sess.alive:
        if args.deadline and time.time() - t0 > args.deadline:
            log("deadline reached")
            break
        # --- 命令文件：每轮扫描一次 ---
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
        # --- 被动判据：仅记录字节增量，绝不发帧 ---
        if time.time() - last_idle_check >= 10:
            delta = sess.total - last_total
            event(kind="bytes", total=sess.total, delta=delta, alive=sess.alive)
            last_total = sess.total
            last_idle_check = time.time()
        time.sleep(0.2)

    sess.close()
    log("closed. total=%d B" % sess.total)
    event(kind="close", total=sess.total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
