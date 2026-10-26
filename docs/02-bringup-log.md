# 上板轮次记录（全部实测）

设备：Xiaomi Pad 5（nabu），12+512，cust payload + `fastboot boot` 加载 Mu/XNU。
每轮先 arm misc（`boot-recovery`）并读回，再 `fastboot flash cust`、`fastboot boot`，
10 s 轮询等回 recovery。

| 轮 | payload / 注入内核 | 关键读数 | 结论 |
|---|---|---|---|
| 1 | `c6dd2e6d` / `76e81411` | `err=23 qc=0 iofail=1 prep=2 stage=1 rb=f8222000` | GCC 写被策略拒；窗口盖不到 clkref |
| 2 | `86ad32a0` / `649ca6ac` | 重复 `prep=2 stage=1` | 归档里没有 `prepare_write`，白跑一轮 |
| 3 | `326b0adb` / `446c26d8` | `err=23 iofail=0 prep=8 stage=4 rb=00102800` | 复位位锁存（`CORESOFTRESET`=GCTL bit11） |
| 4 | `579d5bdb` / `bbea58b6` | `err=10 prep=0 stage=4 rb=00920001` | prepare 全通；卡 DMA |
| 5 | `ed1b9423` / `d3e40956` | `dr=5 ds=0` | SMMU `StreamExists`（已有 SID 0x140 条目） |
| 6 | `6e8663ad` / `702f2db0` | `dr=17 ds=9 la=80000140 lr=10003` | identity lease：SMR 找对，S2CR 改 TYPE 不落地 |
| 7 | `182e2822` / `999b2ee2` | `dr=17 ds=9 asid=1c bank=4c017e5e le=3 lr=20003` | 观测版：写 `0x10003` 回读 `0x20003` → TYPE 被重编码成 FAULT |
| 8 | `53def85d` / `5077db95` | `init=1 dr=0 ds=1 bank=0x37 le=3 lr=37` | **bypass-bank 通**；主机 COM3 出现 32 s |
| 9 | `e45a3c9a` / `6e55740e` | `init=1 … lt=8 tr=39 ta=1 tb=1` | Win32 逐步打开成功；读到 4096 B |
| 10 | 同 9（未重刷） | `lt=8 tr=31 tb=1` | 持续捕获 10,806 B / 179 行；日志静止 31.7 s 后复位 |

## r5–r7 时间线（示例）

- r5：boot 01:35:08 → COM3 01:35:33（25 s）→ 01:36:05 消失；81 s 回 recovery，PON `0x80041`。
- r6：boot 01:38:47 → COM3 01:39:19 → 01:39:45 消失；80 s 回 recovery，PON `0x80041`。
  捕获脚本在端口刚注册时立即 `Serial()`，配置失败（`OSError(22)`），**没有发生读取**。
- r6b：boot≈01:45:57 → COM3 01:46:23 → 01:46:54 消失；71 s 回 recovery。
  逐步 Win32 全成功；`ReadFile` 4096 B，pyserial 3739 B。
- r7：COM3 01:48:48.1 → 01:49:21.2（32.4 s）；10,806 B / 179 行；
  最后一条数据 01:48:49.4，之后 31.7 s 零字节直到复位；80 s 回 recovery，PON `0x80041`。

## 串口内容（r7 摘要）

- 开头是 VM/zone 初始化：`compressor`、`zone_init: map …`、`VM bootstrap done`、`OSLog stream configured`。
- 中段：`apfs_vfsop_mountroot … mountroot called!` → `disk1`（`disk1 device_handle block size 4096`）
  → `disk1s1` 挂载 `DawnUpdate21H461…OS`，`mount-complete`。
- 末尾：
  `nabu: owner SystemOS mount skipped (unverified root, err 80)`
  → `load_init_program: attempting to load /sbin/launchd`
  → `AMFI: '/sbin/launchd' is adhoc signed.` / `unsuitable CT policy 0 … rejecting signature.`
  → `proc 1: load code signature error 4 for file "launchd"`
  → 同样拒绝 `dyld` → `AMFI: hook..execve() killing pid 1`。
- **之后没有 panic/backtrace**，31.7 s 后复位。

> 结论：内核实际已经走到 launchd；此前“窗口只到 attach-done”是 `init=1` 后日志切到 USB、
> userdata 窗口不再更新造成的假象。

## 复位与 PON

- 每轮回 recovery 71–81 s，PON 恒 `0x80041`（内核侧 PSCI / 复位源）。
- 回滚线（300 s 未回 / PON `0x800c1` / 无任何输出）从未触发；板子 10 轮未挂死。
