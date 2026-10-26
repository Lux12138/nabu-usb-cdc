# 构建与上板步骤

以下命令以 UniXNU 设备仓库为例，路径用占位符替换。

## 1. 模块

需要一份与目标内核同参数的编译记录（`pe_kprintf.o.json`，由内核构建产生）：

```sh
cd module
python3 scripts/build.py \
    --compile-record <tree>/build/xnu/ios-obj/.../pexpert/DEVELOPMENT/pe_kprintf.o.json \
    --soc soc/Sm8150.h \
    --output <tmp>/build-qcom-usb
```

产出 `<tmp>/build-qcom-usb/libQcomXnuUsbDebug.a` 与 `build-report.json`。检查：

```sh
grep -E '"soc"|"successful_archive"|"identity_lease"' build-report.json
llvm-nm --defined-only libQcomXnuUsbDebug.a | grep -E 'QcomUsbBypass(Plan|Enter|Restore)'
```

把归档放到设备仓库的链接路径（示例 `build/libQcomXnuUsbDebug.a`）。
**归档必须比所有模块源码新**（`find module -newer lib.a` 为空）。

## 2. 内核

应用 `patches/0005-nabu-usbdebug-console.patch`、`0006-…worker.patch`、
`0007-…rebased.patch`（按序；0007 含 `[usbdbg]` / `[usbdbg2]` 诊断打印）。

`nabu_defconfig`：

```
CONFIG_XNU_EXTRA_CFLAGS="... -DXNU_QCOM_USB_DEBUG=1 -I<module>/include"
CONFIG_XNU_EXTRA_LD_ARCHIVES="<build>/libQcomXnuUsbDebug.a"
```

boot args：`qcom-usb-debug=1 qcom-usb-prepare=1`（不要带 `qcom-usb-diag`）。

构建：

```sh
unixnu build kernel -D <device-repo> --out <out>
unixnu build payload -D <device-repo> --out <out> --uefi <uefi-repo> \
    --unique-chip-id <chip-id>          # 该值不要提交到仓库
unixnu bootimg -D <device-repo> --out <out> --uefi <uefi-repo> \
    --dtb <device-repo>/dts/nabu-full.dtb --ramdisk none -o boot.img
```

如果内核编译报 `TrustCache/API.h` 缺失，从同版本树的
`.cache/xnu-work/EXTERNAL_HEADERS/TrustCache` 拷回后再编。

## 3. 制品核对（上板前）

```sh
strings -a kernel.macho | grep 'lt=%u tr=%u ta=%08x tb=%08x'
llvm-nm <tree>/.../mach.*.unstripped.noctf | grep QcomUsbBypass
mcopy -n -i payload.img ::/EFI/UNIXNU/PAYLOADS/kernel.macho /tmp/k.mac
sha256sum /tmp/k.mac            # 应等于 payload.json 的 payload_kernel_sha256
```

## 4. 上板

先 arm misc 并读回，再同一 fastboot 会话里刷 cust、启动：

```sh
adb push misc-bootrecovery.bin /tmp/m0.bin
adb shell 'dd if=/tmp/m0.bin of=/dev/block/by-name/misc bs=4096 count=1'
adb shell 'dd if=/dev/block/by-name/misc bs=16 count=1 | od -An -tc'   # 必须是 boot-recovery
adb reboot bootloader
fastboot flash cust payload.img
fastboot boot boot.img
```

等待用 10 s 轮询（`tools/nabu-cycle.sh`），上限 300 s。回滚线（任一成立即停）：

- 300 s 未回 recovery；
- PON = `0x800c1`（长按硬复位）；
- 屏幕/窗口/串口全无输出。

只刷 cust，不刷 boot；`fastboot boot` 不落盘。

## 5. 主机捕获

Windows（pyserial）：

```powershell
py -3 tools/cdc-capture.py --seconds 180 --raw com.bin --tsv com.tsv
```

行为：2 s 轮询等 `VID_1209/PID_316D`；注册后 0.5 s 再打开；失败重试 5×1 s；
150 ms 超时持续读到端口消失；raw/tsv/事件日志三份落盘。

逐步诊断（Win32 `GetLastError` + pyserial 两次）：

```powershell
py -3 tools/cdc-probe.py --wait 180 --out probe.txt --raw probe.bin
```

`tools/nabu-cycle.sh`（WSL）把 arm→fastboot→boot→等回→取窗口串成一轮；
`tools/comwatch.ps1` 记录主机 COM 时间线；`tools/usbinfo.ps1` 查 USB 拓扑。

## 6. 判读

- `init=1` 且主机出现 COM3 → 链路通。
- 读不到内容时先看 `lt`：`8`=DTR 已到；`6`=有请求被 STALL（`ta` 高 16 位是
  `bmRequestType+bRequest`，如 `0x2120`）；`tr` 很小=几乎没收到 SETUP。
- 日志静止但端口还在 → 内核没再输出（本例是 launchd 被拒后）；端口消失 → 复位。
