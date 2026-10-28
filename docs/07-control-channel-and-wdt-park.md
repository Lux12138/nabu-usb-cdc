# 控制通道、内核复位与看门狗定时停车（2026-10-28）

本文记录把 CDC 串口从「只能读日志」推进到「可双向控制 + 可无人值守刷机」的全过程，
以及最后落地的方案：**由内核在指定时刻武装 APSS 硬件狗，硬件负责复位**。

一句话结论：**完整启动日志（~146 KB）+ 硬件自复位 + 自动回到 recovery，三者可同时成立，零人工。**

---

## 1. 帧协议 SXNU1（已有实现，勿另造）

```
请求  SXNU1  <session:16hex> <seq:8hex> <VERB> <crc32:8hex>\n
回复  SXNU1R <session:16hex> <seq:8hex> <VERB> <status> [extras] <crc32:8hex>\n
```

- `session`/`seq` 在 VERB **之前**；行 ≤127 B，帧 ≤128 B（含换行）。
- CRC-32/IEEE 覆盖行首到「CRC 前那个空格之前」的全部字节。
- `HELLO` 必须 `session=0 && seq=0`；其余动词必须用当前**非零 session** 且 **seq 严格递进**。
- CRC 不符、跳号、重放都只是把 `control_rejected` 加一，不会回帧。

实测回复：

```
TX  SXNU1 0000000000000000 00000000 HELLO a1b2c3d4
RX  SXNU1R a1a4d73a12f05416 00000000 HELLO OK session=a1a4d73a12f05416 last_seq=00000000 control=1 <crc>

TX  SXNU1 a1a4d73a12f05416 00000001 STATUS
RX  SXNU1R a1a4d73a12f05416 00000001 STATUS OK usb=3 er=0 rx=48 rxo=0 tx=86 bp=0 bo=0 poll=49705 ev=1 gen=1 rej=0 <crc>
```

`usb=3` 是 `QCOM_USB_DEBUG_ACTIVE`；`rej=0` 表示没有一帧被拒。

主机侧客户端：[`tools/sxnu_ctl.py`](../tools/sxnu_ctl.py)（单帧、可脚本化）、
[`tools/port-session.py`](../tools/port-session.py)（同一句柄既落日志又发帧）。

---

## 2. 复位链路：`REBOOT` 走到哪里、卡在哪里

`REBOOT` 只做两件事：回 `OK state=queued`，然后由专用内核线程在 master CPU 上
`wait_drained(2000)` → `arm` → 复位。实测：

| 阶段 | 观察 |
|---|---|
| 帧被接受 | `REBOOT OK state=queued` |
| 复位线程 | 绑核成功、drain 成功（否则回 `-2`/`-3`） |
| `reboot_kernel()` | **launchd(pid 1) 退出并试写 coredump ⇒ 确实进入了正常重启流程** |
| 实际复位 | **没有发生**：USB 不断、无 PSCI 迹象 |

日志显示它停在：

```
syncing disks... Killing all processes
vfs_unmountall(nonsystem) start
unmount(1) /private/var
unmount(1) /System/Library/Caches
pid 1 exited -- exit reason namespace 2 subcode 0xf
```

`halt_all_cpus(TRUE)` 之前的 **VFS 卸载阶段卡死**（本移植的 `/private/var` 是自建内存
FS，根卷是只读 APFS，本来没有需要落盘的数据）。而 `halt_all_cpus()` 才轮到
`PE_halt_restart`，所以劫持钩子永远没被调用（`[rb] hook` 零命中）。

**修法**：arm 成功后**跳过 `reboot_kernel()`**，直接 `platform_smc_reset()`
（PSCI `SYSTEM_RESET`，`x0=0x84000009`，`smc #0`，之后 `wfe` 死等）。

结果：`REBOOT` 后 **同秒 USB 消失**，`boot+12~15 s` 落 recovery（`misc` 里的
`boot-recovery` 由 XNU 每次 `startRoot()` 自动写入，ABL 读后清零）。

---

## 3. 为什么最后改用硬件狗定时停车

需求分两面：

1. **采集窗口要长**：完整启动日志到 `boot+81 s` 左右才静默，总量 ~146 KB；少于 95 s 的采集拿不到全量。
2. **控制帧来不及**：CDC 的**接收**在 `boot+70~90 s` 之后失效（同一内核早/晚发帧结论相反），
   所以主机无法在「启动跑完」之后再发 `REBOOT`。

于是必须让**内核或硬件**在 `boot+130` 左右自己复位。内核侧四种写法全失败：

| 写法 | 结果 |
|---|---|
| `delay_for_interval(N, NSEC_PER_SEC)` | 一次迟到 ~2 倍、一次永不触发 |
| `assert_wait_deadline()` 独立线程 | `ticks` 算对（`19.2 MHz × N`）仍不触发 |
| worker 循环里轮询 deadline | 打印证明在比（`now` 每 10 s +1.92e8），但 `phase != ACTIVE` 后循环退出 |
| 写路径里轮询 deadline（`qcom_usb_debug_write` 入口） | 能触发，且能跨过 worker 退出 |

`delay_for_interval` 之外那两种失败是同一族：**`thread_block()` 自身没有超时**，
不检查 `assert_wait_deadline()` 的返回值就会永久睡死
（`_clock_delay_until_deadline_with_leeway()` 正是这个形状：`assert_wait_deadline(...); thread_block(...)`）。

**最终方案：内核只负责「何时武装」，复位交给硬件。**

```
WDT.CFG  mode=0                      # Mu 不再在 ExitBootServices 预武装
kernel   nabu-park-after=32          # 到点调用 nabu_wdt_arm_now()
         → WDT EN=0 → BARK=BITE=ticks → EN=1
         → 硬件在 ~64 s 后咬合 → SoC 复位 → ABL 读 misc → recovery
```

实测（三轮一致）：

| 轮次 | 配置 | arm | 咬合 | recovery | 日志 |
|---|---|---|---|---|---|
| r10z3s | Mu `mode=1`（预武装 240 s） | boot+0 | **boot+61** | boot+74 | 15.8 KB |
| r10z3t | Mu `mode=1`（预武装 540 s） | boot+0 | **boot+64** | boot+76 | 15.8 KB |
| **r10z3v** | Mu `mode=0` + XNU `park-after=32` | counter≈50.7 s | **boot+114（COM3+86）** | boot+129（+15 s） | **146 KB** |

### APSS 狗的硬约束（实测）

- `WDT_TICK_HZ = 32768`、`NABU_WDT_BITE_MS` 是 **Mu 编译期常量**，`WDT.CFG` 只认 `mode=N`；
- **咬合上限约 64 s**：`BITE_MS` 240000 → 61 s，540000 → 64 s。`2097152 ticks ≈ 64 s`
  正好是 21 位计数在 32.768 kHz 下的满量程 ⇒ **写更大的值只是回绕**。
  "把咬合时间往后延"在硬件层做不到，只能改「什么时候武装」。
- Mu 的驱动 `NabuWdtDxe` 在 **boot.img 的固件卷**里，不在 payload；改它必须重打 boot 镜像。
  在内核侧武装则可以完全不碰固件。

---

## 4. 无人值守流水线

`tools/pipeline.py` 把一轮压成一条命令：

```
step0 状态自适应：CDC 在→XNU；adb recovery→TWRP；fastboot→直接刷
step1 XNU：单句柄 hello → status → reboot（可选 pause→reboot 收尾）
step2/3 CDC 消失 ≤10 s，adb recovery ≤90 s
step4/5 adb reboot bootloader ≤60 s；核 sha256+字节 → flash cust → boot
step6/7 新内核 CDC ≤60 s；单句柄 hello（+ 可选采集）
step8 park：等 recovery ≤120 s（复位源=内核/WDT，非主机）
```

实测：**单轮 86 s（park 轮）/ 2 分 34 秒（含 95 s 采集）**，全程零人工。
换内核只是改 `--payload-dir` 与 `--payload-sha256`。

---

## 5. 踩过的坑（都带判据）

1. **一个 boot 周期只能 open 一次 CDC**。`close()` 之后再 `open()`：要么阻塞 120 s，
   要么直接 `ERROR_GEN_FAILURE(31)`（`SetCommState` 失败）。⇒ 多步操作必须放进同一进程、
   同一句柄；"先 hello，另起一次发 reboot"的写法会被设备限制打死。
2. **控制帧的 RX 窗口在 `boot+70~90 s` 关闭**。同一内核：`boot+28 s` 发 hello 正常、
   `boot+69 s` 正常、`boot+88 s` 发出的 REBOOT 完全没有 `[ctl] rx`。⇒ 结论必须绑定 `boot+N`。
3. **TX 会偶发停摆（约 1/3 轮次）**，位置固定停在 `IONVRAM sync` 之后，且 95 s 内零新增字节。
   **日志静默 ≠ 系统停住**：同一轮里 `startRoot()` 仍然跑完、`misc=boot-recovery` 仍然被写、
   板子仍然进了 recovery。
4. **`kprintf` 进不了 CDC**。板上有大量 `kprintf`（如 `QCOM USB WORKER active`），
   CDC 日志里一个字都看不到。诊断必须走 `qcom_usb_debug_write()` / 直写环。
5. **`delay_us()` 拒绝 >10 ms 的延时**，并且会顺手把 `io_failed=1`（随后 phase 转
   `IO_FAILED`、日志彻底停发）。长延时一律用等待原语或硬件定时器。
6. **等待原语要检查返回值**：`thread_block()` 没有超时，`assert_wait_deadline()` 返回
   `THREAD_NOT_WAITING` 时直接 `thread_block()` 会永久睡死。
7. **worker 循环会退出**：`if (status.phase != QCOM_USB_DEBUG_ACTIVE) break;` —— userland
   起来后 phase 变化，之后没人再检查。任何「到点做事」的逻辑，要么放在写路径，要么交给硬件。
8. **复位归因要看来源，不要看时间窗**：落到 recovery 的事件可能正好落在预期窗口内，
   实际却是人按的键。可靠判据只有三条：日志里出现内核侧标记（`[park] wdt`）、
   主机收到 `REBOOT OK`、或者用户确认没按键。
9. **`reboot_kernel()` 在本移植上会卡在 `vfs_unmountall`**（见第 2 节），
   要么修卸载路径，要么用 PSCI/WDT 直接复位。
10. **`ticks` 只证明"算对了"**：`[park] arm n=32 ticks=614400000` 正确，但到点没触发——
    算对与执行到是两件事，缺探针就会把"没执行"误判成"时间不准"。

---

## 6. 复现清单

- 内核侧需要（本仓库 `patches/` 里的是补丁形态，其余两个改动以本文代码为准）：
  1. `qcom_usb_debug_control_start()` + 钩子安装 + `arm_reboot()` 校验；
  2. reboot worker：arm 成功后**直接** `platform_smc_reset()`；
  3. `nabu-park-after=N`：worker **和**写路径各检查一次 deadline，到点调 `nabu_wdt_arm_now()`；
  4. `nabu_wdt_arm_now()`：映射 `0x17C10000`，`EN=0 → BARK/BITE=ticks → EN=1`。
- `WDT.CFG`：`mode=0`（不要让 Mu 预武装）。
- 引导参数：`qcom-usb-debug=1 qcom-usb-ctl=1 nabu-park-after=32`。
- 主机侧：`tools/port-session.py`（单句柄）+ `tools/pipeline.py`（编排）。

预期：`boot+28` 出现 COM，`boot+81` 日志静默（~146 KB），`boot+114` 咬合，
`boot+129` 回到 recovery，全程不需要按键。
