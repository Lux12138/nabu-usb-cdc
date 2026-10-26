# 原始证据

## 内核窗口两行（userdata 尾部 `NABULOG1`）

r5（bypass-bank 第一次成功，COM3 出现 32 s）：

```
[   46.746926]: [usbdbg] init=1 phase=3 err=0 qc=0 iofail=0 diag=0 prep=0 stage=4 rb=00920001
[   46.746964]: [usbdbg2] dmapa=c7f50000 aw=64 dr=0 ds=1 asid=2a0406 bank=00000037 la=80000140 le=3 lr=37
```

r6b（加 `lt/tr/ta/tb`，主机逐步 probe 同轮）：

```
[   42.257859]: [usbdbg] init=1 phase=3 err=0 qc=0 iofail=0 diag=0 prep=0 stage=4 rb=00920001
[   42.257905]: [usbdbg2] dmapa=c7f50000 aw=64 dr=0 ds=1 asid=2a0406 bank=00000037 la=80000140 le=3 lr=37 lt=8 tr=39 ta=00000001 tb=00000001
```

r7（持续捕获 10,806 B）：

```
[   35.943735]: [usbdbg] init=1 phase=3 err=0 qc=0 iofail=0 diag=0 prep=0 stage=4 rb=00920001
[   35.943780]: [usbdbg2] dmapa=c7f50000 aw=64 dr=0 ds=1 asid=2e0406 bank=00000037 la=80000140 le=3 lr=37 lt=8 tr=31 ta=00000001 tb=00000001
```

对照（r4，identity lease 时代，TYPE 写不落地）：

```
[usbdbg2] dmapa=c7f4c000 aw=64 dr=17 ds=9 asid=1c bank=4c017e5e la=80000140 le=3 lr=20003
```

`asid` 是 CR0 的实测值；`lr` 在 bypass 路径是 S2CR 的读回（仅诊断），在 identity 路径是
写入后回读。`dmapa` 是运行时分配的物理地址，会漂移（`c7f4c000/c7f50000`），不要写死。

## 主机侧：Win32 逐步（r6b，01:46:22）

```
PnP: {"Status":"OK","Class":"Ports","Problem":0}
CreateFileW(\\.\COM3) -> handle=644 err=0
GetCommState -> 1 err=0 baud=115200 bytesize=8 parity=0 stop=0 flags=0x00001001
SetCommState(as returned) -> 1 err=0
SetCommState(115200 8N1 dtr/rts on) -> 1 err=0
SetupComm -> 1 err=0
PurgeComm -> 1 err=0
SetCommTimeouts -> 1 err=0
SetCommMask(0) -> 1 err=0
EscapeCommFunction(SETDTR) -> 1 err=0
EscapeCommFunction(SETRTS) -> 1 err=0
ReadFile -> 1 err=0 bytes=4096 elapsed=0.179s
pyserial(defaults) open ok; read bytes=3739 elapsed=1.013s
pyserial(115200) 在 01:46:55 失败：OSError(22, 指定不存在的设备) —— 此时 COM3 已消失
```

## r7 串口内容（10,806 B / 179 行）

开头：

```
compressor              : 0xfffffff834002000 - 0xfffffff98c019000 (5G)
zone_init: pgzalloc  0xfffffff234002000:0xfffffff234935000 (9M) [1177 slots]
VM bootstrap done: 39 maps, 112 entries and 115 holes left
OSLog stream configured: stream: 8192 bytes, cache: 2112 bytes
```

结尾：

```
apfs_vfsop_mountroot:2965: apfs: mountroot called!
dev_init:306: disk1 device_handle block size 4096 block count 1483776
nx_mount:1275: disk1 initializing cache w/hash_size 4096 and cache size 10064
apfs_log_op_with_proc:3091: disk1s1 mounting volume DawnUpdate21H461.J120J121J207J208OS
apfs_log_op_with_proc:3091: disk1s1 mount-complete volume DawnUpdate21H461.J120J121J207J208OS
is_root_hash_authentication_required:555: disk1s1 Root Volume, root hash authentication is required
nabu: owner stage=1 status=-1 mount=-1 cache=0 root=1 err=80 dev=16777219 sel=-1
nabu: owner SystemOS mount skipped (unverified root, err 80)
load_init_program: attempting to load /sbin/launchd
AMFI: '/sbin/launchd' is adhoc signed.
AMFI: '/sbin/launchd': unsuitable CT policy 0 for this platform/device, rejecting signature.
AMFI: code signature validation failed.
proc 1: load code signature error 4 for file "launchd"
AMFI: '/usr/lib/dyld' is adhoc signed.
AMFI: '/usr/lib/dyld': unsuitable CT policy 0 for this platform/device, rejecting signature.
proc 1: load code signature error 4 for file "dyld"
AMFI: hook..execve() killing pid 1: dyld signature cannot be verified.
```

最后一条之后 **31.7 s 没有任何字节**，随后端口断开（复位）。

这 31.7 s 里设备并非静默运行：同一位置的屏幕上有完整 panic 正文（见下一节）。
串口拿不到它，是因为 `nabu_panic_restart()` 在 `qcom_usb_debug_panic_drain()` 之前触发。

## 屏幕上的 panic 正文（merge-r2，官方基座 + USB）

同一停点（launchd/dyld 被 AMFI 拒）在屏幕上的原文：

```
unexpected SIGKILL of launchd  with reason -- namespace 9 code 0x8 description none
```

`namespace 9` = `OS_REASON_EXEC`（`bsd/sys/reason.h:96`），code `0x8` =
`EXEC_EXIT_REASON_SECURITY_POLICY`（`reason.h:255`）——exec 被安全策略拒绝，不是内存压力。

同轮主机侧行为：

- COM3 出现后再次打开报 `OSError [WinError 31]`（设备已断开、端口还在枚举表里），
  这是"设备侧已经走了"的信号，不是脚本 bug。
- 这一轮**没有自动复位**：官方基座的 FV 里没有 `NabuWdtDxe`，内核 20 s 狗在 panic 后停跑，
  最后由人工长按电源拉起，PON 读到 `0x80081`（冷启/长按），不是 `0x80041`。
- 捕获任务以 `timeout` 结束 = 设备在整个窗口内从未复位。

## dwc3 基座 vs 官方基座（同一轮次的复位差异）

| 基座 | FV | 复位源 | 挂死时是否自动回 recovery | PON |
|---|---|---|---|---|
| dwc3（r5–r10） | `30468b54…`，含 `NabuWdtDxe`（`NABU_WDT_MODE 1`，bite 240 s） | 内核 20 s 狗（CPU 还活着时） | 是，71–82 s | `0x80041` |
| 官方（merge-r1/r2） | `8bb58948…`，不含 `NabuWdtDxe` | 只有内核 20 s 狗 + panic hook | **否**，需长按 | `0x80081` |

## 时间线与复位

| 轮 | boot | COM3 出现 | COM3 消失 | 回 recovery | PON |
|---|---|---|---|---|---|
| r5 | 01:35:08 | 01:35:33 | 01:36:05 | 81 s | `0x80041` |
| r6 | 01:38:47 | 01:39:19 | 01:39:45 | 80 s | `0x80041` |
| r6b | ≈01:45:57 | 01:46:23 | 01:46:54 | 71 s | `0x80041` |
| r7 | ≈01:48:16 | 01:48:48 | 01:49:21 | 80 s | `0x80041` |

`seconds` 为 `fastboot boot` 到 recovery 检测到的秒数（含 TWRP 启动约 45 s）。
