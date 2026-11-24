# T63 · lldb / KDP 控制手册（nabu 专用）

- 日期：2026-11-24
- 读者：要用 Apple 原生调试器调试 nabu 内核的人。**不需要 Mac**,用 Linux/WSL + 我们自建的 lldb 即可。
- 前置:`T62-lldb-build-recipe.md`(把 lldb 编出来)、`T61-lldb-linux-deploy.md`(拓扑与验收)。
- 说明:本手册只写**已在本机验证过**的用法;凡需真机才能确认的命令,一律标注【待真机验证】。

---

## 1. 先理解这三者的关系

```
lldb(主机,Apple 官方调试器)
   └─ kdp-remote 插件  ── 标准 KDP-over-UDP :41139 ──┐
                                                     ▼
Windows 网桥(kdp_net_bridge.py, 独占 COM3)
   └─ USB CDC 字节流 ── nabu 设备(XNU,处于 KDP 等待态)
```

- **lldb** 是客户端;**KDP** 是协议;**我们的网桥** 负责把设备的串口字节流转成标准 UDP。
- 设备侧开关:`debug=0x14e`(**不能带 `0x200`**)、`nabu-cdc-rx=1`;进 KDP 的方式是 panic / NMI / SXNU1 `DEBUGGER` verb。

---

## 2. 一次性准备

```powershell
# Windows:起网桥(必须 --cdc-framing kdpf 和 --udp-mode kdp)
cd D:\_nabu-usb-cdc\kdp_net_bridge
python kdp_net_bridge.py --port COM3 --cdc-framing kdpf --udp-mode kdp --poll 0.01 --verbose
```

> **`--cdc-framing` 必须是 `kdpf`,不是 `serial`**(2026-11-24 实测)。
> 板上 `nabu-kdp-net=1` 生效后,`QcomKdpNet.c` 用 `kdp_register_send_receive()`
> 接管传输层,发的是 **KDPF v1 裸帧**(`"KDPF"|ver|flags|len|crc32|payload`),
> 没有 0xFA 封装。用 `serial` 时网桥把帧包进 `0xFA…CRC…0xFB`,设备侧
> `net_feed_byte` 找不到 `"KDPF"` magic → 静默丢弃;设备回的 `KDPF…` 也没有
> `0xFA`,deframer 永远等不到帧头 → 零回复。
> 现象是「网桥写了 N 字节,设备毫无反应」,极难自查。
> 判据:网桥 verbose 日志里必须出现 `wrote … (kdpf frame)`。
>
> **`--poll 0.01` 也不能省**:默认 0.1 s 的串口读超时占掉每次往返 90 ms,
> 实测 KDP 吞吐 8×16 KiB 从 14043 ms 降到 2544 ms(5.5 倍)。

Linux 侧编译好的 lldb 在 `~/nabu-lldb/build/bin/lldb`(见 T62)。

**符号**:把**与板上实际版本一致**的内核镜像作为目标文件。
当前在跑 `gpuw`(含 GPU READPA 白名单),`E:\nabu\hold\gpuw\kernel.macho`;
回退版 `p0-1` 在 `E:\nabu\hold\p0-1\kernel.macho`。
**给错版本,断点和变量地址会全部偏移。** 每次换包必须重新对 sha。
没有符号时 lldb 仍能读内存/寄存器/下断点,但看不到函数名和调用栈。

---

## 3. 连接:三步

```bash
# WSL2 里取 Windows 主机 IP —— 不能用 `ip route`,精简镜像常没有 iproute2;
# 也不能用 /etc/resolv.conf 的 nameserver(那是 WSL 的 DNS 代理,UDP 打不通)。
# /proc/net/route 的 Gateway 字段是小端十六进制,按字节倒读:
#   01B015AC  ->  172.21.176.1
GW_HEX=$(awk '$2=="00000000"{print $3; exit}' /proc/net/route)
HOSTIP=$(printf "%d.%d.%d.%d" 0x${GW_HEX:6:2} 0x${GW_HEX:4:2} 0x${GW_HEX:2:2} 0x${GW_HEX:0:2})
echo "Windows host = ${HOSTIP}"

cd ~/nabu-lldb/build/bin

# 1) 先建目标(可选,给符号)
./lldb -o "target create /path/to/kernel.macho"

# 2) 连 KDP
./lldb -o "process connect --plugin kdp-remote udp://${HOSTIP}:41139"

# 3) 进去之后
(lldb) process status
(lldb) register read
```

> **实测(2026-11-24 23:48)**:`172.21.176.1` 连通成功;
> `10.255.255.254`(resolv.conf 的 nameserver)的包网桥根本收不到。
> 判据:连上后网桥日志出现 `UDP->CDC KDP 8 B -> Ethernet frame 50 B`。

设备进入 KDP 后,串口日志会出现 `Waiting for remote debugger connection.`——**看到这句再连**。
连通判据:不再报 `KDP_REATTACH failed`,而是出现进程/线程上下文,`process status` 有输出。

### 3.1 实测连接输出(符号化内核栈,2026-11-24 23:48)

```
Process 1 stopped
* thread #1, stop reason = signal SIGSTOP
    frame #0: 0xfffffff0072dde40 kernel.macho`DebuggerTrapWithState(db_op=DBOP_DEBUGGER,
             db_message="qcom_usb_debug", ...) at debug.c:732:2
    frame #1: 0xfffffff0072de1bc kernel.macho`DebuggerWithContext(reason=0,
             message="qcom_usb_debug", ...) at debug.c:912:3
    frame #2: 0xfffffff0072ddf2c kernel.macho`Debugger(message=<unavailable>) at debug.c:858:2
    frame #3: 0xfffffff007ac3810 kernel.macho`control_handle_frame(...) at QcomXnuUsbDebug.c:2420:9
```

`QcomXnuUsbDebug.c:2420` 正是控制台 `DEBUGGER` verb 里调 `Debugger("qcom_usb_debug")`
的那行 —— 栈和源码完全对上,符号解析正确。

---

## 4. 常用命令速查

### 4.1 连接与状态
| 命令 | 用途 |
|---|---|
| `process connect --plugin kdp-remote udp://<ip>:41139` | 建立 KDP 会话 |
| `process status` | 看当前进程/线程与停止原因 |
| `process detach` / `process kill` | 断开 / 结束 |
| `process plugin packet send <req> [args...]` | **发送任意原始 KDP 请求**(插件自带命令,连上后才注册)【待真机验证】 |

### 4.2 执行控制(动态调试的核心)
| 命令 | 用途 |
|---|---|
| `breakpoint set -a 0xfffffe00xxxxxxxx` | 按地址下断点(KDP 支持最多 100 个,硬件断点) |
| `breakpoint set -n _some_symbol` | 按符号下断点(需要先加载内核镜像) |
| `breakpoint list` / `breakpoint delete N` | 管理断点 |
| `watchpoint set expression -- <addr>` | 观察点(数据变化时停) |
| `continue`(或 `c`) | 恢复运行 —— **⚠️ 在 arm64 上无效,见 §4.2.1** |
| `stepi` / `nexti` | 单步(指令级) |
| `thread list` / `thread select N` | 选线程 |

### 4.2.1 ⚠️ arm64 上 `continue` 不能真正恢复内核(实测确认)

**2026-11-24 实测:发 `continue` 后 lldb 立刻报 `Process 1 stopped`,PC 仍停在
`DebuggerTrapWithState` 的 `TRAP_DEBUGGER` 处,内核一步都没走。**

源码层面三层原因,缺一不可:

**(1) `kdp_resumecpus` 只清一个标志,不返回内核** —— `osfmk/kdp/kdp.c:407`:
```c
kdp.is_halted = FALSE;      /* 唯一动作,然后就回 reply */
```

**(2) `kdp_handler` 的循环退出后,外层立刻又跳回去** —— `kdp_udp.c:1478`:
```c
#if defined(__x86_64__)
	/* We only support returning from KDP on x86 */
	if (reattach_wait == 1)
#endif
	{
		goto again;      /* ← arm64 无条件跳回 again,出不去 */
	}

exit_debugger_loop:      /* ← arm64 永远到不了 */
```

**(3) 唯一的 `exit_debugger_loop` 出口要求 `!kdp.is_conn`** —— `kdp_udp.c:1418`:
```c
if ((kdp_flag & PANIC_CORE_ON_NMI) && !panic_active() && !kdp.is_conn) {
	kdp_panic_dump();
	if (!(kdp_flag & DBG_POST_CORE)) {
		goto exit_debugger_loop;
	}
}
```

**⇒ 一旦 CONNECT(`is_conn=1`),arm64 上就没有任何软件路径能让内核继续跑。**

**⇒ lldb 在本板是「冻结现场的法证工具」:读内存/寄存器/符号/栈、下断点、
写内存(`expr -- x = v` 实测可用)。它不是运行期采样器。**

**要运行期数据,用控制台的导出变量(内核跑着时能读);要精确现场,用 lldb 冻结。**

#### 想让 arm64 支持动态调试,需要改 XNU

改 `kdp_udp.c` 的出口条件,加一个「用户主动 resume」的标志:
```c
if (reattach_wait == 1 || kdp_user_resume) {
	kdp_user_resume = 0;
	goto again;
}
```
并在 `kdp_resumecpus` 里置该标志。**未做 —— 需要验证 `exit_debugger_loop` 的
`kdp_en_setmode(FALSE)` 链路清理后能否重新进入。**

#### 另一条已知的「不 CONNECT 也能进能出」路径

`debug` 含 `DB_KERN_DUMP_ON_NMI`(0x800)且**不含** `DB_DBG_POST_CORE`(0x1000)时,
`kdp_raise_exception` 会走 `kdp_panic_dump()` → `goto exit_debugger_loop`,
在 `!is_conn` 的前提下真能返回内核。
**但 `kdp_arp_resolve()` 会设 `DBG_POST_CORE`,一旦 ARP 收到调试包出口就被堵。**
历史上「能退出」很可能就是这条 —— 不稳定,但确实存在。

### 4.3 现场读取
| 命令 | 用途 |
|---|---|
| `register read` | 通用寄存器 + PC/CPSR |
| `register read x0 x1 sp pc` | 只看指定寄存器 |
| `memory read 0xfffffe00xxxxxxxx` | 读内存(虚拟地址) |
| `memory read --size 8 --format x --count 16 <addr>` | 指定格式批量读 |
| `image lookup -a <addr>` | 反查该地址属于哪个符号/kext(需符号) |
| `kext list`【待真机验证】 | Darwin 内核扩展列表(来自 DynamicLoaderDarwinKernel) |

### 4.4 物理内存(很有用)
KDP 有 `READPHYSMEM64`/`WRITEPHYSMEM64`,**绕过内核映射直接读写物理地址**。lldb 侧用 memory read 时注意:常规 `memory read` 走的是虚拟地址;若要读物理(比如 framebuffer、GPU 寄存器),需要在会话里选用对应的访问方式——可用 `process plugin packet send` 发原始请求,或用 lldb 的 `memory read` 配合内核已知映射。**具体写法【待真机验证】**,建议先用自研客户端 `kdp_cli.py readphys` 交叉验证同一个地址。

---

## 5. 四个典型会话模板

### 5.1 panic 取证
1. 触发 panic(NMI 或复现问题);
2. 设备打印 `Waiting for remote debugger connection.`;
3. `process connect ...` → `register read` → `bt`(调用栈,需符号) → `memory read` 看关键结构;
4. `process detach` 让设备继续走它自己的 panic 流程(主线的 `nabu-panic-restart`)。

### 5.2 断点定位「跑到哪一步出问题」
1. 加载内核镜像拿到符号;
2. `breakpoint set -n <疑似函数>`;
3. `continue` —— 让系统正常跑;
4. 命中后取现场,`continue` 继续;可反复。
> 这正是自研 verb 做不到的事:**不用改代码、不用重刷**。

### 5.3 读任意物理地址(framebuffer / GPU 寄存器)

**⚠️ lldb 侧读不到 MMIO(实测,见 §9.2)。** 走控制台的 `READPA`。

**GPU 寄存器窗口**(2026-11-25 实测可用):

```bash
# 前提:内核含 qud_read_whitelist 的 "gpu" 段(0x02c00000..0x02c40000),见下
readpa 0x02c00000 4      # chip_id      -> 0x00010053
readpa 0x02c00840 4      # RBBM_STATUS  -> 0x00000001 (bit0 = CP idle)
readpa 0x02c000e0 4      # RBBM_INT_0_MASK
readpa 0x02c00804 4      # RBBM_INT_0_STATUS
```

寄存器偏移来自 `a6xx.xml.h`(与 Linux `msm` 驱动一致):

| 寄存器 | byte offset | 绝对地址 |
|---|---|---|
| `REG_A6XX_RBBM_STATUS` | `0x210 * 4 = 0x840` | `0x02c00840` |
| `REG_A6XX_RBBM_INT_0_STATUS` | `0x201 * 4 = 0x804` | `0x02c00804` |
| `REG_A6XX_RBBM_INT_0_MASK` | `0x38 * 4 = 0x0E0` | `0x02c000e0` |
| `A6XX_RBBM_INT_0_MASK_CP_CACHE_FLUSH_TS`(fence) | bit 20 | `0x00100000` |

**该窗口默认不在 READPA 白名单里**(只有 gpucc `0x02c90000`)。
需要 `module/kernel/QcomUsbDiag.c` 的 `qud_read_whitelist` 加:
```c
{ UINT64_C(0x02c00000), UINT64_C(0x02c40000), "gpu" },
```
**已在 `hold/gpuw` 内核中包含。** 这是一行增量改动,与驱动走同一条
`ml_io_map` 路径,风险低于任何 PHY/时钟改动。

**注意窗口内有空洞**:`0x02c00100`/`0x02c000dc` 读回 `0xdeafbead`(未映射标记),
而 `0x02c00840`、`0x02c000e0`、`0x02c00000` 是真实寄存器。
**判断真值的方法:连读三次,值稳定的是真寄存器,随机变的是空洞。**

### 5.4 主动 dump
KDP 的 `DUMPINFO` 可请求 core/panic dump;**大文件取回不要走 CDC**(6 GB 要 27 小时),走 UFS 导出区(DUMPUFS)。lldb 侧可配合 `process plugin packet send` 触发请求【待真机验证】。

---

## 6. nabu 当前几个硬仗,分别怎么用 lldb

> **先读 §4.2.1。** arm64 上 `continue` 不能真正恢复内核,所以「下断点 → continue →
> 命中 → 采数据 → 再 continue」这个经典循环**在本板不成立**。
>
> **本板可用的用法是两种:**
> 1. **冻结现场取证** —— 进去之后读任意内存/寄存器/符号/栈。适合确认「某个值
>    到底是什么」「某个结构布局对不对」。
> 2. **写内存做实验** —— `expr -- x = v` 实测可用,能当场改一个变量看行为。
>
> 需要**运行期**数据时,回控制台读已导出的变量(内核跑着时能读),
> 或者用 `READPA` 读物理/MMIO(见 §5.3)。

| 硬仗 | lldb 能做的 | 怎么开始 |
|---|---|---|
| **GPU 单帧 149~335 ms stall** | 冻结现场读 `native_adreno_gpu_busy[8]`、`a6xx_stats[24]`、`gpu_job_now[8×6]`,和 `FRAMESTAT`/`GPUSTATE` 交叉验证 | 见 §6.1 |
| **vblank 双源竞争** | 读 `native_legacy_vsync_state[9]` 的事件计数 | `memory read -f u -c 9 <addr>` |
| **BO map/import 每帧 10.6 次** | 读 `profileMapCalls`/`profileMapUs` | 需要符号 |
| **RBBM 中断是否使能** | **`READPA 0x02c000e0 4` 读 `INT_0_MASK`,查 bit20** | 见 §5.3 |
| **panic/corefile** | 现场取证 + 触发 dump | 见 §5.1 / §5.4 |

### 6.1 实测:`INT_0_MASK` 是判断 fence 中断的关键读数

2026-11-25 在 `gpuw` 内核上实测(桌面已起、GPU 在工作):

```
readpa 0x02c00000 4   ->  0x00010053   (chip_id,GPU 在)
readpa 0x02c00840 4   ->  0x00000001   (RBBM_STATUS,bit0=CP idle)
readpa 0x02c000e0 4   ->  0xc2e3d013   (RBBM_INT_0_MASK,连读三次完全稳定)
```

`INT_0_MASK` 的解码(bit 定义与 `a6xx.xml.h` 一致):

| bit | 名称 | 状态 |
|---|---|---|
| 0 | CP_IDLE | **ON** |
| 1 | CP_AHB_ERROR | ON |
| 12 | CP_CCU_RESOLVE | ON |
| 14 | CP_IB1 | ON |
| 15 | CP_PM4 | ON |
| 16 | CP_PM4_LPAC | ON |
| 17 | CP_RB | ON |
| **20** | **CP_CACHE_FLUSH_TS(fence)** | **OFF** |
| 21 | CP_CACHE_FLUSH_TS_LPAC | ON |
| 22 | ATB_BUS_OVF | ON |
| 23 | RBBM_HANG | ON |
| 25 | UCHE_TRAP | ON |
| 4 | (未在 a6xx.xml.h 中定义) | **ON** |
| 30/31 | ISDB_CPU / ISDB_DEBUG | ON |

**⇒ 这是「CP fence 中断位没被使能」的第一个直接证据。**
驱动 `NativeAdrenoIrq::mask()` 对 A6xx 返回的集合**含** `IrqBitCpFence`(bit20),
但硬件读回来 bit20 是 OFF,且 bit4(驱动从不清的位)是 ON ——
**说明 `programMask()` 写进去的值和最终生效的值不一致,或者之后被别的路径覆盖。**

**下一步:连续多轮读 `0x02c000e0`,确认 bit20 是否在任何时刻被置起;
以及在 fence 刚写完的瞬间读 `0x02c00804`(INT_0_STATUS)看 bit20 会不会跳。**

---

## 7. 坑与排错

| 现象 | 处理 |
|---|---|
| `debug` 带了 `0x200` | `kdp_getc()` 会吃掉预置帧,必须去掉 |
| 连不上 / 超时 | 设备是否打印了 `Waiting for remote debugger connection.`;防火墙 UDP 41139;网桥必须是 `--udp-mode kdp` **且 `--cdc-framing kdpf`** |
| **网桥写了 N 字节但设备零回应** | **`--cdc-framing` 用了 `serial`。本板 `nabu-kdp-net=1` 必须 `kdpf`** |
| **KDP 通了但只有约 10 KB/s** | **缺 `--poll 0.01`。默认 0.1 的读超时吃掉 90 ms/往返** |
| **`continue` 后立刻 stopped,PC 不动** | **arm64 限制,不是配置问题。见 §4.2.1** |
| `KDP_REATTACH failed` 但有 `(plugin = kdp-remote, state = detached)` | 正常:插件工作正常,只是没服务端 |
| `invalid reply port` 类错误 | 用修正版 `processkdp-ub-fix.patch` 重编(见 T62 §7) |
| **`kdp read memory failed` 读 `0xfffffff8...`** | **IO 映射区,pmap 没后端。改用 `READPA`** |
| 长会话掉线 | CDC 单句柄 + boot 后 70–90 s RX 窗口,属既有约束;必要时重连 |
| 同一时刻别开两个客户端 | 一个 COM 口只能一个 owner;要并存请用 `kdp_mux.py`(TCP 41140 给控制台) |
| 改 boot-arg 后不生效 | 确认改的是 `/root/merge/l738-device/board.toml` **且同时改了 payload 里的 `unixnu.toml`** |

---

## 8. 速查(贴在手边)

```
# 连
./lldb -o "process connect --plugin kdp-remote udp://${HOSTIP}:41139"

# 动态调试循环
b   breakpoint set -a 0x...        # 下断点
c   continue                      # 跑起来
r   register read                  # 停下来看寄存器
m   memory read 0x...              # 读内存
bt                                 # 调用栈(需符号)
s   stepi                          # 单步
del breakpoint delete 1            # 删断点

# 原始 KDP
process plugin packet send <req> [args]      # 【待真机验证】
```

---

## 9. 验证结果(2026-11-24/25 实测)

原文标了三条「待真机验证」,现在全部有结论。

### 9.1 ✅ 符号镜像:`target create` 有效,栈能完整解析

```
target create /mnt/e/nabu/hold/gpuw/kernel.macho
process connect --plugin kdp-remote udp://172.21.176.1:41139
bt 5
```
输出四层符号化栈(frame #0 DebuggerTrapWithState → #3 control_handle_frame
at QcomXnuUsbDebug.c:2420)。`image lookup -a <addr>`、`image lookup -t A640State`
也都能用(返回 `byte-size = 1059640`),说明调试信息完整。

### 9.2 ❌ 物理内存:lldb 侧读不到

**(a) `memory read` 走虚拟地址,读不了 IO 映射区。**
实测 GPU 寄存器窗口 VA `0xfffffff8e2a85000`(由 `IOMemoryMap::fAddress` 推得):
```
memory read -f x 0xfffffff8e2a85000
  error: kdp read memory failed
```
同一次会话里,内核代码/数据段(`0xfffffff0...`)能正常读。
**⇒ `0xfffffff8...` 的 IO 映射区没有 pmap 后端,KDP 的 readmem 走不通。**

**(b) 物理地址要走自研客户端。** `kdp_cli.py readphys <pa> <len>` 可用,
但**受 READPA 等价的访问路径限制**;读 GPU 寄存器最终用的是控制台的
`READPA`(物理地址 + `ml_io_map`),那是和驱动同一条路。

**⇒ 结论:lldb 读物理/MMIO 不可行,用 `kdp_cli readphys` 或控制台 `READPA`。**

### 9.3 ✅ 版本不一致的表现:符号全错,极易误判

**实测过的教训**:重新刷机后 `A640State` 堆地址从 `0xfffffff76cd5a000`
变成 `0xfffffffbff57c000`,我用了旧地址去读,读出 `0x50000000` 之类的垃圾,
**误判成「GPU 关电」**。实际是源对象找错了。

**⚠️ 不只是「看不到函数名」那么简单 —— 错版本会让内存内容看起来完全合理但毫无意义。**

**正确做法**:
```
# 板上在跑哪个内核
kdp_cli.py kernelversion      # 输出里的编译时间就是判据
# 每次换包必须重新核对
sha256sum /e/nabu/hold/<tag>/kernel.macho | cut -c1-16
# 全局符号地址会变(数据段大小变了),必须重查
llvm-nm <kernel.macho> | grep <symbol>
```

### 9.4 补充:三个 lldb 能力边界(实测)

| 能力 | 结论 |
|---|---|
| `expr -l objc++ -- x = v` 写内存 | **可用**。实测 `control_rejected = 4242` 写入并读回 |
| `expr` 里调函数(`getVirtualAddress()`) | **不可用**。`Interpreter doesn't handle one of the expression's opcodes`(target 未在运行)。改读成员 `->fAddress` 可以 |
| `process continue` 让内核真跑 | **不可用**。见 §4.2.1 |

### 9.5 仍然待验证

- 【待真机验证】`process plugin packet send` —— 未试;
- 【待真机验证】`kext list` —— 未试;
- 【待真机验证】`watchpoint set expression` —— 未试;
- 【待真机验证】`stepi` / `nexti` —— 未试(受 §4.2.1 影响,可能同样无效)。
