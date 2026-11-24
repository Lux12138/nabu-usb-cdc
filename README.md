# nabu USB CDC 控制台

Xiaomi Pad 5（nabu / SM8150）上的 XNU 17.7.11 调试控制台。
主机看到一个 USB CDC 串口：实时读内核日志，同时反向发控制命令。

目标板：Xiaomi Pad 5 · SoC SM8150 · XNU 17.7.11 (iPadOS 21H461)
代码仓库：[Lux12138/nabu-usb-cdc](https://github.com/Lux12138/nabu-usb-cdc)

---

## 现在的状态（2026-11-24 23:50）

**48 个设备端命令 + 15 条主机侧命令 + KDP 网络链路 + 原生 lldb 直连，全部真机验证。**

| 指标 | 状态 |
|---|---|
| 设备端 verb | **48** 个（`module/kernel/QcomUsbControl.h`）|
| 主机侧命令 | **15** 条（读已导出数组，不需要内核改动）|
| 内核日志 | 开机到复位连续，无需第二串口 |
| 读内存 | 内核虚拟地址 + 物理地址/MMIO，带白名单防护 |
| **写内存/寄存器** | `WRITE` / `WRITEPA` / `SYSREGW` **三条都做过值变化验证** |
| panic 日志 | BSS 环 → UFS，`PANICSTR` 从介质读回，跨断电重启存活 |
| **`Debugger()` 进出** | **可进可出**（预置 ≥8 个 `'c'`，见下文）|
| **KDP 远程调试** | **`CONNECT`/`READMEM`/`BREAKPOINT`/`DISCONNECT` 全部跑通**（见 [`docs/KDP-NET-OVER-CDC.md`](docs/KDP-NET-OVER-CDC.md)）|
| **原生 lldb 直连** | **Linux 上 lldb 20.1.8 + Apple `kdp-remote`，已拿到符号化内核栈**（见下文）|
| 链路 | **USB High Speed**（`DSTS.CONNECTSPD=0`，两次 boot 复现）|
| CDC 吞吐 | **51.5 KB/s**（KDP 裸数据，`--poll 0.01`）；瓶颈 `DWC3_RX_BYTES=512` |

**最新可用基线**：`t134`（kernel `e68f924a4caae610`）。

### KDP 远程调试（新）

同一个 CDC 口，除了控制台还能跑 XNU 的 KDP 调试器：

```bash
tools/kdp.sh bridge            # 停控制台，起网桥（KDPF v1）
tools/kdp.sh cli --fresh connect
# CONNECT ok: error=NO_ERROR key=0x1337c0de seq=1
```

真机验证过：`CONNECT`、`VERSION`、`HOSTINFO`、`MAXBYTES`、`KERNELVERSION`、
`READMEM`、`BREAKPOINT64_SET/REMOVE`、`DISCONNECT`、断线重连。

**三个必须知道的前提：**

1. **`--cdc-framing` 必须匹配设备侧**：本模块（`nabu-kdp-net=1`）用 `kdpf`，
   旧 serial KDP 用 `serial`。用错时网桥在发帧、设备零回应，极难自查。
2. **`debug=` 不能带 `0x200`**（`DB_KDP_GETC_ENA` 会让 `kdp_getc()` 逐字节吃掉帧）。
3. **arm64 上 KDP 会话不能返回内核** —— 每次会话结束仍需断电重启。

完整用法、帧格式、构建步骤见 [`docs/KDP-NET-OVER-CDC.md`](docs/KDP-NET-OVER-CDC.md)。

> ### ⚠️ NOP18 是硬性步骤
>
> 构建出的 `kernel.macho` **必须**跑一次
> `python3 /e/nabu/tools/_nop18_l800.py`。它把签名 `6100005408214439` 后面的
> `cbnz`（`a8000037`）改成 `nop`（`1f2003d5`）。
>
> **没打这个补丁的内核会在 `disk1s1` 挂载阶段卡死，日志只出 6 行 `nabu:`。**
> 2026-11-23 一晚上有 6 轮构建因此白跑。
>
> 验证：
> ```
> python3 -c "d=open('kernel.macho','rb').read();i=d.find(bytes.fromhex('6100005408214439'));print(d[i+8:i+12].hex())"
> # 期望 1f2003d5
> ```

---

## 快速开始

### 1. 单元测试（可选）

```bash
cd module
clang -std=c11 -D_GNU_SOURCE -Wall -Wextra -I. \
  tests/test_usb_control_args.c -o /tmp/t && /tmp/t   # 应 ALL PASS
```

### 2. 打包（一条命令）

```bash
bash /e/nabu/tmp/pack_round.sh <新包名> [基线包名]
```

它会：**检查 NOP18** → 原地替换 kernel（不重排 FAT 簇）→ 校验三条 sha →
校验簇链布局 → 校验 boot 镜像 → 打印 args 数量。任一步失败就**不产出包**。

> **不要用 `mcopy` 直接替换 payload 里的 `kernel.macho`。** 那会重新分配
> 125 MB 的簇，FAT 布局全变。`payload_swap_kernel.py` 按原有簇链原地写入，
> 布局保持不变。

### 3. 上板（先起控制台，再刷机）

```bash
# 终端 1：控制台（务必在刷机前启动）
cd tools
python -u nabu_console.py --tag run1 --out /path/to/logs \
  --wait 300 --deadline 900 --kernel /path/to/kernel.macho

# 终端 2：刷机
bash go_round_next.sh <包名>
```

控制台每 2 秒扫描端口，发现 COM3 后打开，日志写入 `<tag>-com.bin`，
命令写入 `<tag>-cmd.txt`（**只追加，勿截断**）。

### 4. 发命令

向 `<tag>-cmd.txt` 追加一行即可：

```
hello
ptdump _gPhysBase
readpa 0x17c10008 16
sysreg TCR_EL1
readufs panic
vmstat
```

回复写入 `<tag>-ctl.txt`。

---

## 设备端命令（48 个 verb）

| 类别 | 命令 |
|---|---|
| 基础 7 | `HELLO` `PING` `STATUS` `BOOTARGS` `VERSION` `UPTIME` `REBOOT` |
| 内存读 7 | `READ` `READNEXT` `READPA` `READPANEXT` `RDBLK` `DUMP` `DUMPPA` |
| 内存写 2 | `WRITE` `WRITEPA` |
| UFS 3 | `DUMPUFS` `DUMLIST` `READUFS` |
| 调试 2 | `DEBUGGER` `KDPPUSH` |
| 页表 / panic 3 | `PTDUMP` `PANICDUMP` `PANICTRIG` |
| 系统 7 | `SYSCTL` `TASKDUMP` `THREADDUMP` `SYSREG` `SYSREGW` `SYM` `SYMS` |
| 显示 6 | `SCREENSHOT` `SHOTNEXT` `VSYNCON` `FLIPNOW` `DISPREGS` `SMMUDUMP` |
| GPU 6 | `GMUSTATE` `GPUFREQ` `GPUHWCG` `GPUPROFILE` `PERFSTART` `PERFSTOP` |
| PERF 2 | `PERFREAD` `PERFCLEAR` |
| 断点 3 | `BKPADD` `BKPDEL` `BKPLIST` |
| 诊断 2 | `STACKSHOT` `KDBG` |

`SYM`/`SYMS` 按设计回 `unsupported`（符号表 53k 条在主机侧解析）。

### 命令的安全阀

| 命令 | 条件 |
|---|---|
| `PANICTRIG` | 需要 boot-arg `nabu-panic-trig=1` **且** 字面 key `PANIC-NOW` |
| `SYSREGW` | 需要 `nabu-sysreg-write=1`，或运行时把 `qud_sysreg_write_enabled`（`.data` 导出，`0xfffffff00c0f5a70`）写成 1 |
| `WRITEPA` | 需要 `qcom-usb-write=1`，且地址在 MMIO 白名单内 |

**运行时开 gate 的例子**（不用刷机）：

```
read  0xfffffff00c0f5a70 4      → ffffffff   (-1 = 未解析)
write 0xfffffff00c0f5a70 01000000
read  0xfffffff00c0f5a70 4      → 01000000   (gate 已开)
```

---

## 主机侧命令（9 条，零内核改动）

这些不是 verb。它们用 `READ` 直接读**已导出的统计数组**，解析全在 Python 里做，
因此不需要重新编译内核。

| 命令 | 数据源 | 说明 |
|---|---|---|
| `VMSTAT` | `native_adreno_vm_phase_us[2][8]` | map/unmap 各阶段耗时 |
| `MEMSTAT` | `native_adreno_backing_cache_stats[12]` | backing cache 计数 |
| `POOLSTAT` | `native_adreno_pool_stats[8]` | 自有池统计 |
| `MEMSTAGE` | `native_adreno_memory_stage_stats[10][4]` | 10 个内存阶段 |
| `ALIASSTAT` | `nabu_alias_stats[6]` | alias 构建统计 |
| `METHODS` | `native_adreno_method_stats[20][4]` | 20 个 gpu method 槽 |
| `CLIENTS` | `native_adreno_client_stats[5][6]` | 5 个 client 槽 |
| `WAITOUTER` | `native_adreno_wait_outer[4]` | external method 等待计数 |
| `IB [n]` | `native_adreno_ib_log[4][32]` + `_count` | 最近提交的 IB 头 |

另有控制台自用命令：`VAR <sym>`（按符号名读）、`STATS`、`MARK`、`DUMP`/`DUMPPA`。

> **`READ` 的回复上限**：`SXNU_CONTROL_REPLY_BYTES = 512`，扣掉协议开销后
> `data=` 最多 **192 字节**。**`len=` 是请求长度，不是实际返回长度。**
> 超过 192 字节的数组由 `read_bytes()` 自动分块。

**`TSENS`** 已实现但**当前基线没有对应符号**（那次导出已回滚），会报"读取失败"。

---

## KDP 调试器：进得去也出得来

**两种场景，退出方式完全不同**：

| 场景 | 退出方式 | 为什么 |
|---|---|---|
| **未 CONNECT** | **预置的裸 `'c'`（≥8 个）** | 走 `kdp_getc()` 循环，事后发的字节要经 `serial_rx_drain()`，被 `acquire()` 的 `cpu_number() != master_cpu` 挡住 |
| **已 CONNECT** | **`kdp resume`**（`KDP_RESUMECPUS` 请求）| 走 KDP 协议，不经过 `kdp_getc()` |

### 进入 + 退出（一条命令）

```
kdp enter 6363636363636363
```

即 `DEBUGGER 6363636363636363`。模块回复会给出三个关键字段：

```
DEBUGGER OK state=entering queued=8 pending=8 ovf=0
```

`queued=8 pending=8 ovf=0` = 8 个字节全部进环、零丢失。

### 日志判据

**成功**（kb-94 实测）：

```
Using serial KDP.
Waiting for remote debugger connection.
Options.....    Type
------------    ----
continue....    'c'
reboot......    'r'
Continuing...                      ← 'c' 生效，Debugger() 返回
NabuGPU: state=starting step=0     ← 内核继续跑
```

**8 个是实测有效量**：2 个不够（kb-93 用 `6363` 失败）。

### 两个坑

1. **`kdp resume` 在未 CONNECT 时不生效** —— 它发的是裸字节，同样被 `acquire()` 挡住。
   控制台现在会直接报错并打印正确命令。
2. **`'r'` 走不通** —— `kdp_machine_reboot()` 传 `kPEPanicRestartCPU = 4`，
   而模块的 halt 钩子只认 `CONTROL_PE_RESTART_CPU = 1`，其余返回 `-1` 后空转。
   要让 `'r'` 通，只需让钩子也接受 `type == 4`（只改模块）。**尚未做。**

### panic 之后仍然出不来

panic 路径 `kdp_rx_armed = 0`，`serial_getc()` 读的是**控制环**（只有 SXNU1 文本），
而且我们无法在 panic 前预置字节。**只能断电**，除非改 XNU。

---

## ★ 原生 lldb 直连（Linux / macOS 主机）

`deliverables/lldb-kdp-linux-20261124/` 里是**在 Linux 上构建带 Apple `kdp-remote`
插件的 lldb**，可以直接连这台设备，拿到完整符号化内核栈。

**2026-11-24 23:48 已实测跑通。**

### 为什么可行

上游把 KDP 插件放在 `lldb/source/Plugins/Process/MacOSX-Kernel/`，父文件
`CMakeLists.txt` 只在 **Darwin** 分支里 `add_subdirectory`。插件本身**没有任何
平台门控**，依赖的模块在 Linux 都是活代码。把门控放宽到 `Linux|Android` 就能编。

另有一处 Apple 遗留 UB：`ProcessKDP.cpp` 把 UDP 连接 `static_cast` 成
`TCPSocket` 后调 `GetLocalPortNumber()`，macOS 靠内存布局恰好能用，Linux 上是 UB。

### 三条命令

```bash
# 1) Windows：起网桥（注意 kdpf + kdp 模式，不是 serial/eth）
cd kdp_net_bridge
python kdp_net_bridge.py --port COM3 --cdc-framing kdpf --udp-mode kdp --poll 0.01 --verbose

# 2) Linux：取 Windows 宿主 IP（WSL 里没有 ip 命令，resolv.conf 也不对）
awk '$2=="00000000"{print $3; exit}' /proc/net/route   # 01B015AC -> 172.21.176.1（小端倒读）

# 3) Linux：连
cd ~/nabu-lldb/build/bin
./lldb -o "target create <与板上一致的内核>" \
       -o "process connect --plugin kdp-remote udp://172.21.176.1:41139"
```

### 实测输出（符号化内核栈）

```
Process 1 stopped
* thread #1, stop reason = signal SIGSTOP
    frame #0: DebuggerTrapWithState(db_op=DBOP_DEBUGGER,
             db_message="qcom_usb_debug", ...) at debug.c:732:2
    frame #1: DebuggerWithContext(message="qcom_usb_debug") at debug.c:912:3
    frame #2: Debugger(message=<unavailable>) at debug.c:858:2
    frame #3: control_handle_frame(...) at QcomXnuUsbDebug.c:2420:9
```

`QcomXnuUsbDebug.c:2420` 正是控制台 `DEBUGGER` verb 里调
`Debugger("qcom_usb_debug")` 的那行 —— 栈和源码完全对上。

### 三个必踩的坑

| 坑 | 症状 |
|---|---|
| `--cdc-framing serial` | 网桥写了 N 字节，**设备零回应**。本板带 `nabu-kdp-net=1`，必须 `kdpf` |
| `--udp-mode eth` | lldb 发的是 8 B 裸 KDP 包，`eth` 模式当以太网帧处理 → 丢弃。必须 `kdp` |
| WSL 用 `nameserver` 当宿主 IP | `10.255.255.254` 是 DNS 代理，**UDP 打不通**。必须取 default gateway |

完整手册（11 节 + 排错表 + 真机联调记录）：
`deliverables/lldb-kdp-linux-20261124/T62-lldb-build-recipe.md`

---

## panic 日志（方案 D）

内核已有 4 MiB BSS 环；本仓库把它接到 UFS 预留区：

```
内核输出 → nabu_log_ring (BSS) → 每 2 秒落盘 → UFS userdata 末尾
```

panic 时 `nabu_log_land_now()` 强制落盘到**专用槽**，与周期槽分离：

| 槽 | 块偏移 | 内容 |
|---|---|---|
| 周期 | `rel 0` | `NABULOG1` 头（`is_panic=0`）|
| 周期 | `rel 1..25` | 最近日志 |
| panic | `rel 8190` | `NABULOG1` 头（`is_panic=1`）|
| panic | `rel 8166..8189` | panic 时刻的日志尾部 |

读回：

```
readufs header            # 周期槽头，is_panic 应为 0
readufs panic             # panic 槽头，is_panic 应为 1
readufs 8166@4032         # panic 文本（PANICSTR: ...）
```

`NABULOG1` 头布局（小端）：magic(8) + version(4) + is_panic(4) +
available(4) + dropped(4) + total_lo(4) + total_hi(4)。

方案 A（外部保留区 `0x9b000000`）已作废：两轮真机证明该地址无后备物理页。

---

## 重要操作纪律

1. **一个 tag 用到底**：`<tag>-cmd.txt` 只用 `>>` 追加，不截断。要重来就换 tag。
   （控制台按字节偏移追踪，截断后命令会被静默跳过。）
2. **一个 boot 只能 open 一次 CDC**：`close()` 后再 `open()`，即使返回成功、
   即使日志还在流，**OUT 方向也可能已失效**。活性判据只能是「HELLO 有回复」。
3. **先起控制台，再刷机**：否则早期日志全丢。
4. **复位判据**：COM3 消失 **且** adb 出现 `f91fa664 recovery`；`adb devices` 空不算。
5. **复位发 `reboot`**（走 PSCI，可靠）；**禁 `--arm-at` 自动复位**。
6. **改 boot-arg 必须重打 payload**，而重打会改 FAT 布局。能不改就不改；
   要开 gate，优先用运行时 `WRITE` 改对应的 `.data` 变量。
7. **写危险寄存器前先看原值**：像 `0xfffffff9...` 这种内核指针，写随机值会直接卡死内核。
8. **`READPA` 的目标地址必须有同 boot 的成功读记录**；不要用它探测未知寄存器
   （kb-72 用 `readpa 0x0105c000` 卡死过 worker，尽管那个地址在白名单内）。
9. **构建流程不要留 bind mount**：`out-v6/nabu` 现在是指向 `l738-full/out/nabu`
   的符号链接。历史上 91 层残留 mount 导致构建读到 11 月的旧快照。

更完整的纪律见 [`docs/DISCIPLINE.md`](docs/DISCIPLINE.md)。

---

## 目录

| 路径 | 内容 |
|---|---|
| `tools/nabu_console.py` | **唯一控制台**：日志采集 + 48 verb + 15 条主机侧命令 |
| **`tools/kdp.sh`** | **KDP 一键切换**：`status`/`console`/`bridge`/`stop`/`cli` |
| `tools/payload_swap_kernel.py` | 原地替换 payload 里的 kernel（保持 FAT 布局）|
| `tools/payload_layout_check.py` | 校验 kernel 簇链是否为已知可启动的布局 |
| `tools/pack_round.sh` | 打包入口（含 NOP18 与布局门）|
| `tools/build_and_pack.sh` | 构建 + 打包全流程 |
| `tools/pipeline.py` | 日志分析流水线 |
| `tools/symtab.py` | 主机侧符号表（53k 符号）|
| **`module/kernel/QcomKdpNet.c`** | **KDP over CDC 裸以太网帧（KDPF v1）** |
| `module/kernel/` | 控制协议与 verb 实现（C）|
| `module/tests/` | 命令解析单元测试 |
| **`kdp_net_bridge/`** | **主机网桥：CDC ↔ UDP:41139** |
| **`kdp_client/`** | **`kdp_cli.py`，走 UDP 或串口连 KDP** |
| `patches/` | 内核/驱动补丁 |
| `docs/KDP-NET-OVER-CDC.md` | **KDP 网络链路完整文档** |
| **`deliverables/lldb-kdp-linux-20261124/`** | **Linux 上构建带 `kdp-remote` 的 lldb**（补丁 + VERIFY + T62 手册 + 构建脚本）|
| **`deliverables/usb-sw-throughput-20261124/`** | **T59 软件侧提速补丁**（S1 多 TRB 环 + S2 事件预算）|
| `docs/` | 详细文档、历史记录、备份说明 |

---

## 备份

| 内容 | 位置 | 远端 |
|---|---|---|
| 源码 / 设备树活源 | `docs/backup/source/`（本仓库）| ✅ 已提交 |
| 编译产物 | 本地 `hold/_baseline_archive/20261123/` | ❌ 太大，不上传；见 [`docs/BACKUP.md`](docs/BACKUP.md) |

---

## 背景

- 项目起点：把 DWC3 USB 控制器做成 XNU 上的 CDC ACM 控制台，替代刷探针调试。
- 早期历史（已撤回的判断、黑洞争议等）见 [`docs/README-history-20261122.md`](docs/README-history-20261122.md)。
- 反编译侧协作记录见 `E:/nabu/state/SYNC-TO-RE-*.md`（本机文档）。

## 许可

见 [LICENSE](LICENSE)。`module/` 基于
[xiaoleGun/QcomXnuUsbDebug](https://github.com/xiaoleGun/QcomXnuUsbDebug)（MIT），改动见 `docs/module-changes.patch`。
部分分析参考 m1n1（GPL-2.0），仅吸收方法，未整文件拷贝。所有内容仅用于自有设备的移植研究。
