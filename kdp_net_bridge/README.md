# kdp_net_bridge —— UDP ↔ USB CDC 网桥（XNU KDP over Ethernet frames）

把 nabu 设备侧 USB CDC 上的 KDP 帧转成主机 UDP socket，让 KDP 客户端
（Apple lldb 的 `kdp-remote`，或本项目自己的 `kdp_cli.py`）可以通过标准 UDP
端口 `41139` 连接。

> **状态**：代码 + 真机日志离线回放 + 端到端自检完成（44/44）；**尚未上板联调**。

---

## 1. 它解决什么

XNU 的 KDP 核心（`osfmk/kdp/kdp_udp.c`）**总是**把传输层缓冲当成完整的
`Ethernet(14) + IPv4(20) + UDP(8) + KDP` 帧来收发：`kdp_send()/kdp_reply()`
无条件前置 L2/L3 后才调用 `kdp_send_data()`（`:544-687`，本机直读），而
serial 模式下该回调就是 `kdp_serial_send()`。因此**当前设备的 0xFA 串口
KDP 帧里装的已经是完整以太网帧**，`kdp_poll()` 也按 `ether_type/proto/ip_hl/
dport=41139` 校验（`KDP_REMOTE_PORT`，`osfmk/kdp/kdp_protocol.h:72`）。

⇒ **不需要改设备任何代码**：主机只要按 Apple 的 serial-KDP 帧格式收发出帧，
再把里面的以太网帧与 UDP 互相转换即可。真机日志回放已证实这一点（§4）。

网桥在主机侧做的转换：

```
┌────────────────────┐        UDP :41139        ┌──────────────────────┐
│  KDP 客户端         │  ─────────────────────▶  │      nabu 设备        │
│  - Apple lldb       │   一个 datagram =         │  XNU KDP core         │
│    kdp-remote       │   一个裸 KDP 包            │  (kdp_send/kdp_reply) │
│  - kdp_cli.py       │ ◀─────────────────────  │  + 裸帧回调           │
│    --transport udp  │                          └──────────────────────┘
└────────────────────┘            │
                                  ▼   USB CDC (COM21 / /dev/ttyACM0)
                           ┌──────────────┐
                           │ 本网桥        │  KDPF 帧 = 12 B 头 + 以太网帧
                           │ kdp_net_bridge│  CRC32 = zlib(以太网帧)
                           └──────────────┘
```

两种 UDP 载荷约定（`--udp-mode`）：

| 模式 | 一个 UDP datagram = | 用途 |
|---|---|---|
| `kdp`（默认） | **裸 KDP 包**（8 B `kdp_hdr_t` + body） | 与 Apple lldb `CommunicationKDP` 完全兼容；网桥负责加上/剥掉 Ethernet/IP/UDP |
| `eth` | **完整以太网帧** | 纯字节泵；本项目 `kdp_cli.py --transport udp` 走这条路（复用未改动的 `kdp_proto.KdpSession`） |

`kdp` 模式下若收到「本身就像 Ether/IPv4/UDP/KDP」的 datagram，会原样透传，
所以两种客户端可以同时连。

---

## 2. 文件

| 文件 | 作用 |
|---|---|
| `kdp_cdc_frame.py` | CDC 分帧/解帧（magic + version + flags + len + CRC32），无第三方依赖 |
| `kdp_net_bridge.py` | 主程序：pyserial ↔ UDP 双向转发、统计、日志、优雅退出；`--selftest` |
| `selftest_loopback.py` | 伪串口 + 假内核的离线自检（27 项） |
| `../kdp_client/kdp_udp.py` | 给 `kdp_cli.py` 用的 UDP transport（新增，未改动协议层） |
| `../kdp_client/kdp_cli.py` | 已扩展 `--transport {serial,udp} --host --udp-port`（原 16/16 自检仍通过） |

依赖：Python 3.10+。真机需要 `pyserial`（`python -m pip install pyserial`）。
自检只需要标准库（L2/L3 交叉校验需要同级的 `../kdp_client/kdp_proto.py`）。

---

## 3. CDC 分帧格式（`--cdc-framing`）

两种格式，**载荷都是完整以太网帧**（这一点由 XNU 源码保证，与选哪种 framing 无关）：

| `--cdc-framing` | 格式 | 状态 |
|---|---|---|
| **`serial`**（默认） | Apple serial-KDP：`0xFA \| escape(CRC32_LE(4)) \| 0xFA \| escape(packet) \| 0xFB` | **当前设备实际使用的格式**，直接复用 `kdp_client/kdp_serial.py` 的 `SerialDeframer`/`frame_packet` |
| `kdpf` | 主机自定义：`"KDPF" + ver + flags + len(LE16) + crc32(LE32) + payload` | 备选；若将来设备改为裸帧回调可用 |

### 3.1 `serial`（当前设备，推荐）

```
0xFA  ESCAPE(crc32_le[4])  0xFA  ESCAPE(eth_frame)  0xFB
```

* CRC32 = `z_crc32(0, eth_frame, len)`，小端写在线上，且**CRC 四个字节自身
  也要转义**；
* 转义规则：字节 ∈ `{0xFA, 0xFB, 0xFE, 0x0A}` → `0xFE, ~byte`（`\n` 是因为
  内核串口控制台会把它变成 CRLF）；
* 第二个 `0xFA` 是分隔符兼「CRC 存在」标记；
* 接收缓冲 `dsBuffer[1518]`，因此 payload 最大 1517 B 才被接受（1518 B 会
  在最后一个字节触发溢出丢弃）。真实 KDP 包 ≤1200 B，封装后 ≤1242 B。

### 3.2 `kdpf`（备选）

```
偏移  长度  字段
0     4    magic   = b"KDPF"
4     1    version = 0x01
5     1    flags   = 0x00（保留）
6     2    length  = payload 长度，小端（1..1518）
8     4    crc32   = zlib CRC-32(payload)，小端
12    N    payload = 完整以太网帧（14 + 20 + 8 + KDP）
```

无转义、固定 12 B 开销；接收机「找 magic → 校验头 → 收满 → 验 CRC」，坏头会
把后续字节重新喂回扫描器（可恢复重叠 magic）；空闲超时（`--cdc-idle-reset`
，默认 1 s）丢弃被截断的半帧。

### 3.3 示例（同一以太网帧，两种上线形态）

T38 的 CONNECT 以太网帧（64 B）：

```
ffffffffffff73657269616c080045000032000100003c11184f00000000abadbabea0b4a0b3001e000000001600dec03713b4a0b9a06772656574696e677300
```

`kdpf` 上线（76 B）：

```
4b445046 01 00 4000 f294c6f2 ffffffffffff...6772656574696e677300
```

`serial` 上线（把该帧转义后夹在 `0xFA…0xFA…0xFB` 之间；CRC 同为
`0xf294c6f2`）。二者载荷相同，所以 `--udp-mode` 的转换逻辑完全共用。

---

## 4. 真机日志离线回归（强验收）

`replay_cdc_logs.py` 用 `SerialDeframer` 回放 `E:\nabu\logs\kb*-com.bin`
（真实 USB CDC 抓包），不需要硬件：

```powershell
python E:\nabu\state\bridge\answers\kdp_net_bridge\replay_cdc_logs.py `
       --glob "E:/nabu/logs/kb*-com.bin" `
       --json E:\nabu\state\bridge\answers\kdp_net_bridge\replay_kb_logs_summary.json
```

2026-11-24 实测结果：

| 指标 | 数值 |
|---|---|
| 扫描文件数 | **187** |
| 扫描字节数 | **590,271,860**（时点值；日志目录被并行任务追加，两次运行分别为 584,339,306 / 590,271,860） |
| 原始 `0xFA` 计数 | 154 |
| CRC 通过帧（`frames_ok`） | **73**（= 146 个 `0xFA` 标记） |
| CRC 丢弃 | **0** |
| 溢出丢弃 | **0** |
| 未归因 `0xFA`（控制台噪声/截断残片） | 8 |
| payload 为合法 Ether/IPv4/UDP 的比例 | **73/73 = 100%** |
| 其中 UDP 端口 = 41139 | **73/73 = 100%** |
| payload 长度 | min 50 / max 118；top: 58×30, 62×24, 54×13, 50×3, 70×1 |
| 有帧的文件 | kb28..kb41（含 kb35b/kb35c），其余文件为纯控制台输出 |

结论：**现有 serial-KDP 帧载荷确实是完整以太网帧，且 100% 带 KDP 端口
41139** —— 网桥的 `--cdc-framing serial` 与设备现状精确对齐，无需改设备。

> 注：日志目录可能有并行任务在写入，字节数会随时间变化；`--json` 输出
> 保存了本次逐文件明细作为证据。

---

## 5. 用法

### 5.1 启动网桥

```powershell
# Windows：COM 号用 Get-CimInstance Win32_SerialPort 或设备管理器确认
# 默认 --cdc-framing serial，正匹配当前设备
python E:\nabu\state\bridge\answers\kdp_net_bridge\kdp_net_bridge.py `
       --port COM21 --udp-port 41139 --udp-mode eth

# 若将来设备改用 KDPF 裸帧：加 --cdc-framing kdpf

# WSL：需要先 usbipd-win 透传（见 T29 §5.4）
python3 kdp_net_bridge.py --port /dev/ttyACM0 --udp-host 127.0.0.1
```

常用参数：`--cdc-framing {serial,kdpf}`、`--udp-mode {kdp,eth}`、
`--baud`（默认 115200，USB CDC bulk 吞吐与波特率无关）、`--poll`、
`--cdc-idle-reset`、`--stats-interval 10`、`--verbose`。

### 5.2 用自己的客户端连（推荐，当前主机唯一可落地的原生路径）

```powershell
cd E:\nabu\state\bridge\answers\kdp_client
python kdp_cli.py --transport udp --host 127.0.0.1 --udp-port 41139 connect
python kdp_cli.py --transport udp --host 127.0.0.1 --udp-port 41139 readmem 0xfffffe0012345000 64
python kdp_cli.py --transport udp --host 127.0.0.1 --udp-port 41139 disconnect
```

`kdp_udp.py` 只是把「一个以太网帧」装进一个 UDP datagram；`KdpSession`
（CONNECT/VERSION/HOSTINFO/READMEM64/READREGS/断点/RESUME…）完全复用。

### 5.3 用 Apple lldb 连（需要带 KDP 插件的 lldb）

```bash
lldb
(lldb) process connect --plugin kdp-remote udp://127.0.0.1:41139
# 简写： kdp-remote 127.0.0.1   （等价于 udp://127.0.0.1:41139）
```

网桥必须以默认 `--udp-mode kdp` 启动。lldb 在 UDP 上发的是**裸 KDP 包**
（`CommunicationKDP::SendRequestPacketNoLock()` 直接 `Write(packet_data,
packet_size)`），所以网桥会代为封装/解封装 L2/L3。

> **注意**：上游 LLVM 在 Linux/Windows **不构建** KDP 插件，本机（Windows +
> WSL Ubuntu 24.04）没有可用 lldb。详见 `../T51-host-bridge.md` §6。

---

## 6. 自检

```powershell
python E:\nabu\state\bridge\answers\kdp_net_bridge\kdp_net_bridge.py --selftest
# 或
python E:\nabu\state\bridge\answers\kdp_net_bridge\selftest_loopback.py
```

不需要硬件/串口/pyserial。当前输出 **44/44 tests passed**，覆盖：

* `kdpf` 分帧/解帧：18 种长度往返、7 B 分块投递、CRC 损坏丢弃并重同步、
  垃圾前缀中的伪 magic 恢复、坏 version/flags/零长度/超长头拒绝、半帧空闲
  恢复、空/超长 payload 拒绝、固定 12 B 开销；
* **`serial` 分帧**：11 种长度（含大量需转义字节）往返、`0xFA/0xFB/0xFE/
  0x0A` 转义往返、CRC 损坏丢弃并在下一帧重同步、计数器正确、>1517 B 溢出
  丢弃、`SerialCodec.encode == kdp_serial.frame_packet`、以太网 KDP 帧往返；
* 与 `kdp_proto.build_l2l3/parse_l2l3` 的逐字节一致性；
* 端到端 `eth` 与 `kdp` 两种 UDP 模式 × `serial`/`kdpf` 两种 CDC framing
  （共 4 组）：**未改动的** `Kdp_proto.KdpSession` 跑完
  `CONNECT → VERSION → HOSTINFO → READMEM64 → READPHYSMEM64 → DISCONNECT`；
  lldb 形状 `REATTACH → CONNECT → VERSION` 裸包流程 + 完整以太网帧透传。

另可单独跑真机日志回归（§4）：`replay_cdc_logs.py`。

---

## 7. MTU / 超时 / 统计 / 日志

* UDP 单 datagram 上限按 1518 B 与 CDC 一致（超限计数 `oversize` 并丢弃）；
  注意 serial 接收侧实际接受上限是 **1517 B** payload。
* `--cdc-idle-reset`（默认 1 s）仅对 `kdpf` 有效（它按声明长度判断半帧）。
* 统计按 framing 不同：`kdpf` 有
  `frames_tx/rx, bytes_tx/rx, crc_errors, bad_headers, oversize_drops, resyncs`；
  `serial` 有 `frames_tx/rx, bytes_tx/rx, crc_errors, overflow_drops`。
  另有 `udp_tx/rx, wrap, unwrap, passthrough, non_kdp, no_peer, serial_err`，
  可用 `--stats-interval 10` 定期打印，退出时自动打印一次。
* 日志带时间戳与级别；`--verbose` 打印每个帧的方向与长度。
* 优雅退出：Ctrl-C / SIGINT → 关闭 UDP socket 与串口、join 线程、打印统计。

---

## 8. 已知限制

* 未上板联调：真机日志回放已证明线上字节格式，但网桥尚未在实链路跑通过。
  USB CDC 单句柄、boot 后 70~90 s RX 失效、FS ~64 KB/s 等既有约束仍适用。
* **ARP**：设备侧 `kdp_arp_reply()`（`kdp_udp.c:875`）自己应答 ARP request；
  而 `kdp_arp_resolve()`（`:1062`）只在 `flag_panic_dump_in_progress` 为真时
  才发 ARP request 并等待。⇒ **交互式 KDP 不需要网桥处理 ARP**；**panic dump
  路径需要**（网桥目前不代理 ARP，属已知缺口）。
* `kdp` 模式的自动透传按结构嗅探（ethertype/proto/端口），极端构造的 KDP
  body 理论上可能误判；`--udp-mode eth` 无此问题。
* 设备若既不用 `0xFA` 也不用 `KDPF`（例如其它私有格式），网桥不适用；那种
  情况继续用 `kdp_cli.py --transport serial`。
* 不涉及任何对 `0x3c0fc000` 的读写。
