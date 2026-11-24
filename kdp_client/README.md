# KDP-over-serial 客户端（XNU 17.7.11 / nabu）

一个纯 Python 的 Apple KDP 主机侧客户端，走 **串口（USB CDC）**，覆盖
`CONNECT / VERSION / HOSTINFO / READMEM64 / READPHYSMEM64 / READREGS /
RESUMECPUS / SUSPEND / BREAKPOINT64_SET / BREAKPOINT64_REMOVE / DISCONNECT`。

> **诚实标注：本客户端尚未上板验证。**
> 协议细节全部来自 XNU 17.7.11 源码直读（见文末「证据来源」），并用本地伪设备
> （`selftest.py`）做了 16 项离线回归。真机联调尚未进行，因此**不能保证**在
> nabu 上一定连通；下文「已知限制」列出了最可能先出问题的地方。

---

## 1. 文件结构

| 文件 | 作用 |
|---|---|
| `kdp_serial.py` | 串口收发 + `0xFA/CRC32/0xFB` 成帧/解帧 + 转义/反转义 + CRC 校验与重同步 |
| `kdp_proto.py` | Ethernet/IP/UDP 封装 + `kdp_hdr_t` 打包/解包 + 全部 verb + 分片/重发/异步 exception |
| `kdp_cli.py` | 命令行入口（子命令、会话持久化、十六进制/寄存器输出） |
| `selftest.py` | 本地回环自检（伪设备），不依赖真机 |
| `kdp_session.json` | 运行时生成，保存 `seq/key/端口`，用于跨调用续接会话 |
| `kdp_breakpoints.json` | 运行时生成，主机侧断点清单 |

依赖：Python 3.10+；真机需要 `pyserial`（`python -m pip install pyserial`）。
自检不需要 pyserial。

---

## 2. 协议要点（源码直读结论）

### 2.1 串口成帧（`osfmk/kdp/kdp_serial.c`）

```
帧 = 0xFA | escape(CRC32 小端 4B) | 0xFA | escape(包体) | 0xFB
转义集 = {0xFA, 0xFB, 0xFE, '\n'}      转义方式 = 0xFE 后跟 (~byte & 0xFF)
CRC = zlib 标准 CRC-32（z_crc32(0, data, len)）
```

**注意 CRC 字节序**：源码是 `crc>>0, >>8, >>16, >>24` 依次写出，即
**小端（little-endian）**，不是大端。四个 CRC 字节本身也要走转义。
（任务背景里写「大端」与源码不符，本实现按源码取小端；T26 报告同样记为小端。）

接收状态机与内核一致：`WAITSTART / READING / ESCAPED`；CRC 不符即整包丢弃并
在下一个 `0xFA` 处重同步；缓冲上限 1518 字节。

### 2.2 Ethernet + IP + UDP 封装（`osfmk/kdp/kdp_udp.c`）

串口 KDP **不是裸 KDP**：`kdp_send()/kdp_reply()` 无条件加 14B 以太网头 +
28B IP/UDP 头，`kdp_poll()` 强制校验 `ether_type=0x0800`、`ip.proto=17`、
`ip.ip_hl<=5`、`udp.dport=41139`。所以 **KDP 载荷在帧内偏移 42 字节**。

串口模式伪造地址：`MAC="serial"`、`IP=0xABADBABE`（`KDP_SERIAL_IPADDR`）。
内核用收到的第一个包的 **目的** MAC/IP 当自己的地址、**源** 当对端地址，
所以地址本身不被校验。默认值：

```
以太：dst=ff:ff:ff:ff:ff:ff  src="serial"  type=0x0800
IP  ：src=0.0.0.0            dst=0xABADBABE proto=17 ttl=60
UDP ：sport=41140(reply_port) dport=41139(KDP_REMOTE_PORT)
```

> 请求方向必须是 `dport=41139`（`kdp_poll` 会丢弃其它 dport）；内核回复方向是
> `sport=41139`。这一点搞反会表现为「连不上 / 内核静默丢包」。

### 2.3 KDP 包（`osfmk/kdp/kdp_protocol.h`）

```
kdp_hdr_t (8B, 小端位域)：
  byte0 : request(bit0..6) | is_reply(bit7)
  byte1 : seq
  byte2-3: len  （整包长度，含 8B 头）
  byte4-7: key  （会话 key）
```

`KDP_VERSION=12`，单包数据上限 `MAX_KDP_DATA_SIZE=1024`，单包总上限 1200。

### 2.4 序列号规则（最关键）

* KDP **只在内核 trap 进调试器之后**才 `kdp_connection_wait()` 等 `CONNECT`；
* **首个 `CONNECT` 的 `seq` 必须为 0**；
* 之后每个请求的 `seq` 必须等于内核的 `exception_seq`，每收到一个回复内核 `+1`；
* 重发「上一个 seq」会让内核重发它保存的回复 —— 这正是超时重发的依据；
* 收到异步 `KDP_EXCEPTION`（`is_reply=0`）必须先回一个
  `request=KDP_EXCEPTION, is_reply=1, seq=同一个` 的 ACK，内核才会 `+1`。

---

## 3. 设备侧前提（由测试端实现，此处仅列出）

1. **`board.toml` 的 `kernel_args` 加非零 `debug=`**。
   推荐 `debug=0x14e`（含 `DB_NMI=0x4`，让 panic/NMI 进 KDP；**不含 `DB_HALT=0x1`**，
   否则开机即停机等调试器）。
   `debug=0x14e = DB_NMI(0x4) | DB_KPRT(0x8) | DB_ARP(0x40) | 0x2 | 0x100(后两者已废弃)`。
   若只想最小行为变化，可用 `debug=0x8`（仅 `DB_KPRT`）或 `debug=0x4`。
2. **注册 `pe_serial_functions` 到 `gPESF`**，把 CDC（`qcom_usb_debug_putc` /
   RX tee ring）变成串口后端。`register_serial_functions()` 是 `static`，
   照 `patches/xnu/0001-nabu-fbcon.patch` 的先例在 `pe_serial.c` 内加实例 +
   在 `serial_init()` 顶部加探针；`has_irq=false`、IRQ 三回调 NULL、
   `device=SERIAL_NABU_CDC(0x40)`。注册必须早于 lockdown/KTRR 只读化。
3. **验证判据**：内核串口出现 `Initializing serial KDP` 与
   `Using serial KDP.`，且不再打印 `kdp_poll: no debugger device`。

---

## 4. 主机侧连接步骤

### 4.1 Windows（COM 口）

```powershell
cd E:\nabu\state\bridge\answers\kdp_client
python -m pip install pyserial
python kdp_cli.py --port COM21 connect
python kdp_cli.py --port COM21 version
python kdp_cli.py --port COM21 readmem 0xfffffe0012345000 64
```

COM 号用设备管理器或 `Get-CimInstance Win32_SerialPort` 确认。

### 4.2 WSL（/dev/ttyACM0）

WSL2 默认看不到 USB，需 `usbipd-win` 透传：

```powershell
winget install --interactive --exact dorsel.usbipd-win
usbipd list; usbipd bind --busid <busid>; usbipd attach --wsl --busid <busid>
```

然后 WSL 内：

```bash
cd /mnt/e/nabu/state/bridge/answers/kdp_client
python3 -m pip install pyserial
python3 kdp_cli.py --port /dev/ttyACM0 connect
```

（WSL 里若没有 pyserial，也可用 `socket://` 由 Windows 侧转发。）

### 4.3 连接时序

1. 让设备进入 KDP：触发 panic / NMI（`debug=0x14e` 下 panic 会进 KDP）。
2. 内核打印 `Waiting for remote debugger connection.`。
3. 主机执行 `connect`。成功判据：收到 `is_reply=1` 且 `error=0` 的 `CONNECT`
   回复，且 `VERSION`/`HOSTINFO` 有返回。

---

## 5. 命令清单

```
python kdp_cli.py --port <COMx|/dev/ttyACM0> <子命令> [参数]
```

| 子命令 | 说明 |
|---|---|
| `connect` | 发 `KDP_CONNECT`(seq=0)，打印 key，并查询 VERSION/HOSTINFO |
| `version` | `KDP_VERSION` → remote_version / feature / bp_supported |
| `hostinfo` | `KDP_HOSTINFO` → CPU 数 / cpu_type / cpu_subtype |
| `maxbytes` | `KDP_MAXBYTES` |
| `kernelversion` | `KDP_KERNELVERSION`（含 `stext=`，可取 KASLR slide） |
| `readmem <addr> <len>` | `KDP_READMEM64`（64 位虚拟地址，自动按 1024B 分片） |
| `readphys <addr> <len> [--cpu N]` | `KDP_READPHYSMEM64`（64 位物理地址） |
| `readregs [--cpu N] [--flavor 6]` | `KDP_READREGS`（AArch64 x0-x28/fp/lr/sp/pc/cpsr/flags） |
| `resume [--mask 0xffffffff]` | `KDP_RESUMECPUS` |
| `suspend` | `KDP_SUSPEND` |
| `bpadd <addr>` | `KDP_BREAKPOINT64_SET`（软件断点，最多 100 个） |
| `bpdel <addr>` | `KDP_BREAKPOINT64_REMOVE` |
| `bplist` | 列出主机侧记录的断点（无需连设备） |
| `disconnect` | `KDP_DISCONNECT` 并清会话 |
| `step` | **不支持**（见 §6.2），仅打印原因 |
| `selftest` | 跑本地回环自检 |

通用选项：`--baud`、`--timeout`（单次重传超时，默认 3s）、`--retries`（默认 8）、
`--connect-timeout`（默认 10s）、`--reply-port`（默认 41140）、
`--exception-port`（默认 41145）、`--key`（默认 `0x1337c0de`）、
`--fresh`（忽略持久会话，强制新 `CONNECT`）、`--verbose`。

`readmem/readphys` 默认十六进制 dump；`--format hex` 只打 hex；
`--out FILE` 写原始二进制。

### 会话持久化

内核的 `exception_seq` 在调试器循环里一直累加。为了让「一条条命令」能连续用，
CLI 把 `seq/key/端口` 存进 `kdp_session.json`，下次调用自动续用：
`connect` 总是新建会话（seq=0）；其它命令优先复用已存会话；`disconnect`
发送 `DISCONNECT` 并清空。`--fresh` 可强制走新 `CONNECT`。

---

## 6. 已知限制

### 6.1 未上板验证（最重要）

* 整个客户端**未在 nabu 真机上跑过**。协议/成帧/封装均按源码实现，
  并用伪设备做了 16 项离线回归，但 **USB CDC 实链路的字节边界、时序、
  吞吐、丢包** 都没验证过。
* CDC 是**单句柄**，boot 后约 70~90s RX 失效（既有结论），验证窗口要抓紧。
* USB FS 吞吐约 64 KB/s，内核 `kdp_poll` 超时 3ms；大块读取会多包 + 重传，
  实时性未实测。

### 6.2 `step` 不支持

XNU 17.7.11 的 `kdp_req_t` **没有 `KDP_SW_STEP`**（全树 0 命中；只有
`osfmk/arm64/sleh.c` 里与 KDP 无关的 ARM 软件单步异常）。所以 `step` 命令
只打印「不支持」。硬件单步只能自己驱动 ARM debug 寄存器，KDP 未暴露。

### 6.3 `WRITEPHYSMEM64` 在 arm64 是空实现

`ml/arm/kdp_vm.c` 里 `kdp_machine_phys_write()` 是 `return 0; /* unimplemented */`，
而 arm64 的 `KDPERR_ACCESS` 宏**恒返回 `KDPERR_NO_ERROR`**。所以写物理内存会
收到「成功」但一个字节都没写。本客户端**没有**提供 `writephys`；`readphys` 是
可用的（`bcopy_phys` 实现）。要写 PA 需先补内核侧实现。

### 6.4 会话失步

若上一次调用异常退出、内核仍停在「已连接」态，新的 `connect` 可能收到
`KDPERR_ALREADY_CONNECTED`（内核 `exception_seq` 未知，无法续接）。此时需
重新触发 KDP（panic/NMI）或重启。`--fresh` 不能解决这种状态。

### 6.5 其它

* `bplist` 是**主机侧**记录，内核没有「列出断点」的 verb。
* `KDPERR_ACCESS` 在 arm64 恒为 0，读内存失败可能表现为**空数据**而非错误码；
  客户端已按「空数据即失败」处理。
* 主机侧不解析符号表，符号化需自备（`KERNELVERSION` 的 `stext=` 可拿 slide）。
* `parse_l2l3` 只按 `dport/sport == 41139` 过滤，不校验 IP/UDP checksum
  （内核自己也不校验）。

---

## 7. 自检

```bash
python kdp_cli.py selftest
# 或
python selftest.py
```

16 项测试全部通过（本地伪设备，不需要真机）：

```
PASS  test_escape_roundtrip          转义/反转义覆盖全部 256 个字节值
PASS  test_frame_deframe_roundtrip   成帧/解帧（含 1200B 上限与 1518B 丢弃）
PASS  test_crc_mismatch_resync       CRC 错误整包丢弃并在下个 0xFA 重同步
PASS  test_resync_after_garbage      垃圾字节后重同步
PASS  test_l2l3_layout               Eth/IP/UDP 字段与 42 字节偏移
PASS  test_connect_and_seq           CONNECT(seq=0) → seq 严格递增
PASS  test_fragmented_read           >1024B 读取分片重组
PASS  test_readphys                  READPHYSMEM64
PASS  test_async_exception           异步 EXCEPTION 的 ACK 与 seq 推进
PASS  test_retransmit                超时重发同一 seq
PASS  test_readregs                  AArch64 寄存器解析
PASS  test_breakpoints               断点增删与错误码
PASS  test_disconnect                DISCONNECT 后会话复位
PASS  test_step_unsupported          step 明确报不支持
PASS  test_bad_sequence_dropped      错误 seq 被内核丢弃 → 超时
PASS  test_cli_end_to_end            真实 CLI 全流程（connect→readmem→disconnect）
```

---

## 8. 证据来源

XNU 源码（`/root/merge/l738-device/refer/xnu`，17.7.11）：

* `osfmk/kdp/kdp_serial.c` / `kdp_serial.h` —— 成帧、转义、CRC、状态机、1518B 缓冲
* `osfmk/kdp/kdp_protocol.h` —— `kdp_req_t`、`kdp_hdr_t`、各请求/回复结构、
  `KDP_REMOTE_PORT=41139`、`MAX_KDP_DATA_SIZE=1024`
* `osfmk/kdp/kdp_udp.c` —— `kdp_send/kdp_reply` 加 Eth+IP+UDP、`kdp_poll` 校验、
  `KDP_SERIAL_IPADDR=0xABADBABE`、`kdp_init` 伪造 `"serial"` MAC、
  `kdp_connection_wait/kdp_handler` 的 seq 规则、`kdp_send_exception/kdp_exception_ack`
* `osfmk/kdp/kdp.c` —— `KDP_VERSION 12`、分发表、connect/disconnect/readmem/readregs/
  breakpoint 实现与错误码
* `osfmk/kdp/ml/arm/kdp_machdep.c` —— `kdp_exception` 包格式、READREGS flavor、hostinfo
* `osfmk/kdp/ml/arm/kdp_vm.c` —— `kdp_machine_phys_read` 可用、`phys_write` 未实现
* `osfmk/mach/arm/_structs.h` —— `arm_thread_state64_t` 布局
* `osfmk/kern/debug.h` —— `DB_HALT/DB_NMI/...` 位定义

报告：`E:\nabu\state\bridge\answers\{T26-kdp-peserial,T29-lldb-kdp-remote,T19-dynamic-debug}.md`。
