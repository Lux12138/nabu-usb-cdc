# nabu USB CDC bring-up (Xiaomi Pad 5 / SM8150)

把 Qualcomm DWC3 控制器做成一个 **USB CDC ACM 实时串口**，跑在移植到 Xiaomi Pad 5（nabu，
SM8150-AC）的 XNU 17.7.11（iPadOS 21H461）上。串口既能读内核日志，也能反向发控制帧。

> Bring-up log and code for a USB CDC ACM console on a DWC3 controller under XNU 17.7.11
> on the Xiaomi Pad 5 (nabu, SM8150). The host sees a COM port, reads live kernel output,
> and can send `SXNU1` control frames back down the same serial line.

---

# 当前状态（2026-11-19）

## 现在能做什么

| 能力 | 实测 |
|---|---|
| 读内核日志 | **连续到设备复位，无断流**。最长实测 l839-ctl 到设备时间 **593 s** |
| 写控制帧 | `HELLO` **8 ms** 往返，`control=1` 下发 session（l839 实测，设备时间 ~576 s） |
| 边采边控 | 同一进程、同一句柄同时收日志和发帧 —— **已验证同时有效** |
| 无人值守 | stdin 关掉也能发命令（命令文件通道），一轮零人工 |
| 每轮数据量 | 1–10 MB |

## CDC 一直在输出，两条历史上的「黑洞」都是测量侧错误

| 旧说法 | 真相 | 判据 |
|---|---|---|
| 「51–100 s 有 48.6 s 固定黑洞」 | **撤回**。那段是 GPU 收尾刷屏，**行不带 `t=`** | 区间内有 88–96 KB / 1,074–1,165 行，`NabuGPU step=0..7` + `checkpoint=ready` 全在，launchd 墙钟首尾只差 **0.02 s** |
| 「335 s 之后三条通道全空」 | **撤回**。旧依据那三轮脚本只跑到 316.9 / 323.8 / 334.1 s | l837 到 399 s（335 s 后 291 条）、l839 到 570 s（852 条）、l839-ctl 到 593 s（64 条） |

**最硬的一条证据**：l839-ctl 在设备时间 **575–593 s** 发 `HELLO`，8 ms 内收到回复，
而**同一条流里的日志当时还在往外走** —— 收和发共用一个句柄、同时有效，
已经越过所谓「截断线」240 秒。

## 仍然存在的真实盲区（不是 CDC）

| 通道 | 何时失效 | 状态 |
|---|---|---|
| **CDC（内核 printf）** | 到设备复位 | ✅ 连续，无黑洞 |
| os_log 快照 | **286–293 s**（`diagAt1=300` 是固有限制） | ❌ 之后没有 |
| NABULOG1 | 滚动窗口，约最后 2 s | ❌ 只有尾巴 |

**⇒ 300 秒之后仍然看不到用户态在干什么。缺的是 os_log 与 NABULOG1，不是 CDC。**

## 唯一的测量精度限制

没有 `t=` 的行无法精确归属时间。l839 里最大的真实 `t=` 间隔是 48.7 s（44.9→93.6）
和 26.5 s（17.1→43.7），但那两段**字节是完整的**，只是行不带时间戳。
要钉死只能给这些行加 `t=`（改内核），不是测量方法能解决的。

## 判「真的没数据」只用这三条

1. 采集侧**累计字节数连续 N 秒一字不增**（`capture.log` 的 `[stats]` 或 `-console.ctl.txt`）
2. `drop (SerialException), reopen` 有没有打出来
3. 设备侧 `QCOM USB LOSS overflow=/contention=/transport=` 里 **`transport` 是否为 0**

l837 实测：只有 435→440 s 一次真实掉线，498 s 重连成功，**全程 `transport=0`**。

**不要用 `t=` 的时间差判「有没有数据」** —— `t=` 有三套互不相干的时钟源：
`nabu: spawn/exec` 是设备 uptime；`nabu-display` 是另一套（0..811e6）；
`NativeAdrenoProfile: S` 又是第三套（1.3e8..3.29e8，只覆盖 6.9–17.2 s）。

---

# 控制台：`tools/cdc-console.py`

**一个进程、一个句柄，边落日志边发控制帧。**

设备侧硬约束：**一个 boot 周期只能 open 一次 CDC** —— `close()` 后再 `open()` 要么阻塞
约 120 s，要么直接 `ERROR_GEN_FAILURE(31)`，只有板子复位才能恢复。所以采集进程和控制
进程不能分开。控制台把 reader 线程和命令循环放进同一个进程，共用同一个 `Serial`。

```bash
python -I tools/cdc-console.py --tag r900 --wait 600   # 开机前先起
python -I tools/cdc-console.py --self-test             # 不需要板子
```

进入后的命令：

| 命令 | 作用 |
|---|---|
| `hello` | 建 session（HELLO 必须 session=0 seq=0）|
| `status` | `usb= er= rx= rxo= tx= bp= bo= poll= ev= gen= rej=` |
| `ping` | `rx/rxo/tx/poll` 快照 |
| `screenshot` / `shotnext` | 抓帧写 UFS |
| `reboot --confirm` | PSCI 复位（会掉 USB）|
| `mark <文字>` | 主机时间锚点，同时写进原始日志 |
| `mute / unmute <前缀>` | 只静音终端回显，**原始日志仍完整** |
| `stats` / `quit` | 本地统计 / 退出 |

产出 `logs/<tag>-console.bin`（原始字节）+ `logs/<tag>-console.ctl.txt`（TX/RX 与事件）。
`--wait N` 开机前先起；`--deadline N` 到点自退；`--drop PREFIX` 启动即静音；
**默认不重连**（掉线就报掉线，不偷偷换句柄）；stdin 关掉时自动转「只采集 + 读命令文件」。

---

# 控制帧协议 SXNU1

```
请求   SXNU1  <session:16hex> <seq:8hex> <VERB> <crc32:8hex>\n
回复   SXNU1R <session:16hex> <seq:8hex> <VERB> <status> [extras] <crc32:8hex>\n
```

- 行 ≤ **127 B**，帧（含 `\n`）≤ **128 B**
- **CRC-32/IEEE** 覆盖「行首到 CRC 前那个空格之前」的所有字节
- `HELLO` 必须 `session=0 && seq=0`；其它动作必须非零 session 且 `seq` **严格递进**
- CRC 不符 / 跳号 / 重放只加 `control_rejected`，**不回帧**
- 回帧带前置 `\n`，主机解析要跳过行首空行

| 动词 | 作用 | 回复 extras |
|---|---|---|
| `HELLO` | 建立会话 | `session=… last_seq=… control=1` |
| `PING` | 存活探针 | `rx= rxo= tx= poll=` |
| `STATUS` | 完整状态 | `usb= er= rx= rxo= tx= bp= bo= poll= ev= gen= rej=` |
| `REBOOT` | PSCI 复位 | `state=queued` |
| `SCREENSHOT` / `SHOTNEXT` | 抓帧 / 等下次 flip 再抓 | — |

`usb=3` = `QCOM_USB_DEBUG_ACTIVE`；`rej=0` = 没有一帧被拒。
协议实现唯一权威是 [`module/kernel/QcomUsbControl.h`](module/kernel/QcomUsbControl.h)（385 行，纯逻辑，无 I/O）。
完整端到端说明（含真机 TX/RX 原文）见 [docs/08-control-frames-end-to-end.md](docs/08-control-frames-end-to-end.md)。

## 真机实测（2026-11-18 22:11–22:12，l839）

```
22:11:59.874 TX SXNU1 0000000000000000 00000000 HELLO e5b60eb1
22:11:59.882 RX SXNU1R 1a0bba57bcab3195 00000000 HELLO OK session=1a0bba57bcab3195 last_seq=00000000 control=1

22:12:06.791 TX SXNU1 1a0bba57bcab3195 00000001 STATUS 51292187
22:12:16.797 ! no SXNU1R within 10s for STATUS

22:12:21.699 TX SXNU1 1a0bba57bcab3195 00000002 PING a5f0eddc
22:12:23.712 ! write failed: Write timeout

22:12:27.304 USB LOST
```

**`HELLO` 8 ms 往返；7 秒后 `STATUS` 无回复；再 15 秒 `PING` 连写都超时。**
当时板子正在画面抽搐、GPU 连续 7 帧 `complete=0`。
⇒ **失效与「系统还能不能响应」有关，不是「boot 后多少秒」**。
旧文「RX 在 `boot+70~90 s` 后关闭」是按时间点表述的，本次 592 秒发 `HELLO` 照样成功。

---

# 三条必须知道的硬约束

1. **一个 boot 周期只能打开一次 CDC**。`close()` 后再 `open()` 要么阻塞约 120 s，
   要么 `ERROR_GEN_FAILURE(31)`。多步操作必须放进同一进程、同一句柄。
2. **回帧带前置 `\n`**，解析要跳过行首空行。
3. **端口号会变**（COM3 → COM4，还可能复用）。按 **`VID_1209:PID_316D`** 匹配，不写死 COMx。

---

# 目录

```
module/    QcomXnuUsbDebug 的本地副本（上游 MIT；我们的改动见 docs/module-changes.patch）
patches/   XNU 侧补丁（0005 console / 0006 worker / 0007 rebase + 诊断打印 /
           0008 reboot-worker-and-wake）
tools/     主机侧工具
           sxnu_ctl.py      单次发一个动词
           port-session.py  同句柄边采集边发帧
           cdc-console.py   交互式控制台（单句柄 + 命令文件通道）
           cdc-capture.py   纯采集
           pipeline.py      整轮编排
docs/      01 架构 / 02 轮次记录 / 03 踩坑 / 04 原始证据 / 05 构建 /
           06 dyld cache / 07 控制通道与硬件狗定时停车 / 08 控制帧端到端
```

---

# 快速开始

模块（AArch64 静态库，编译参数取自内核真实编译记录）：

```sh
python3 scripts/build.py --compile-record <pe_kprintf.o.json> \
    --soc soc/Sm8150.h --output <tmp-dir>
```

内核侧按 `patches/0005 → 0006 → 0007` 应用，并在 defconfig 里加：

```
CONFIG_XNU_EXTRA_CFLAGS="... -DXNU_QCOM_USB_DEBUG=1 -I<module>/include"
CONFIG_XNU_EXTRA_LD_ARCHIVES="<build>/libQcomXnuUsbDebug.a"
```

引导参数：`qcom-usb-debug=1 qcom-usb-prepare=1 qcom-usb-ctl=1`。

首次 clone 后给脚本加执行位：`chmod +x tools/*.sh`。

主机捕获（Windows，pyserial）：

```powershell
py -3 tools/cdc-capture.py --seconds 180 --raw com.bin --tsv com.tsv
```

详细步骤与验证命令见 [docs/05-build.md](docs/05-build.md)。

---

# 两个关键发现

1. **这块固件的 SMMU 不接受 `S2CR.TYPE=BYPASS`**：写入 `0x10003` 会读回 `0x20003`
   （TYPE 被重编码为 FAULT）。正确做法是把 stream 指向固件预留的 bypass context bank：
   `SCTLR=0 / CBAR.TYPE=S1_TRANS_S2_BYPASS / CBA2R.VA64=1`，S2CR 里写 **bank 号（TYPE=TRANSLATE）**。
   同板 UFS 与 sheng 移植都是这么做的，证据见 [docs/01-architecture.md](docs/01-architecture.md)。
2. **SM8150 上 S2CR/CBAR/SCTLR 的读回不保证等于写入值**，不能用"回读不符"判定失败；
   验收要看真实 DMA/枚举（事件环、主机 COM）。

---

# 三通道分工与覆盖

| 通道 | 何时用 | 产出 |
|---|---|---|
| **DWC3 CDC** | 默认通道，每轮必用 | 内核 console 实时流，1–10 MB/轮，**连续到复位** |
| **NABUDIAG 双槽** | 需要用户态 os_log | `seq=0`（220 s）+ `seq=1`（300 s），各约 30–38 MB，走 UFS 导出区 |
| **UFS 窗口 / NABULOG1** | **仅当 USB 功能死亡** | 见 [`unixnu-nabu-autolog`](https://github.com/Lux12138/unixnu-nabu-autolog) |

```
0s        50s      100s              286s              593s+
│          │         │                  │                 │
│  CDC ███████████████████████████████████████████████████  内核 printf（无断流）
│              os_log ████████████████████░░░░░░░░░░░░░░░░  用户态 tracev3（286 s 止）
│                                        └ 之后只剩 CDC
```

| 时间窗 | CDC | os_log | 结果 |
|---|---|---|---|
| 0 – 50 s | ✅ | ❌ | 仅内核 |
| 51 – 100 s | ✅ | ✅ | 双视角对齐 |
| 100 – 286 s | ✅ | ✅ | **双视角对齐，最有价值的区间** |
| 286 – 593 s+ | ✅ | ❌ | 仅内核 |

## 判读时的实际用法

1. **先看 CDC** 拿内核时间线 —— 从 0 s 到设备复位，中间无断流
2. **需要用户态就去 os_log**（47–286 s），两者在 100–286 s 可对齐
3. **286 s 之后只有 CDC**，不要用「CDC 里没看到」推「没发生」—— 原因是**行可能不带 `t=`**，不是通道断了
4. 若 `l*-cdc-merged.bin` 为 0 字节（如 l749），说明 USB 功能死亡，
   此时**只能**靠 UFS NABULOG1 窗口

---

# 标准采集流程

```text
recovery  → arm misc（读回校验 boot-recovery）→ adb reboot bootloader
fastboot  → fastboot flash cust payload.img → fastboot boot boot-merge-r3.img
kernel    → 跑；pet=600 / arm-delay=0 由 WDT 自动回收
recovery  → CDC watcher 已分段落盘 → 合并 → 拉 diag 双槽 → 拉 screen.bgrx
判读      → 解码 os_log（macul unifiedlog_iterator）→ 结构化写入 logs/rounds/<tag>.json
```

关键细节：

- **CDC watcher 会分段**：一轮可能产出 2–3 个 `cdc-*.bin`，必须**合并**再判读。
  只读第一段会得到截断结论 —— l727 的「LaunchServicesMigrator 卡死」就是 `seq=0` 截断造成的误判，
  实际它在 226.5 s 已完成。
- **`screen.bgrx` 是判据，不是照片**：800×1280×4 = 4,096,016 B，比较 sha 与颜色直方图。
- **os_log 时间轴要换算**：macul 导出的 `time` 是 19.2 MHz ticks，`真实秒 = ticks / 19200000`。

## 判读纪律（血泪）

1. **`ncpy == xfer` 才是「CPU 全帧拷贝」的证据**，不是 `ncpy > 0`。
   `ncpy=0` 也不等于零拷贝 —— 现在看 `P4/PF`。
2. **`PF/PX` 走 GPU7 标记流**，shipped kernel 的 `nabu_capture_append` 是空实现，**0 命中不能当失败判据**。
3. **`Migration not necessary` / `finishedStartup` / `Dismissing overlay`** 三个词才是系统 UI 交还的证据。
4. **不要用「设备离线」推「系统卡死」**：WDT 会咬合，设备本来就会离线回 TWRP。

---

# 与 WDT 的关系

```text
pet=600（内核侧 petSeconds 同长补丁）+ nabu-wdt-arm-delay=0
→ 咬点 380–410 s
→ PON 0x80041 自动回 TWRP
```

`arm-delay` 必须为 0 与 `sheng-smp=1` 配对（`precheck.py` 会硬性拒绝 `sheng-smp=1 + arm>0`）。

**注意**：`pet=600` 来自**内核常量**（`NativeUfs.cpp` 里 `petSeconds` 的默认值被改成 600），
**不在 boot-args 里**。新编内核如果没打这个补丁，`NativeUfs: args` 行会是 `pet=300`。

---

# 宿主机侧注意

- Git Bash 下 `adb pull` 要加 `MSYS_NO_PATHCONV=1`，否则 `/tmp/...` 会被改写成 Windows 路径。
- WSL 与 Windows 两套工具链分工固定：Windows Python 跑 `precheck.py`（硬编码 `E:/nabu/tmp`），
  WSL 跑 `audit_bootability` / `fsck.fat` / `mcopy` / macul。
- **判进程死活不要用 `tasklist | grep`** —— 中文编码会吞掉 `python.exe` 那一行，
  看起来像进程没了。用 `powershell Get-CimInstance Win32_Process -Filter "name='python.exe'"`。

---

# 已知限制

- 链路是 Full Speed；未配置 USB3 SS PHY。
- 没有 banner 行：初始化后的 backlog 从 `zones` 类早期文本开始。
- `init=1` 之后 `PE_kputc` 切到 USB，userdata 窗口不再更新；判卡点以串口为准。
- **panic 正文到不了串口**：`nabu_panic_restart()` 排在 `qcom_usb_debug_panic_drain()` 之前，
  panic 只能从设备屏幕上拍照读回；修顺序前不要指望串口里有 backtrace。
- **自动复位只来自 Mu 侧硬件狗**：同一停点上，`WDT.CFG mode=0` 实测 109 s 以上完全不回（要人工长按），
  `mode=1` 时 boot+60 s 自动回来。内核侧的 `nabu-hang-reset` **一次都没有 fire 过**。
  PON 也无法区分复位源（硬件狗与内核 PSCI 都是 `0x80041`），唯一可靠的判据是 **COM3 有没有出现**。
- 捕获脚本以 `timeout` 结束时含义是"这一轮设备从未复位"，不是"脚本失败"。
- **TX 会偶发停摆（约 1/3 轮次）**，固定停在 `IONVRAM sync` 之后、几十秒零新增字节。
  但**日志静默 ≠ 系统停住**：同一轮里 `startRoot()` 仍在跑、`misc=boot-recovery` 仍被写。
- **`kprintf` 不会进 CDC**：诊断输出必须走 `qcom_usb_debug_write()` 或直写环。
- **`delay_us()` 拒绝 >10 ms**，并顺手把 `io_failed=1`（随后 phase 转 `IO_FAILED`、日志停发）。
- **APSS 狗咬合 = 武装后约 32 s**（`BARK=BITE=2097152 ticks` 实测；换算出的 64 s 不成立，
  寄存器语义未定），改 `NABU_WDT_BITE_MS` 无效；要更晚只能改"何时武装"。
  它的驱动在 `boot.img` 的固件卷里，不在 payload。
- **控制帧只在系统还能响应时可用**。同一句柄在画面抽搐前兆出现后数秒就会连 TX 都超时。
  不是固定时间点。

---

# 附录：历史记录（按时间倒序，含已撤回内容）

## 2026-11-16：19 天后的实际用法

量级已经换代：

| 项 | 2026-10-27 记录 | 2026-11-16 实际 |
|---|---|---|
| 单轮捕获 | 10,806 B / 179 行 | **1–10 MB / 每轮** |
| 能看到的层 | 内核 console、APFS 挂载、AMFI 正文 | **完整 os_log**（解码后单轮 20 万行级） |
| 通道数 | 1 条（CDC） | **3 条**（CDC + NABUDIAG 双槽 + UFS 窗口） |
| 复位 | 日志静止 ~30 s | `pet=600` + `arm-delay=0`，双诊断槽 220/300 s |

当时的覆盖矩阵（**其中「洞」的判读有误，见本文开头**）：

| 轮次 | CDC 覆盖 | CDC >10s 洞 | os_log 覆盖 |
|---|---|---|---|
| l739b | 7.9 – 269.2 s | 14 处 | — |
| l750 | 7.6 – 316.9 s | 20 处 | — |
| l769 | 7.3 – 323.8 s | 20 处 | 47.5 – 282.1 s（212,087 行）|
| l770 | 7.4 – 326.2 s | 21 处 | — |
| l771 | 7.2 – 334.1 s | 21 处 | 47.5 – 286.0 s（209,300 行）|
| l730 | — | — | 38.5 – 292.8 s（218,847 行）|

**注意上表 CDC 上限 269–334 s 是脚本窗口，不是设备输出上限。** 当时据此推出「335 s 后真空」，
是把「脚本停了」读成「系统不输了」。采得更长的轮次见本文开头。

### 当时的「洞 ①」原文（已撤回）

> ~~l739b: 64.3 → 112.9 (缺 48.7s)~~ / ~~l750: 56.1 → 104.7 (缺 48.6s)~~ /
> ~~l769: 51.2 → 99.8 (缺 48.6s)~~ / ~~l770: 51.7 → 101.9 (缺 50.2s)~~ /
> ~~l771: 51.2 → 99.9 (缺 48.6s)~~
>
> ~~成因是启动中段 USB re-enumeration……这段时间的 console 输出主机没收到，永久丢失。~~
>
> **错在哪**：只看 `t=` 差值。那几段是 GPU 收尾刷屏，行不带 `t=`，
> 区间内实际有 88–96 KB / 1,074–1,165 行，`NabuGPU step=0..7` 全在，launchd 墙钟首尾仅差 0.02 s。
> 「`7.x → 141.x` 跳变是分段拼接顺序错乱」也是同一个错：`t=` 混了三套时钟源。

### 当时的「洞 ②」原文（已撤回）

> ~~CDC 在 334 s 前后断（USB 掉了或设备静默）~~
>
> **这一条不成立**（见前）。但同节另外两条**仍然成立**：
> - os_log 快照 `diagAt1=300` 触发，内容只到 286–293 s ✅
> - NABULOG1 是滚动窗口，回 TWRP 后拉到的是最后 2 秒内的内容 ✅

| 改进路径 | 当时状态 | 现在 |
|---|---|---|
| `diagAt1` 300 → 360 | ⚠️ 未验证；l724（500）/l725（400）已被证明会早死，300 是实测临界点 | 未变 |
| CDC 撑到 400 s 以上 | 🟡 尚未做过 | ✅ **已达成**：l837 到 399 s、l839 到 570 s、l839-ctl 到 593 s，无断流 |

## 2026-10-28：控制通道 + 硬件狗定时停车

串口从「只读日志」推进到「双向控制 + 无人值守刷机」：

| 能力 | 实测 |
|---|---|
| 控制通道 | `HELLO` 返回非零 session；`STATUS OK usb=3 er=0 rej=0` |
| 串口 REBOOT | 命令被接受后**同秒 USB 消失**，`boot+12~15 s` 落 recovery |
| 硬件狗定时停车 | 内核在 `boot+~83` 武装 APSS WDT ⇒ **武装后约 32 s 咬合（boot+114，COM+86）** ⇒ `boot+129` 回 recovery |
| 完整启动日志 | **146,128 B / 2,031 行**（与自动复位同一轮拿到） |
| 无人值守一轮 | `reboot → TWRP → bootloader → 核 sha → flash cust → boot → 采集 → 验证`，**86 s / 轮，零人工** |

关键点：

- `REBOOT` 不能走 `reboot_kernel()`：本移植在 `vfs_unmountall` 阶段卡死（`launchd` 已退出、
  但 `halt_all_cpus` 永不执行），必须 arm 成功后**直接 PSCI `SYSTEM_RESET`**。
- 当时认为「采集要 95 s 才够，但控制帧的 ~~RX 在 `boot+70~90 s` 后失效~~」
  ⇒ 必须让内核/硬件自己定时复位。**「RX 70~90 s 失效」这条已按时间点表述失准**，
  实际取决于系统是否还能响应（见本文开头）。
- 内核侧四种定时写法（`delay_for_interval`、`assert_wait_deadline` 线程、worker 轮询、写路径轮询）
  前三种都失败，最终落地为：**内核只决定"何时武装"，复位交给 APSS 硬件狗**
  （武装后约 32 s 咬合，`BITE_MS` 改大无效）。
  细节与判据见 [docs/07-control-channel-and-wdt-park.md](docs/07-control-channel-and-wdt-park.md)。

主机侧工具：[`tools/port-session.py`](tools/port-session.py)（同一句柄既落日志又发帧）、
[`tools/pipeline.py`](tools/pipeline.py)（整轮编排）。

## 2026-10-27：首次枚举成功

| 项 | 值 |
|---|---|
| 主机枚举 | `USB\VID_1209&PID_316D\QCOMXNUUSBDEBUG` → `COM3`，boot 后约 25–32 s |
| 设备侧 | `init=1 phase=3 err=0`；`dr=0 ds=1 bank=0x37 le=3 lr=0x37`；`lt=8 tb=1`（DTR） |
| 主机读取 | 单轮捕获 10,806 B / 179 行内核日志，含 APFS 挂载与 launchd/dyld 的 AMFI 拒绝正文 |
| 复位 | 日志静止约 30 s 后复位，80 s 回到 recovery，PON `0x80041`。复位源是 **Mu 侧 APSS 硬件狗**，不是内核侧的狗（三档实测见 [docs/02](docs/02-bringup-log.md)） |
| 链路速率 | Full Speed（`DSTS.CONNECTSPD=1`），USB2 线缆/接口即可 |

---

# 许可与来源

- `module/` 是 [xiaoleGun/QcomXnuUsbDebug](https://github.com/xiaoleGun/QcomXnuUsbDebug) 的副本
  （基线 `83be578`，文件头 SPDX: MIT），我们对它的改动以 patch 形式列在 `docs/module-changes.patch`。
- 其余代码与文档：MIT，见 [LICENSE](LICENSE)。
- 所有内容仅用于自有设备的移植研究。
