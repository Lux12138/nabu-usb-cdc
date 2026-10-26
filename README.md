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
| 复位 | 日志静止 31.7 s 后由**内核 20 s 狗**（`nabu-hang-reset=20`）复位，82 s 回到 recovery，PON `0x80041` |
| 链路速率 | Full Speed（`DSTS.CONNECTSPD=1`），USB2 线缆/接口即可 |

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
tools/     主机与设备侧取证脚本（Windows / WSL）
docs/      架构、轮次记录、踩坑、原始证据、构建方法
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
- **自动复位依赖"内核还活着"**：上面那张结果表的复位来自内核 20 s 狗，它是一条普通内核线程，
  panic 或 CPU 挂死后不会跑。官方基座（不含 Mu 侧硬件狗）在内核挂死时**从不自动回 recovery**，
  只能长按电源。区别见 [docs/02-bringup-log.md](docs/02-bringup-log.md) 的"复位手段可用性"。
- 捕获脚本以 `timeout` 结束时含义是"这一轮设备从未复位"，不是"脚本失败"。

## 许可与来源

- `module/` 是 [xiaoleGun/QcomXnuUsbDebug](https://github.com/xiaoleGun/QcomXnuUsbDebug) 的副本
  （基线 `83be578`，文件头 SPDX: MIT），我们对它的改动以 patch 形式列在 `docs/module-changes.patch`。
- 其余代码与文档：MIT，见 [LICENSE](LICENSE)。
- 所有内容仅用于自有设备的移植研究。
