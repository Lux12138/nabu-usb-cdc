# 控制帧（SXNU1）端到端：协议、实现、真机实测

**写于 2026-11-19**，基于 nabu（小米平板 5 / SM8150 / iPadOS 17.7.11 移植）上的实际跑通记录。
本文只讲“怎么在串口上双向收发控制帧”，不讲刷机流程。

---

## 一、一句话

**同一条 USB CDC 串口上，除了内核日志往下流，还能反向发 ASCII 控制帧进去。**
我们实测跑通了 `HELLO`，拿到了内核下发的 session：

```
TX  SXNU1 0000000000000000 00000000 HELLO e5b60eb1
RX  SXNU1R 1a0bba57bcab3195 00000000 HELLO OK session=1a0bba57bcab3195 last_seq=00000000 control=1
```

`control=1` 表示内核侧控制 worker 已启用，`session` 是它随机生成的会话号。

---

## 二、协议

### 2.1 帧格式

```
请求   SXNU1  <session:16hex> <seq:8hex> <VERB> <crc32:8hex>\n
回复   SXNU1R <session:16hex> <seq:8hex> <VERB> <status> [extras] <crc32:8hex>\n
```

- 行 ≤ **127 B**，帧（含 `\n`）≤ **128 B** —— 见 `QcomUsbControl.h:9-11`
- **CRC-32/IEEE** 覆盖「行首到 CRC 前那个空格之前」的所有字节
- `session` / `seq` 在 `VERB` **之前**

### 2.2 会话与序号规则（严格的）

| 规则 | 说明 |
|---|---|
| `HELLO` 必须 `session=0 && seq=0` | 唯一合法的一次性引导 |
| 其它动作用当前**非零** session | 否则拒绝 |
| `seq` 必须**严格递进** `last_seq + 1` | 跳号、重放都只加 `control_rejected`，**不回帧** |
| CRC 不符 | 同上，不回帧 |

实现见 `QcomUsbControl.h` 的 `sxnu_control_accept()` / `sxnu_control_commit()`。

### 2.3 动词表

| 动词 | 作用 | 回复 extras |
|---|---|---|
| `HELLO` | 建立会话 | `session=… last_seq=… control=1` |
| `PING` | 存活探针 | `rx= rxo= tx= poll=` |
| `STATUS` | 完整状态 | `usb= er= rx= rxo= tx= bp= bo= poll= ev= gen= rej=` |
| `REBOOT` | 让板子复位（走 PSCI） | `state=queued` |
| `SCREENSHOT` | 抓一帧写 UFS | — |
| `SHOTNEXT` | 等下一次 flip 再抓 | — |

`usb=3` = `QCOM_USB_DEBUG_ACTIVE`；`rej=0` = 没有一帧被拒。

---

## 三、内核侧实现

### 3.1 文件

| 文件 | 作用 | 行数 |
|---|---|---|
| `module/kernel/QcomUsbControl.h` | 纯协议层：解析、校验、回帧构造。**无 I/O、无内存分配、无任意寄存器操作** | 385 |
| `module/kernel/QcomXnuUsbDebug.c` | DWC3 驱动 + worker + 控制帧接入 | 1520 |
| `module/include/QcomXnuUsbDebug.h` | 对外头（含 `qcom_usb_debug_register_framebuffer`） | — |
| `module/patches/darwin22/0001..0002` | 打入 XNU 的补丁 | — |
| `patches/0005..0008` | 控制通道 / worker / rebase / reboot-worker | — |

### 3.2 关键函数（`QcomXnuUsbDebug.c`）

```c
static void control_service(const uint8_t *bytes, size_t count,
                            uint32_t generation,
                            const struct qcom_usb_debug_status *status)
{
    if (!__atomic_load_n(&control_enabled, __ATOMIC_ACQUIRE)) return;
    if (generation != usb_control_context.state.generation) {
        usb_control_context.state.generation = generation;
        control_rotate_session_owned();       /* ← 换 session，seq 归零 */
    }
    for (i = 0; i < count; i++) {
        int complete = sxnu_control_parser_feed(&parser, bytes[i], line, sizeof line, &line_bytes);
        if (complete > 0) {
            if (sxnu_control_parse(line, line_bytes, &frame))
                control_handle_frame(&frame, status, generation);
            else
                control_rejected++;           /* 不回帧 */
        } else if (complete < 0) {
            control_rejected++;               /* 超长行，丢到换行 */
        }
    }
}
```

**`control_rotate_session_owned()`** 在 USB transport generation 变化时调用 ——
这是为什么"同一个 boot 周期里重开串口后 HELLO 还能成功"：旧 session 失效、新 session 下发。

### 3.3 回帧是怎么塞进 TX 的

```c
static size_t control_reply_queue(const char *reply, size_t bytes)
{
    uint8_t wire[SXNU_CONTROL_REPLY_BYTES + 1];
    ...
    if (native.status.phase == QCOM_USB_DEBUG_ACTIVE) {
        backlog_drain_owned();
        if (DWC3_TX_BYTES - dwc3_queued(native.core) >= bytes + 1u) {
            wire[0] = '\n';                        /* ← 前置换行，把回帧与日志分开 */
            bytes_copy(wire + 1, reply, bytes);
            if (dwc3_queue(native.core, wire, bytes + 1) == bytes + 1) queued = 1;
        }
    }
    ...
}
```

**注意**：回帧会带一个前置 `\n`，所以主机侧解析要容忍行首空行。

### 3.4 REBOOT 为什么不能走 `reboot_kernel()`

本移植上 `vfs_unmountall` 会卡死（`/private/var` 是自建内存 FS，根卷只读 APFS，
本来没有要落盘的数据），而 `halt_all_cpus()` 才轮到 `PE_halt_restart`，
所以劫持钩子永远没被调用。

**修法**：arm 成功后跳过 `reboot_kernel()`，直接 `platform_smc_reset()`
（PSCI `SYSTEM_RESET`，`x0=0x84000009`，`smc #0`，之后 `wfe` 死等）。

---

## 四、主机侧实现

### 4.1 三个工具，三种用法

| 工具 | 用途 | 特点 |
|---|---|---|
| `tools/sxnu_ctl.py` | 单次发一个动词 | 最简单，会自动找 `VID_1209:PID_316D` |
| `tools/port-session.py` | **同句柄**边采集边发帧 | 带 reader 线程，不阻塞、可 Ctrl-C |
| `tools/cdc-console.py` | **交互式控制台** | 支持命令文件通道，无人值守也能发 |

### 4.2 硬约束：一个 boot 周期只能 open 一次

**这是设备侧的限制，不是主机侧 bug：**

```
close() 之后再 open()：
  - 要么阻塞约 120 s
  - 要么直接 ERROR_GEN_FAILURE(31)（SetCommState 失败）
只有板子复位才能恢复。
```

所以「先 hello 一次，另起进程发 reboot」这种做法会被打死。
**所有动词必须放进同一个进程、同一个句柄。**

`cdc-console.py` 就是为这个约束写的：reader 线程收字节、主线程发命令，共用一个 `Serial`。

### 4.3 最省事的调用

```python
import binascii
def crc32(b): return binascii.crc32(b) & 0xFFFFFFFF

def frame(session, seq, verb):
    body = "SXNU1 %016x %08x %s" % (session, seq, verb)
    return ("%s %08x\n" % (body, crc32(body.encode()))).encode()

# HELLO 必须 session=0 seq=0
ser.write(frame(0, 0, "HELLO"))
# 解析回复里的 session / last_seq，之后 seq 严格 +1
```

---

## 五、真机实测记录（2026-11-18 22:11–22:12）

**环境**：l839 轮（`hold/l814v2-rerun` 基线），桌面已起来，设备 uptime 约 560 s，
主机侧 `cdc-console.py` 通过命令文件通道发帧。

```
22:11:59.874 TX SXNU1 0000000000000000 00000000 HELLO e5b60eb1
22:11:59.882 RX SXNU1R 1a0bba57bcab3195 00000000 HELLO OK session=1a0bba57bcab3195 last_seq=00000000 control=1

22:12:06.791 TX SXNU1 1a0bba57bcab3195 00000001 STATUS 51292187
22:12:16.797 ! no SXNU1R within 10s for STATUS

22:12:21.699 TX SXNU1 1a0bba57bcab3195 00000002 PING a5f0eddc
22:12:23.712 ! write failed: Write timeout

22:12:27.304 USB LOST，控制台退出
```

**三条结论：**

1. **`HELLO` 8 ms 内回复**，`control=1` —— 控制通道端到端可用，这条是硬的
2. **7 秒后 `STATUS` 就没有回复**，再 15 秒 `PING` 连 **写** 都超时
3. **失效与系统是否还能响应有关，不是"boot+70~90 秒后必然失效"**

第 3 条是对旧结论的更正。旧文档（`docs/07-control-channel-and-wdt-park.md` §5.2）
写的是「控制帧 RX 在 `boot+70~90 s` 之后关闭」。本次实测：**592 秒发 `HELLO` 照样成功**，
但同一句柄在系统进入画面抽搐前兆后 7 秒就完全堵死。

⇒ **判据应该是「板子还活着吗」，不是「boot 后多少秒」。**

**当时板子的状态**（同一时间窗的日志）：

```
NABU TOUCH status t=8336514352 reads=21286 fingers=166 reports=173 latch=173
nabu-display: frames xfer=430 nswap=431 nok=430 ncpy=0 nret=431 fingers=354
NativeAdrenoProfile: S seq=103 ... complete=0     ← 连续 7 帧 GPU 完成信号丢失
QCOM USB LOSS overflow=0 contention=4960 transport=0
```

---

## 六、踩过的坑

| 坑 | 现象 | 正确做法 |
|---|---|---|
| **一个 boot 只能 open 一次** | 重开阻塞 120 s 或 `ERROR_GEN_FAILURE(31)` | 单进程单句柄，所有动词一次跑完 |
| **回帧带前置 `\n`** | 行首出现空行 | 解析时跳过空行 |
| **`kprintf` 进不了 CDC** | 板上大量 `kprintf` 一个字都看不到 | 诊断必须走 `qcom_usb_debug_write()` |
| **`delay_us()` 拒绝 >10 ms** | 顺手把 `io_failed=1`，之后日志停发 | 长延时用等待原语或硬件定时器 |
| **worker 循环会退出** | `phase != ACTIVE` 后 `break`，之后没人检查 | "到点做事"要么放写路径，要么交给硬件 |
| **Windows 端口号会变** | COM3 → COM4，还可能复用 | 按 **`VID_1209:PID_316D`** 匹配，不写死端口号 |
| **`thread_block()` 没有超时** | `assert_wait_deadline()` 返回 `THREAD_NOT_WAITING` 时直接 block 会永久睡死 | 必须检查返回值 |

---

## 七、最小复现步骤

```bash
# 1. 板子启动参数里要有：qcom-usb-debug=1 qcom-usb-ctl=1
#    （现基线 boot-args 已包含）

# 2. boot 前起采集/控制台
python -I tools/cdc-console.py --tag myrun --wait 900

# 3. 等它自己匹配到 VID_1209:PID_316D，然后在这个进程里发命令：
#      hello
#      status
#      ping
```

或用 `sxnu_ctl.py` 做一次性验证：

```bash
python tools/sxnu_ctl.py hello     # 自动找端口
python tools/sxnu_ctl.py status
```

**注意**：`sxnu_ctl.py` 每次运行都是新进程 = 新 open。
如果板子已经跑起来了，第二次调用大概率撞 120 s 阻塞/`ERROR_GEN_FAILURE(31)`。
**要发多个动词，用 `port-session.py` 或 `cdc-console.py`。**

---

## 八、文件清单

```
nabu-usb-cdc/
├── module/kernel/QcomUsbControl.h          ← 协议实现（385 行，纯逻辑）
├── module/kernel/QcomXnuUsbDebug.c         ← DWC3 驱动 + 控制接入（1520 行）
├── module/include/QcomXnuUsbDebug.h        ← 对外头
├── module/scripts/build.py                 ← 构建
├── module/scripts/collect-xnu-usb.py       ← 采集 + 控制 socket
├── module/scripts/xnu_usb_control.py       ← socket 客户端
├── module/scripts/screenshot.py            ← 截图
├── tools/sxnu_ctl.py                       ← 单次控制客户端
├── tools/port-session.py                   ← 单句柄会话
├── tools/cdc-capture.py                    ← 纯采集
├── docs/07-control-channel-and-wdt-park.md ← 控制通道 + WDT 停车
└── docs/01-architecture.md                 ← 整体架构
```

我们这一侧新加的：

```
E:/nabu/tools/cdc-console.py                ← 交互式控制台（单句柄 + 命令文件通道）
E:/nabu/logs/l839-ctl-console.ctl.txt       ← 上面那次真机实测的原始 TX/RX 记录
```
