# nabu USB CDC bring-up (Xiaomi Pad 5 / SM8150)

把 Qualcomm DWC3 控制器做成一个 **USB CDC ACM 实时串口**，跑在移植到 Xiaomi Pad 5（nabu，
SM8150-AC）的 XNU 17.7.11（iPadOS 21H461）上。仓库记录了从 USB 电源/时钟/PHY 到 SMMU
bypass、再到主机枚举并读到内核日志的完整过程，含代码、补丁、取证工具与踩坑。

> Bring-up log and code for a USB CDC ACM console on a DWC3 controller under XNU 17.7.11
> on the Xiaomi Pad 5 (nabu, SM8150). The host sees a COM port and reads live kernel output.

## 结果（实测，2026-10-27）

| 项 | 值 |
|---|---|
| 主机枚举 | `USB\VID_1209&PID_316D\QCOMXNUUSBDEBUG` → `COM3`，boot 后约 25–32 s |
| 设备侧 | `init=1 phase=3 err=0`；`dr=0 ds=1 bank=0x37 le=3 lr=0x37`；`lt=8 tb=1`（DTR） |
| 主机读取 | 单轮捕获 10,806 B / 179 行内核日志，含 APFS 挂载与 launchd/dyld 的 AMFI 拒绝正文 |
| 复位 | 日志静止约 30 s 后复位，80 s 回到 recovery，PON `0x80041`。复位源是 **Mu 侧 APSS 硬件狗**，不是内核侧的狗（三档实测见 [docs/02](docs/02-bringup-log.md)） |
| 链路速率 | Full Speed（`DSTS.CONNECTSPD=1`），USB2 线缆/接口即可 |

## 2026-10-28：控制通道 + 硬件狗定时停车

串口从「只读日志」推进到「双向控制 + 无人值守刷机」，三项同时成立：

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
- 采集要 95 s 才够，但控制帧的 **RX 在 `boot+70~90 s` 后失效** ⇒ 必须让内核/硬件自己定时复位。
- 内核侧四种定时写法（`delay_for_interval`、`assert_wait_deadline` 线程、worker 轮询、写路径轮询）
  前三种都失败，最终落地为：**内核只决定"何时武装"，复位交给 APSS 硬件狗**（武装后约 32 s
  咬合，`BITE_MS` 改大无效）。细节与判据见 [docs/07-control-channel-and-wdt-park.md](docs/07-control-channel-and-wdt-park.md)。

主机侧新增两个工具：[`tools/port-session.py`](tools/port-session.py)（同一句柄既落日志又发帧）、
[`tools/pipeline.py`](tools/pipeline.py)（整轮编排）。

## 两个关键发现

1. **这块固件的 SMMU 不接受 `S2CR.TYPE=BYPASS`**：写入 `0x10003` 会读回 `0x20003`
   （TYPE 被重编码为 FAULT）。正确做法是把 stream 指向固件预留的 bypass context bank：
   `SCTLR=0 / CBAR.TYPE=S1_TRANS_S2_BYPASS / CBA2R.VA64=1`，S2CR 里写 **bank 号（TYPE=TRANSLATE）**。
   同板 UFS 与 sheng 移植都是这么做的，证据见 [docs/01-architecture.md](docs/01-architecture.md)。
2. **SM8150 上 S2CR/CBAR/SCTLR 的读回不保证等于写入值**，不能用“回读不符”判定失败；
   验收要看真实 DMA/枚举（事件环、主机 COM）。

## 目录

```
module/    QcomXnuUsbDebug 的本地副本（上游 MIT；我们的改动见 docs/module-changes.patch）
patches/   XNU 侧补丁（0005 console / 0006 worker / 0007 rebase + 诊断打印）
tools/     主机与设备侧取证脚本；`sxnu_ctl.py` / `port-session.py` / `pipeline.py` 为控制通道与整轮编排
docs/      架构、轮次记录、踩坑、原始证据、构建方法、dyld cache、控制通道与硬件狗定时停车
```

## 快速开始

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

引导参数：`qcom-usb-debug=1 qcom-usb-prepare=1`。

首次 clone 后给脚本加执行位：`chmod +x tools/*.sh`。

主机捕获（Windows，pyserial）：

```powershell
py -3 tools/cdc-capture.py --seconds 180 --raw com.bin --tsv com.tsv
```

详细步骤与验证命令见 [docs/05-build.md](docs/05-build.md)。

## 2026-11-16：19 天后的实际用法（补记）

上面 2026-10-27/28 的记录仍然准确，但**范围早已不够**。这一节是当前主线的真实用法。

### 量级已经换代

| 项 | 2026-10-27 记录 | 2026-11-16 实际 |
|---|---|---|
| 单轮捕获 | 10,806 B / 179 行 | **1–10 MB / 每轮** |
| 能看到的层 | 内核 console、APFS 挂载、AMFI 正文 | **完整 os_log**（解码后单轮 20 万行级） |
| 通道数 | 1 条（CDC） | **3 条**（CDC + NABUDIAG 双槽 + UFS 窗口） |
| 复位 | 日志静止 ~30 s | `pet=600` + `arm-delay=0`，双诊断槽 220/300 s |

### 三通道分工（重要）

| 通道 | 何时用 | 产出 |
|---|---|---|
| **DWC3 CDC** | 默认通道，每轮必用 | 内核 console 实时流，1–10 MB/轮 |
| **NABUDIAG 双槽** | 需要用户态 os_log | `seq=0`（220 s）+ `seq=1`（300 s），各约 30–38 MB，走 UFS 导出区 |
| **UFS 窗口 / NABULOG1** | **仅当 USB 功能死亡** | 见 [`unixnu-nabu-autolog`](https://github.com/Lux12138/unixnu-nabu-autolog) |

后两条是 CDC 的补充，不是替代：CDC 拿不到的东西（用户态 os_log、USB 死掉后的现场）才动用它们。

### 三通道覆盖矩阵（2026-11-16 实测，5 轮一致）

把三条通道的实际时间覆盖画出来，才能看清它们能拼到什么程度：

```
0s        50s      100s              286s   335s   380-410s
│          │         │                  │      │       │
│  CDC ████████░░░░██████████████████████████████░░░░░░░  内核 printf
│              os_log ████████████████████████░░░░░░░░░░  用户态 tracev3
│                                                  WDT咬点 ✗ 无人覆盖
```

各轮实测数字：

| 轮次 | CDC 覆盖 | CDC >10s 洞 | os_log 覆盖 |
|---|---|---|---|
| l739b | 7.9 – 269.2 s | 14 处 | — |
| l750 | 7.6 – 316.9 s | 20 处 | — |
| l769 | 7.3 – 323.8 s | 20 处 | 47.5 – 282.1 s（212,087 行） |
| l770 | 7.4 – 326.2 s | 21 处 | — |
| l771 | 7.2 – 334.1 s | 21 处 | 47.5 – 286.0 s（209,300 行） |
| l730 | — | — | 38.5 – 292.8 s（218,847 行） |

**能拼出来的：**

| 时间窗 | CDC | os_log | 结果 |
|---|---|---|---|
| 0 – 50 s | ✅ | ❌ | 仅内核 |
| **51 – 100 s** | ✅（**黑洞已撤回**，见下） | ✅ | 双视角对齐 |
| 100 – 286 s | ✅ | ✅ | **双视角对齐，最有价值的区间** |
| 286 – 335 s | ✅ | ❌ | 仅内核 |
| **335 – 410 s** | ❌ | ❌ | **真空，三条通道全空** |

### 两个必须知道的洞

**洞 ①【已撤回】「CDC 固定 48.6 秒黑洞」不是丢数据，是统计假象**

> **2026-11-18 逐轮复核结论：这一段从来不是空的。**
> 旧文说 `l750: 56.1 → 104.7 (缺 48.6s)` 等 5 轮一致，那只是**只看 `t=` 时间戳**得出的。
> 抽区间字节实测：
>
> | 轮次 | 区间 | 区间内字节 | 行数 | 区内 GPU 初始化行 | 首尾 launchd 墙钟 |
> |---|---|---|---|---|---|
> | l769 | 51.2→99.8 s | 97,070 B | 1,165 | 673 | Δ0.02 s |
> | l750 | 56.1→104.7 s | 88,702 B | 1,074 | 673 | Δ0.02 s |
> | l837-baseline | 43.4→93.6 s | 95,924 B | 1,158 | 673 | Δ0.02 s |
>
> 区间里 `NabuGPU: state=starting step=0..7` + `checkpoint=ready` **一条不少**（19 条），
> 即这 48 秒正是 GPU 在初始化并 ready。区间内容 = `CPmmio 331 / SMMU 293 / Metal 110 / BO 78 / GMU 43`，
> **这些行本来就不打 `t=`**。所以「两头有 t=、中间没有」= 中间没有 spawn、没有 Profile 采样，
> 不等于没有日志。launchd 墙钟首尾只差 0.02 s 证明这 95 KB 是同一瞬间的收尾刷屏。
>
> **教训：`t=` 有三套时钟源，不能混算** —— `nabu: spawn/exec` 是设备 uptime；
> `nabu-display` 是另一套（0..811e6）；`NativeAdrenoProfile: S` 又是第三套（1.3e8..3.29e8，只覆盖 6.9–17.2 s）。
> 混算会得出「t 逆序 43 次」「span 7660 秒」这种假象；`7.x → 141.x` 那条也是同一个错。
>
> **判「真的没数据」只看三条**：①`capture.log` / `-console.ctl.txt` 的累计字节数连续 N 秒一字不增；
> ②`drop (SerialException), reopen` 有没有打出来；③设备侧 `QCOM USB LOSS ... transport=` 是否为 0。
> l837 实测**只有 435→440 s 一次真实掉线 + 498 s 重连成功，全程 `transport=0`**。

**洞 ②：335 s → WDT 咬点（380–410 s）三条通道全空**

这是真正补不上的：

- CDC 在 334 s 前后断（USB 掉了或设备静默）
- os_log 快照 `diagAt1=300` 触发，内容只到 286–293 s
- NABULOG1 是滚动窗口，回 TWRP 后拉到的是最后 2 秒内的内容，也在此区间

**而这正好是最有价值的窗口**——panic 现场、最后一帧提交、挂死点都在这里。

已知的两条改进路径：

| 路径 | 状态 |
|---|---|
| `diagAt1` 300 → 360 | ⚠️ 未验证；l724（500）/l725（400）已被证明会早死，300 是实测的临界点 |
| 修 CDC 重连让它撑到 400 s | 🟡 **`tools/cdc-console.py` 已具备单句柄边采边控能力**（见下节），但「335 s 后为什么静默」仍未归因 |

### 控制台（2026-11-18 新增）

`tools/cdc-console.py` 把采集与控制合进**一个进程、一个句柄**（设备侧硬约束：
一个 boot 周期只能 open 一次），产出 `logs/<tag>-console.bin` + `logs/<tag>-console.ctl.txt`。

```bash
python -I tools/cdc-console.py --tag r900 --wait 600   # 开机前先起
# 进去后：hello / status / ping / screenshot / reboot --confirm / mark 文字 / mute 前缀 / stats / quit
python -I tools/cdc-console.py --self-test              # 不需要板子
```


### 判读时的实际用法

1. **先看 CDC** 拿内核时间线（0–335 s；**没有 48.6 s 黑洞**，那条已撤回，见洞 ①）
2. **需要用户态就去 os_log**（47–286 s），两者在 100–286 s 区间可以按 tick 对齐
3. **335 s 之后发生的事，当前无法判读**——不要用「CDC 里没看到」推断「没发生」，
   那段时间本来就没有通道
4. 若 `l*-cdc-merged.bin` 为 0 字节（如 l749），说明 USB 功能死亡，
   此时**只能**靠 UFS NABULOG1 窗口，见 `unixnu-nabu-autolog`

### 现在的标准采集流程

```text
recovery  → arm misc（读回校验 boot-recovery）→ adb reboot bootloader
fastboot  → fastboot flash cust payload.img → fastboot boot boot-merge-r3.img
kernel    → 跑；pet=600 / arm-delay=0 由 WDT 自动回收
recovery  → CDC watcher 已分段落盘 → 合并 → 拉 diag 双槽 → 拉 screen.bgrx
判读      → 解码 os_log（macul unifiedlog_iterator）→ 结构化写入 logs/rounds/<tag>.json
```

关键细节：

- **CDC watcher 会分段**：一轮可能产出 2–3 个 `cdc-*.bin`，必须**合并**再判读。只读第一段会得到截断结论——这个坑踩过（l727 的「LaunchServicesMigrator 卡死」就是 `seq=0` 截断造成的误判，实际它在 226.5 s 已完成）。
- **`screen.bgrx` 是判据，不是照片**：800×1280×4 = 4,096,016 B，比较 sha 与颜色直方图。纯黑 `112b94b6…`、锁屏 `ff868216…`、桌面 `d69…` 可一眼区分。
- **os_log 时间轴要换算**：macul 导出的 `time` 是 19.2 MHz ticks，`真实秒 = ticks / 19200000`。早期误当成秒会得出错误的时序结论。

### 判读纪律（血泪）

1. **`ncpy == xfer` 才是「CPU 全帧拷贝」的证据**，不是 `ncpy > 0`。l731 之后 `ncpy=0` 也不等于零拷贝——该结论已撤回，现在看 `P4/PF`。
2. **`PF/PX` 走 GPU7 标记流**，shipped kernel 的 `nabu_capture_append` 是空实现，**0 命中不能当失败判据**。
3. **`Migration not necessary` / `finishedStartup` / `Dismissing overlay`** 三个词才是系统 UI 交还的证据；`screen owner is now` 在交还后官方日志不用该格式，为 0 不代表失败。
4. **不要用「设备离线」推「系统卡死」**：WDT 会在 380–410 s 咬合，设备本来就会离线回 TWRP。

### 与 WDT 的关系（更正）

README 上文说「复位源是 Mu 侧 APSS 硬件狗」，这在 2026-10-27 当时成立。现在主线是：

```text
pet=600（内核侧 petSeconds 同长补丁）+ nabu-wdt-arm-delay=0
→ 咬点 380–410 s
→ PON 0x80041 自动回 TWRP
```

`arm-delay` 必须为 0 与 `sheng-smp=1` 配对（`precheck.py` 会硬性拒绝 `sheng-smp=1 + arm>0`）。

### 宿主机侧注意

- Git Bash 下 `adb pull` 要加 `MSYS_NO_PATHCONV=1`，否则 `/tmp/...` 会被改写成 Windows 路径。
- WSL 与 Windows 两套工具链分工固定：Windows Python 跑 `precheck.py`（硬编码 `E:/nabu/tmp`），WSL 跑 `audit_bootability` / `fsck.fat` / `mcopy` / macul。
- CDC 端口在设备复位后编号会变（COM3 → COM4…），watcher 按 `USB\VID_1209&PID_316D` 匹配而不是按端口号。

## 已知限制

- 链路是 Full Speed；未配置 USB3 SS PHY。
- 内核在 launchd/dyld 签名被拒后不再输出，约 31 s 后被复位源带走；串口窗口约 32 s。
- 没有 banner 行：初始化后的 backlog 从 `zones` 类早期文本开始。
- `init=1` 之后 `PE_kputc` 切到 USB，userdata 窗口不再更新；判卡点以串口为准。
- **panic 正文到不了串口**：`nabu_panic_restart()` 排在 `qcom_usb_debug_panic_drain()` 之前，
  panic 只能从设备屏幕上拍照读回；修顺序前不要指望串口里有 backtrace。
- **自动复位只来自 Mu 侧硬件狗**：同一停点上，`WDT.CFG mode=0` 实测 109 s 以上完全不回（要人工长按），
  `mode=1` 时 boot+60 s 自动回来。内核侧的 `nabu-hang-reset` **一次都没有 fire 过**，不要把它当兜底。
  PON 也无法区分复位源（硬件狗与内核 PSCI 都是 `0x80041`），唯一可靠的判据是 **COM3 有没有出现**。
- 捕获脚本以 `timeout` 结束时含义是"这一轮设备从未复位"，不是"脚本失败"。
- **一个 boot 周期只能打开一次 CDC**：`close()` 后再 `open()` 要么阻塞 120 s，要么
  `ERROR_GEN_FAILURE(31)`。多步操作（hello/status/reboot）必须放进同一进程、同一句柄。
- **控制帧的 RX 在 `boot+70~90 s` 之后失效**：同一内核 `boot+28 s` 与 `boot+69 s` 可发，
  `boot+88 s` 发出的帧完全没有 `[ctl] rx`。任何发送都要绑定"boot+第几秒"。
- **TX 会偶发停摆（约 1/3 轮次）**，固定停在 `IONVRAM sync` 之后、几十秒零新增字节。
  但**日志静默 ≠ 系统停住**：同一轮里 `startRoot()` 仍在跑、`misc=boot-recovery` 仍被写、
  板子仍能进 recovery。
- **`kprintf` 不会进 CDC**：诊断输出必须走 `qcom_usb_debug_write()` 或直写环。
- **`delay_us()` 拒绝 >10 ms**，并顺手把 `io_failed=1`（随后 phase 转 `IO_FAILED`、日志停发）。
- **APSS 狗咬合 = 武装后约 32 s**（`BARK=BITE=2097152 ticks` 实测；换算出的 64 s 不成立，
  寄存器语义未定），改 `NABU_WDT_BITE_MS` 无效；要更晚只能改"何时武装"。
  它的驱动在 `boot.img` 的固件卷里，不在 payload。

## 许可与来源

- `module/` 是 [xiaoleGun/QcomXnuUsbDebug](https://github.com/xiaoleGun/QcomXnuUsbDebug) 的副本
  （基线 `83be578`，文件头 SPDX: MIT），我们对它的改动以 patch 形式列在 `docs/module-changes.patch`。
- 其余代码与文档：MIT，见 [LICENSE](LICENSE)。
- 所有内容仅用于自有设备的移植研究。
