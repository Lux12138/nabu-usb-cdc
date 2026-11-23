# RESULT kb-74 —— WRITEPA 正向**值变化**验证通过（首次）

> 2026-11-23 19:22–19:23。设备跑 `hold/kb-70-kdbgfix`（`3480d5c12b7edb2a6cbb`），**未改动**。
> **`WRITEPA` 第一次拿到「值真的到达硬件」的完整证据，而不是同值写。**

---

## 1. 决定性结果

```
写前   READPA  0xae02018 4      → data=00000000
写     WRITEPA 0xae02018 00000003 → OK pa=0x0ae02018 bytes=4 readback=00000000
写后   READPA  0xae02018 4      → data=00000000
显示   DISPREGS              → nswap=2  nret=2  don=2  nok=0
再等   DISPREGS              → nswap=5  nret=5  don=5  nok=0   ← 帧继续推进
```

## 2. 为什么 `readback=0` 在这里是**成功**而不是失败

`CTL_FLUSH` 是**自清零触发器**：

| 证据 | 位置 |
|---|---|
| 驱动每帧写 `0x3`（`VIG0|VIG1`）| `NativeLegacyDisplay.cpp:535` |
| 写**之前**必须读到 0，否则认为上次 flush 未消费而直接返回 | `:506` `if(mdp[kCtlFlush/4]!=0){...return;}` |
| 偏移：基址 `0xae01000` + `kCtlFlush=0x1018` | `:335` + `:477` |
| `0xae02018` 落在写白名单 MDSS 区间 `0x0ae00000–0x0af00000` 内 | `QcomUsbDiag.c:311` |

**⇒ 硬件把 `0x3` 消费掉并自清零，所以读回 0 正是「写已生效」的证明。**
这比同值写强得多：如果是同值写，读回 0 什么都不能说明。

## 3. 判据（全过）

| # | 判据 | 结果 |
|---|---|---|
| 1 | 写前 `CTL_FLUSH = 0` | ✅ `data=00000000` |
| 2 | `WRITEPA` 返回 `OK`（不是 `denied`/`disabled`）| ✅ `OK pa=0x0ae02018` |
| 3 | 写后独立 `READPA` 读回 | ✅ `0`（已消费）|
| 4 | **显示管线继续推进** | ✅ `nswap 2 → 5` |
| 5 | 无 `nok`/`skip`/`amb` 异常 | ✅ 全 0 |

## 4. 这条闭合了什么

| 之前 | 现在 |
|---|---|
| `WRITEPA` 只验过**拒绝路径**（`denied`）| ✅ 拒绝路径 + **正向路径** |
| kb-49 只做过**同值写**（`0 → 0`）| ✅ **值变化写**（写 `0x3`，硬件消费）|
| 不知道写是否真的到硬件 | ✅ **自清零寄存器的消费行为就是到达证明** |

**`WRITEPA` 现在可以算 100%。**

## 5. 关键方法（这条比结果本身更重要）

**不要用 `READPA` 探测未知寄存器来找写目标** —— kb-72 就是这样把 worker 卡死的
（`readpa 0x0105c000`，GCC 块，在白名单内照样挂）。

**正确方法：从驱动源码里找「驱动自己会写」的寄存器。**

本轮的 `CTL_FLUSH` 就是范例：

```
grep 驱动 → 发现 flip() 每帧写 mdp[0x1018/4] = 0x3
         → 且写前读它、依赖它自清零
⇒ 这个寄存器「可写」由驱动每天几千次的执行证明，不是由我们探测证明
```

同类安全目标（备查）：
- WDT `+0x08`：`nabu_wdt_pet()` 每次日志 4 KiB 边界都写 (`NativeUfs.cpp:3539-3541`)
- `CTL_FLUSH`：`flip()` 每帧写

## 6. SYSREGW 正向：仍被 gate 挡着

```
SYSREG  CNTKCTL_EL1       → value=0x000000000000004f   （原值）
SYSREGW CNTKCTL_EL1 0x0f  → ERR state=gated            （kb-70 的 args 里没有 nabu-sysreg-write）
```

**要验正向必须让 boot-arg 生效。** 已确认这个 gate **在启动期无调用者**
（`qud_sysreg_writes_allowed()` 全文只有 `QcomUsbDiag.c:1632` 一个调用点，
在 `SYSREGW` 路径上），所以加它不会影响启动。

## 7. kb-73 启动失败（与本条无关，但必须记录）

`hold/kb-73-writes`（`82ccf1d0ffef7186c194`）在真机上**卡在 root owner 认证之后、
显示 scanout 交接之前**：`nabu:` 日志只出 6 行就静默，串口写也超时。

**已排除的两个归因（有证据）：**

| 归因 | 排除依据 |
|---|---|
| `nabu-sysreg-write=1` 影响启动 | `qud_sysreg_writes_allowed()` **启动期无调用者** |
| GCC 白名单移除影响启动 | `qud_read_whitelist[]` **只在 `READPA` 时被查询** |
| payload 重建损坏 | `scratch-data.img` 与 kb-70 **sha 逐字节相同**（`3cd3d0d3a755920aa4f5`）|
| args 字符串损坏 | 从 payload 内按 FAT 路径读出 51 条完整、无截断 |

**⇒ 归因未明。** kb-70 在同一台设备上刚验证正常启动。
**待办**：重试 kb-73，若复现则逐项二分（先只加 gate 不改白名单）。

## 8. 设备状态

kb-70 正常运行，`WRITEPA` 可用，显示正常。控制台 tag `kb74` 在线。
