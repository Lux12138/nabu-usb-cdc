# KDP 调试器 over USB CDC（KDPF v1）

在**同一条 USB CDC 链路**上同时承载控制台和 KDP 远程调试器。
2026-11-24 真机验证通过：主机→设备方向的请求与回复**首次全线跑通**。

---

## 1. 一句话

设备侧把 KDP 的传输回调换成「裸以太网帧」，用固定 12 字节头（KDPF v1）打在
CDC 字节流上；主机侧一个 Python 网桥把它转成本地 UDP:41139，
`kdp_cli.py` 就能像连真机一样连上去。

```
kdp_cli.py --transport udp --host 127.0.0.1 --udp-port 41139 connect
        │  UDP 127.0.0.1:41139
        ▼
kdp_net_bridge.py --port COM3 --cdc-framing kdpf --udp-mode eth
        │  USB CDC 字节流（Windows 上单句柄独占）
        ▼
nabu 设备：QcomKdpNet.c（boot-arg nabu-kdp-net=1）
        │  kdp_register_send_receive(本模块的 tx/rx)
        ▼
XNU KDP 协议栈
```

---

## 2. 实测结果（2026-11-24 21:19，kb-165 内核）

```
[cli] CONNECT ok: error=NO_ERROR key=0x1337c0de seq=1
[cli] VERSION: remote_version=12 feature=0x1 bp_supported=True
[cli] HOSTINFO: cpus=1 cpus_mask=0x1 cpu_type=0x100000c cpu_subtype=0x1
max_bytes = 1024
Darwin Kernel Version 23.6.0: Tue Nov 24 05:41:11 CST 2026;
  root:ios-obj/DEVELOPMENT_ARM64_BCM2837; UUID=D896297D-BD52-3CD2-82AA-E8FADF14C0CF
BREAKPOINT64_SET 0xfffffff00729dc78: error=NO_ERROR
DISCONNECT: error=NO_ERROR
```

`readmem` 读到真实内核内存（自证语义正确）：

```
fffffff00c120008  30 2e bb ab f4 ff ff ff   -> 0xfffffff4abbb2e30   内核指针
fffffff00c120010  88 93 50 3e f8 ff ff ff   -> 0xfffffff83e509388   内核指针
fffffff00c120018  de c0 37 13               -> 0x1337c0de           session key
fffffff00c12002c  01 00 00 00               -> 1                     标志位
```

`0x1337c0de` 是 KDP 协议自己的 session key，读到它就说明整条读写链路语义正确。

**设备侧生效判据**（进 KDP 时的日志）：

```
ethernet MAC address: 02:42:ac:11:00:02
ip address: 10.0.0.2
```

且 **`Using serial KDP.` 出现 0 次** —— 说明 `kdp_register_send_receive()`
已被本模块的回调接管。

---

## 3. 线上格式（KDPF v1）

```
偏移  长度  字段
0     4    magic   = "KDPF"
4     1    version = 0x01
5     1    flags   = 0x00（保留，必须为 0）
6     2    length  = payload 长度，小端（1..1518）
8     4    crc32   = zlib CRC-32(payload)，小端
12    N    payload = 完整以太网帧（14 + 20 + 8 + KDP）
```

**无转义，固定 12 字节开销，上限 1518（= `KDP_MAXPACKET`）。**

payload 为什么天然是完整以太网帧：XNU 的 `kdp_send()`/`kdp_reply()`
无论传输是什么，都会先前置 Ethernet(14)+IPv4(20)+UDP(8) 再交给传输回调
（`osfmk/kdp/kdp_udp.c:544-687`）。

主机侧对应实现：`kdp_net_bridge/kdp_cdc_frame.py`（与设备侧逐字节对齐）。

---

## 4. 怎么用

### 4.1 设备侧前置条件

| 项 | 要求 | 说明 |
|---|---|---|
| boot-arg | **`nabu-kdp-net=1`** | 本模块开关；不加则完全走原 serial 路径，行为逐字节不变 |
| boot-arg | `debug=0x14e` | **不能带 `0x200`**（`DB_KDP_GETC_ENA` 会让 `kdp_getc()` 吃掉帧）|
| boot-arg | `nabu-cdc-rx=1` | CDC 接收后端门控 |
| 进入 KDP | panic / NMI / SXNU1 `DEBUGGER` 动词 | — |
| PC 侧 | 出现 **COM3** | 网桥要独占它 |

`board.toml` 里的片段：

```toml
    "nabu-cdc-rx=1",
    "nabu-kdp-net=1",
```

> **注意**：`board.toml` 的 boot-arg 不会自动进已打包的 payload。
> payload 里真正的 boot-args 在 `::/EFI/UniXNU/unixnu.toml` 的 `args = "..."`
> 这一行，两处都要改。

### 4.2 主机侧：两条命令

```bash
# 1) 起网桥（关掉即停）
cd kdp_net_bridge
python kdp_net_bridge.py --port COM3 --cdc-framing kdpf --udp-mode eth --verbose

# 2) 另开终端，连上去
cd kdp_client
python kdp_cli.py --transport udp --host 127.0.0.1 --udp-port 41139 connect
```

### 4.3 `--cdc-framing` 必须与设备侧模式匹配

**这是最容易犯的错，表现为「网桥明明在发帧，设备却零回应」。**

| 设备侧模式 | 正确参数 | 帧内层 |
|---|---|---|
| **本模块**（`nabu-kdp-net=1`）| **`--cdc-framing kdpf`** | 裸以太网帧 |
| 旧 serial KDP（`nabu-cdc-rx=1`）| `--cdc-framing serial` | `0xFA`/CRC/`0xFB` 封装 |

用错时网桥日志只有 `wrote NN B to CDC (serial frame)`，没有任何回包。

### 4.4 可用的 kdp_cli 命令

```
connect  reattach  disconnect  version  hostinfo  maxbytes  kernelversion
readmem  readphys  readregs  resume  suspend
bpadd  bpdel  bplist  step  selftest
```

**没有 `writemem`**（当前 CLI 不支持写内存）。

---

## 5. 已知限制

### 5.1 arm64 上 KDP 会话不能返回内核

`DISCONNECT` 只清会话（`kdp.is_halted = kdp.is_conn = FALSE`），
但 `kdp_debugger_loop()` 在 arm64 上无条件 `goto again`；
唯一出口 `exit_debugger_loop` 要求 `!panic_active()`，而 arm64 上
`debugger_is_panic` 恒为 TRUE ⇒ 该出口不可达。

**⇒ 每次 KDP 会话结束仍需断电重启。**

### 5.2 Windows 串口不能共享

实测：

```
CDC OPEN FAILED on COM3: win32 only supports exclusive access (not: False)
```

**⇒ 控制台和网桥物理上无法同时打开 COM3。**
切换时必须先停一个。仓库里的 `tools/kdp.sh` 封装了这个切换（见 §6）。

### 5.3 调试器态下 USB worker 被 halt

进入 KDP 后，`bsd_init.c` 里那个每 5 ms 轮询 USB 的 worker 线程停止。
**这就是为什么纯主机侧的网桥方案（只改主机、不动设备）行不通**：
端口能打开，但没有人读，设备不响应。

本模块在 `rx()` 里自己调用 `qcom_usb_debug_serial_poll()` 服务控制器，
所以端口在 KDP 等待期间保持可用。**这是本方案存在的根本理由。**

---

## 6. 一键脚本

`tools/kdp.sh` 封装了「停控制台 → 起网桥」的切换，避免手滑：

```bash
./kdp.sh status              # 谁占着 COM3、网桥状态、KDP 是否响应
./kdp.sh console [tag]       # 停网桥，起控制台
./kdp.sh bridge [framing]    # 停控制台，起网桥（默认 kdpf）
./kdp.sh stop                # 停掉所有可能占端口的 python
./kdp.sh cli <args...>       # 跑 kdp_cli（UDP）
```

---

## 7. 源码位置与构建

| 文件 | 内容 |
|---|---|
| `module/kernel/QcomKdpNet.c` | 新增，368 行：KDPF 成帧、route、tx、rx、统计 |
| `module/kernel/QcomXnuUsbDebug.c` | route 声明 + drain 钩子 + REBOOT 兜底 |
| `module/scripts/build.py` | `UNITS` 加入新文件；导入白名单加 5 个 KDP 符号 |

**导入白名单**（`build.py:129-135`）新增：`_kdp_register_send_receive`、
`_kdp_register_link`、`_kdp_set_ip_and_mac_addresses`、`_kdp_en_send_pkt`、
`_z_crc32`。不加会卡在打包审计（`RuntimeError: unexpected XNU imports`）。

**地址注入顺序不可颠倒**：先 `kdp_register_send_receive()` 再
`kdp_set_ip_and_mac_addresses()`（`kdp_udp.c:773-778` 有保护门）。

### 构建

```bash
cd module
REC=<xnu 树>/pexpert/DEVELOPMENT/pe_kprintf.o.json
rm -rf <out>          # build.py 要求输出目录不存在
python3 -I scripts/build.py --compile-record "$REC" --soc soc/Sm8150.h --output <out>
# 产物: <out>/libQcomXnuUsbDebug.a
```

内核链接的是固定路径 `/root/merge/l738-device/build/libQcomXnuUsbDebug.a`，
替换它再重编内核即可生效（不需要改任何构建脚本）。

---

## 8. 相关文档

| 文档 | 内容 |
|---|---|
| `state/bridge/answers/T57-kdp-net-ready-to-test.md` | 上板交接单与判据 |
| `state/bridge/answers/T38-getcena-interactive.md` | 为什么必须去掉 `0x200` |
| `state/bridge/answers/T39-kdp-exit.md` | arm64 为何不能返回内核 |
| `kdp_net_bridge/README.md` | 网桥用法与帧格式 |
| `kdp_client/README.md` | CLI 用法 |
