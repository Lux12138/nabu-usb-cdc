# FAILED-ROUND kb72 —— `READPA 0x0105c000` 卡死 worker（WRITEPA 验证中止）

> 2026-11-23 19:06。**达到了主要目标（写入值变化验证通过），但在探测 WRITEPA 目标时
> 踩中 READPA 的已知致命模式，设备卡死。**
> 这是坑 #27 的**第四次**复现，且这次地址**在读写白名单内**。

---

## 1. 卡死现场

```
19:06:16  HELLO OK                              ← 最后一条正常回复
19:06:32  TX READPA 0x0105c000 4 (seq=16)       ← 发出，再无回复
19:06:32 → 之后 kb72-com.bin 停止增长（1,074,541 B 定格）
```

| 判据 | 结果 |
|---|---|
| 控制帧回复 | ❌ 无 |
| 设备日志（IN）| ❌ **也停了**（1,074,541 B 后不再增长）|
| 结论 | **USB worker 被 MMIO 读卡死**，不是单纯 OUT 失效 |

**注意区分**：上一轮 kb71 是 IN 通、OUT 死（第二次 open）。**这一轮是 IN 和 OUT 都死**
（worker 线程卡在 MMIO 读上）——两种失效模式不同。

## 2. 关键：这个地址在白名单里

`qud_read_whitelist[]` 明确包含：

```c
{ UINT64_C(0x01050000), UINT64_C(0x01090000) }, /* GCC */
```

`0x0105c000` 落在 GCC 范围内，**读白名单放行了**，但硬件当时不接受这次访问，
`memcpy` 永不返回。

**⇒ 白名单只能证明"这个地址曾经被安全读过"，不能证明"现在能安全读"。**
这正是源码注释里已经写下的教训（`QcomUsbDiag.c:340-346`）：

> "the address exists" and "reading it right now is safe" are different statements.
> A static list cannot decide the second one -- SMMU needs its clock/power domain up,
> and that is not knowable from here.

**这次证明了这条规律对 GCC 同样成立，不只对 SMMU。**

## 3. 本轮已获得的成果（卡死前完成）

### 3.1 `WRITE`（内核虚拟地址写）—— 值变化验证通过 ✅

这是**真正的新证据**：不是同值写，是 `0 → 0x5a5a → 0` 的完整往返。

```
基线    READ  0xfffffff00c5d5cb8 8  → data=0000000000000000
写      WRITE 0xfffffff00c5d5cb8 5a5a000000000000
                                    → WRITE OK state=written bytes=8
读回    READ  0xfffffff00c5d5cb8 8  → data=5a5a000000000000  ✅ 值真的变了
恢复    WRITE 0xfffffff00c5d5cb8 0000000000000000
                                    → WRITE OK state=written bytes=8
再读回  READ  0xfffffff00c5d5cb8 8  → data=0000000000000000  ✅ 已恢复
活性    HELLO                        → OK session=d0988c14194c0d7d  ✅ 内核健康
```

**目标变量**：`nabu_log_dropped`（`0xfffffff00c5d5cb8`，BSS 计数器，本轮一直是 0，
写到 0x5a5a 再写回 0，无真实副作用）。

**⇒ 写通路本身完全可用，且可逆。**

### 3.2 `WRITEPA`（MMIO 写）—— 正向路径确认可用，但只验了同值写 ⚠️

```
READPA  0x17c10008 4        → data=00000000      （WDT EN=0，狗是关的）
WRITEPA 0x17c10008 00000000 → OK readback=00000000
```

**通路通，但没有值变化证明**（0 → 0 是同值）。原因：WDT `+0x08` 写 1 就是**武装看门狗**
（`NativeUfs.cpp:3520-3522`，`g_regs[8/4] = 0; g_regs[8/4] = 1;`），绝不能拿它做值变化测试。

### 3.3 `SYSREGW` —— gate 行为双向确认（正向未做）

```
SYSREG  CNTKCTL_EL1        → value=0x4f        （原值，与 kb-60b 一致）
SYSREGW CNTKCTL_EL1 0x0f   → ERR state=gated   ✅ gate 关时正确拒绝
```

**正向写入需要 `nabu-sysreg-write=1`，当前 boot-args 里没有。** 留待下一轮。

### 3.4 `WRITEPA` 的 gate 已开

```
WRITEPA 0xdeadbeef 00000001 → ERR state=denied
```
返回 `denied`（不是 `disabled`）⇒ `qcom-usb-write=1` 生效。

## 4. 纪律更新

### 4.1 新增红线：READPA 的目标地址必须先有**同 boot 的**成功读记录

不只是"白名单里有"，而是"**这个 boot 里刚读成功过**"。

**更根本的结论：用 READPA 去探测未知寄存器是错误的方法。**
想找 WRITEPA 目标，应该从**驱动源码里找已知会写的寄存器**
（像 kb-49 选 WDT EN 那样，因为 `nabu_wdt_pet()` 每 4 KiB 就写一次），
而不是拿 READPA 挨个探。

### 4.2 WRITEPA 值变化验证的正确目标（下一轮用）

| 候选 | 依据 | 风险 |
|---|---|---|
| **MDSS `+0x0` 的某个可写位** | 需先查 `NativeLegacyDisplay.cpp` 里哪些寄存器是驱动自己会写的 | 中 |
| **DWC3 的 `GCTL`** | 驱动初始化时写过，且在写白名单 `0x0a600000-0x0a700000` | 中高（动 USB）|
| **WDT `+0x00`（超时值）** | 写白名单内，但改了会变看门狗行为 | **高，不做** |

**建议**：下一轮直接从 `NativeLegacyDisplay.cpp` 里找驱动自己会写、且可回读的状态位，
拿来同时做 `WRITEPA` 和 `SYSREG` 的验证。

## 5. 本轮资源

| 项 | 值 |
|---|---|
| 日志 | `E:/nabu/logs/kb72-com.bin`（1,074,541 B，卡死定格）|
| 控制台 | `E:/nabu/logs/kb72-stdout.log`、`kb72-ctl.txt` |
| 命令文件 | `E:/nabu/logs/kb72-cmd.txt` |
| 内核 | kb-70 `3480d5c12b7edb2a6cbb`，**未改动** |
| boot | 19:02:14 fastboot boot，19:02:43 CDC 枚举（60.7 s）|
| 设备状态 | **worker 卡死，需断电复位** |
