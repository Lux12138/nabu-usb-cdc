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
| 硬件狗定时停车 | 内核在 `boot+~83` 武装 APSS WDT ⇒ **boot+114（COM+86）咬合** ⇒ `boot+129` 回 recovery |
| 完整启动日志 | **146,128 B / 2,031 行**（与自动复位同一轮拿到） |
| 无人值守一轮 | `reboot → TWRP → bootloader → 核 sha → flash cust → boot → 采集 → 验证`，**86 s / 轮，零人工** |

关键点：

- `REBOOT` 不能走 `reboot_kernel()`：本移植在 `vfs_unmountall` 阶段卡死（`launchd` 已退出、
  但 `halt_all_cpus` 永不执行），必须 arm 成功后**直接 PSCI `SYSTEM_RESET`**。
- 采集要 95 s 才够，但控制帧的 **RX 在 `boot+70~90 s` 后失效** ⇒ 必须让内核/硬件自己定时复位。
- 内核侧四种定时写法（`delay_for_interval`、`assert_wait_deadline` 线程、worker 轮询、写路径轮询）
  前三种都失败，最终落地为：**内核只决定"何时武装"，复位交给 APSS 硬件狗**（咬合上限 ~64 s，
  `BITE_MS` 改大无效）。细节与判据见 [docs/07-control-channel-and-wdt-park.md](docs/07-control-channel-and-wdt-park.md)。

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
- **APSS 狗咬合上限 ~64 s**（21 位计数 × 32.768 kHz），改 `NABU_WDT_BITE_MS` 无效；
  要更晚只能改"何时武装"。它的驱动在 `boot.img` 的固件卷里，不在 payload。

## 许可与来源

- `module/` 是 [xiaoleGun/QcomXnuUsbDebug](https://github.com/xiaoleGun/QcomXnuUsbDebug) 的副本
  （基线 `83be578`，文件头 SPDX: MIT），我们对它的改动以 patch 形式列在 `docs/module-changes.patch`。
- 其余代码与文档：MIT，见 [LICENSE](LICENSE)。
- 所有内容仅用于自有设备的移植研究。
