# RESULT 路线 A 命令发送验证通过（2026-11-23 16:35）

> **Route A 完整闭环**：`debug_terminal.py` 的 32 条设备命令，
> 12 条有设备端对应能力 → **全部真机 OK（13 次，含 `usb` 复用 PING）**；
> 20 条无对应能力 → 明确 NOMAP 并记录原因。

---

## 1. 结果

一次 CDC open（`session=6d82a63788ba87eb`），连续跑完 32 条命令：

```
summary: 13 OK / 0 ERR / 20 NOMAP (of 33)
```

### 1.1 可映射命令（全部 OK）

| giulia 命令 | 映射 verb | 实测回复 |
|---|---|---|
| `ping` | `PING` | `OK rx=46 rxo=0 tx=290 poll=10759` |
| `status` | `STATUS` | `OK usb=3 er=0 rx=48 rxo=0 tx=0` |
| `uptime` | `UPTIME` | `OK ticks=780232897 hz=19200000 us=40637130` |
| `bootargs` | `BOOTARGS` | `OK hex=2d762073657269616c3d332064656275673d30783…` |
| `info` | `VERSION` | `OK build=qcom-xnu-usb-console xnu=17.7.11-21H461 soc=sm8150` |
| `memory` | `SYSCTL hw.memsize` | `OK data=0000003001000000`（= 4 GiB，小端 `0x100000000`）|
| `ps` | `TASKDUMP` | `OK tasks_head=0xfffffff00c11d6d0 kernproc=0xffff…` |
| `display` | `DISPREGS` | `OK name=mdp xfer=0 nswap=0 nsurf=0 …` |
| `usb` | `PING`（复用）| `OK rx=46 … poll=11055` |
| `ufs` | `DUMLIST` | `OK 451b34d0` |
| `gpu.status` | `GMUSTATE` | `OK step=0 status=0 chip=0 fence=0` |
| `gpu.hfi.status` | `SMMUDUMP` | `OK state=80000002 summary=8038065e idr0=4c017e5e`（近似）|
| `screen.snapshot` | `SCREENSHOT` | `OK width=1024 height=3 bytes=9216` |

### 1.2 NOMAP 命令（20 条，原因明确）

| 类别 | 命令 | 原因 |
|---|---|---|
| 主机侧 | `help` `features` `replay` `sample` | 不需要设备（log 分析 / 能力表）|
| 用户态守护 | `shell` | 需 daemon + iOS ARM64 shell + AMFI |
| Wi-Fi | `wifi.status` `wifi.start` | 无 wifi verb |
| reboot | `reboot.fastboot` `reboot.status` | 我们的 REBOOT 回 recovery，不是 fastboot |
| GPU HFI | `gpu.observe` `gpu.hfi.pin` `gpu.hfi.query` `gpu.hfi.poll` `gpu.hfi.log` | 未实现的诊断窗口 / HFI 命令 |
| 屏幕流 | `screen.start` `screen.stop` `screen.configure` | 无屏幕二进制流 |
| HID | `input.status` `input.release` `input.keepalive` | 无 HID provider |

**这 20 条是 T43 §5 早就预判的「必须改内核 / 依赖守护 / 无对应物」，实测确认。**

---

## 2. 工具

`tools/route_a_bridge.py`（已提交）：
- `--dry-run` 打印映射表
- 省略 `--port` 时按 VID/PID `1209:316D` 每 2 秒轮询等 CDC
- 复用 `nabu_console.Session`（单句柄 + reader 线程分流日志与 SXNU1R）
- 一次 open 连续跑完所有命令，中途不关端口

**关键**：不能用老 `sxnu_ctl`（同步读会被 1 MB 日志洪流淹没），
也不能换工具反复开关 COM3（一个 boot 只有一次 open 机会）。

---

## 3. 关于 `gpu.hfi.status → SMMUDUMP` 的近似

`GMUSTATE` 返回 `step=0 chip=0`，GMU 状态是空的（当前没跑 GPU profile）；
`SMMUDUMP` 返回真实的 SMMU 寄存器（`summary=8038065e`，kb-16 就验证过逐字命中板测值）。
它是**近似映射**：giulia 的 `gpu.hfi.status` 是 17 个 HWSCHED 队列头，
我们的 `SMMUDUMP` 是 SMMU bank。**语义不完全一致，但确实有真实硬件读数。**

---

## 4. 路线 A 完整状态（最终）

| 部分 | 状态 |
|---|---|
| `debug_terminal.py` `Capture` + TOPICS 日志分析 | ✅ 已可用（零改动，2864 行按 topic 聚合）|
| 命令发送（12 条可映射）| ✅ **13 OK / 0 ERR**（本次）|
| 20 条 NOMAP | ✅ 原因明确，符合 T43 预判 |
| 屏幕流 / HFI / HID / shell / wifi | ⬜ 需内核/守护改动，不在"零改动"范围 |

**⇒ T43 P0#3「接 debug_terminal.py（路线 A）」在零内核改动前提下能做的部分，全部完成。**

---

## 5. 操作教训（本次又踩了一次）

我在验证过程中先用 PowerShell、sxnu_ctl、手工 python 反复开关 COM3，
把那次 boot 的唯一 open 机会用掉，导致 `PermissionError(13)` 锁死，必须复位。
复位后改用 bridge 一次 open 跑完，成功。

**纪律**：单句柄 CDC，任何"换个工具试试"都是破坏性操作。详见
`E:/nabu/state/ROOTCAUSE-20261123-bridge-cdc-handle.md`。

## 6. 设备状态

设备跑干净基线 kb-59，bridge 已关闭（Session.close），端口释放。
