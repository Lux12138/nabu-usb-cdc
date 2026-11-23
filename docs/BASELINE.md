# 基线 kb-99-fivefeatures（2026-11-23 21:57）

> **这是当前最新、能力最全的可用基线。**
> 设备实测：正常启动到桌面（PosterBoard 427 s），五个新功能全部真机验证。

---

## 1. 包信息

| 项 | 值 |
|---|---|
| 包目录 | `hold/kb-99-full` |
| 归档副本 | `hold/_baseline_archive/20261123/kb-99-fivefeatures/` |
| **kernel sha256** | **`f14f0ce6d33315272b29e92e67a4973098da3d75689d2fc858425d0a604aeed1`** |
| payload sha256 | `f0f78c0f5e01358a563149432f4e3fb630d710c4130bec6348cebc252d8c25ac` |
| boot 镜像 | `ead4b53135d67516` / 3,041,280 B（红线，永不重建）|
| adt | `ddb096bf1b20222f` / 211,568 B |
| trust bundle | `a4a6bae61f3febcf` / 61,456 B |
| gpu addon | `950a3968baa13ccd` / 12,317,544 B |
| **NOP18** | **已打**（`1f2003d5` @ 签名 `6100005408214439`）|
| 簇链 | `<39151-61431> <67551-97914> <90-8102> <8104-8827>` |
| boot-args | `n=50 c=887`，无 gate（`nabu-panic-trig` / `nabu-sysreg-write` 都不在）|

**git tag**：`baseline-kb99-fivefeatures`（commit `24113a8`）

## 2. 相对 kb-70 增加的能力

| verb / 命令 | 状态 | 真机输出样例 |
|---|---|---|
| **`TSENS`** | ✅ 新增 | `hottest=37.8 C`，14 个传感器全有效 |
| **`JOBSTAT`** | ✅ 新增 | `completion=1..31 hw=2..32 status=0` |
| **`SLOW`** | ✅ 新增 | `90 MiB 分配 elapsed_us=132348 spans=23112` |
| **`CPSTAT`** | ✅ 新增 | `cp=0 hfi=a/0 ack=7 chip=6401cafe zap phase=6` |
| **`IRQSTAT`** | ✅ 代码已加 | `rtclock_intr` + `irq=` 总数（需重启控制台生效）|
| 其余 48 个 verb | 不变 | 全部可用 |

**驱动侧新增导出（10 个符号）：**

```
_nabu_cpufreq_tsens_tenths / _tsens_raw / _thermal_level / _switches / _vote_khz
_nabu_console_diag_totals / _slow_flat / _slow_count
_nabu_console_job_samples / _job_sample_count
```

## 3. 本轮同时修掉的工程问题

| 问题 | 状态 |
|---|---|
| **NOP18 漏打**（曾导致 6 轮构建启动卡死）| ✅ `pack_round.sh` 强制检查，未打就拒绝打包 |
| **`mcopy` 重排 FAT 簇**（曾导致 4 轮启动卡死）| ✅ `payload_swap_kernel.py` 原地写 + `payload_layout_check.py` 校验 |
| **`out-v6` 91 层 bind mount** | ✅ 改成符号链接，构建 0 层 |
| **kernel 目录 33 个历史副本（3.8 G）** | ✅ 清到 2 个 |
| **138 个 `.bak`/`.pre-l*` 混在驱动真源** | ✅ 清空（历史移到 `hold/_driver-history/`）|
| **20 个旧 `pack_kb*.sh`** | ✅ 全删，只剩 `pack_round.sh` |

## 4. 已知状态与限制

### 4.1 启动慢（427 s vs kb-70 的 207 s）

`xpcproxy` 重试 768 次（kb-70 是 340 次）。`bluetoothd` / `fairplayd` /
`MobileAccessoryUpdater` 反复重启，把 launchd 队列拖长。

**与本轮改动无关**（`lat` 最大值反而从 451 ms 降到 10.8 ms）。

### 4.2 桌面抽搐（根因已定位但未修）

`SLOW` 拿到的直接证据：

```
seq=5 allocation elapsed_us=132348 bytes=94666752 spans=23112 source=2(UserOwned) cache_result=2(Miss)
seq=6 allocation elapsed_us=112040 bytes=94666752 spans=23112 ...
seq=1 allocation elapsed_us= 38186 bytes=67108864 spans=16384 ...
```

**机制**：
```
backboardd 提交 90 MiB BO
  → backing cache 单对象上限 16 MiB（BackingCacheObjectBytes）⇒ 必然 Oversized
    → 永不缓存 ⇒ 每帧重新分配 90 MiB
      → 分配期间 GPU 闲着（GPU 单帧只需 15-20 ms）
        → 用户看到 130 ms 卡顿
```

**但「调大缓存」已被实测证伪**（`board-report-l772-cache-result.md`）：
上限提到 64/256 MiB 后命中率只从 1.2% → 2.1%，
而 ≥500 ms 占比从 61% 恶化到 **81.7%**，因为合成器的工作集是 **imported memory**，
`release()` 在 `cacheBacking()` 之前就把 imported buffer 拒掉了
（`BackingRejectImported`，`NativeAdrenoMemory.cpp:1023`）。

**⇒ 真正的开关在 imported 路径，不在缓存容量。**
**⚠️ 这个结论意味着：加缓存不是解法，下一轮要动的是 imported 生命周期。**

### 4.3 `KDP` 退出

- **`Debugger()`**：能进能出（`DEBUGGER 6363636363636363` 预置 8 个 `'c'`）
- **panic 之后**：仍然出不来（`armed=0`，字节进不去 `kdp_rx`）
- **`'r'`**：走不通（halt 钩子只认 `type == 1`，`kPEPanicRestartCPU` 是 4）

## 5. 复现这一版

```bash
# 1. 构建（零 mount，带环境检查）
bash /e/nabu/tmp/_build_inner.sh <tag>

# 2. NOP18（必须）
python3 /e/nabu/tools/_nop18_l800.py
#    期望 PATCHED / nop18 1

# 3. 打包（自动校验 NOP18 + 三条 sha + 簇链 + boot 红线）
bash /e/nabu/tmp/pack_round.sh <tag>

# 4. 上板
bash /e/nabu/tmp/go_round_next.sh <tag>
```

## 6. 回滚点

| 包 | kernel sha16 | 何时用 |
|---|---|---|
| **`kb-99-full`** | **`f14f0ce6d3331527`** | **当前基线，全部功能** |
| `kb-92-nop18` | `e3cb7046d0f8eec1` | 验证 KDP 进出用的包 |
| `kb-70-kdbgfix` | `3480d5c12b7edb2a` | 上一个稳定基线，启动 207 s |
| `kb-68-stackshot` | `c129d83cee9ceb63` | 有 STACKSHOT，无 HFI |
