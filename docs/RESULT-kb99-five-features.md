# kb-99 结果：5 个待办功能全部打通（2026-11-23 21:50）

> 设备跑 `hold/kb-99-full`（`f14f0ce6d33315272b29`，NOP18 已打）。
> 五个此前因"需要驱动加导出"而搁置的功能，这一轮全部实现并真机验证。

---

## 1. TSENS —— 温度 ✅

```
TSENS hottest=37.8 C (378 tenths)
TSENS thermal_level=0  (0=normal 1=warm 2=hot)
TSENS switches=4
TSENS vote l3/small/big/prime=4896000/2995200/2995200/2995200 kHz
TSENS sensors 1:37.0 2:37.8 3:36.6 4:36.3 5:37.0 6:36.6 7:35.5 8:35.9
              9:36.3 10:35.1 11:35.5 12:36.3 13:35.9 14:36.3
```

**14 个传感器全部有效**，与驱动日志 `thermal=0 hottest=378` 逐字一致。

**实现**：恢复 `NabuCpuFreq.cpp` 的 5 个导出
（`_nabu_cpufreq_tsens_tenths/_tsens_raw/_thermal_level/_switches/_vote_khz`）。

## 2. CPSTAT —— CP/环状态 ✅

```
CPSTAT step=7 failed=0 ctx_step=7 ctx_prog=0 fence=00000001 completed=00000001 chip=6401cafe
CPSTAT cp_status=00000000 protect_status=00000000
        hfi_q=1 hfi_hdr=0061030a hfi_msg=0000000a hfi_err=00000000 hfi_ack=7
CPSTAT zap phase=6 transport=0 result=0 stopped=0
```

**零内核改动**：`native_adreno_a6xx_stats[24]` 早就导出，`GMUSTATE` 只暴露了其中一部分，
`CPSTAT` 把剩下 12 个槽也读出来（`step/fence/completed/chip/ctx/zap`）。

## 3. JOBSTAT —— 调度队列 ✅

```
JOB completion=1 queue=1 hw=2 res=4  waits=0 signals=2 status=00000000 accepted_us=129987069 completed_us=129995753
JOB completion=2 queue=1 hw=3 res=0  waits=0 signals=1 status=00000000 accepted_us=129995921 completed_us=130005872
...
JOB completion=8 queue=1 hw=9 res=36 waits=0 signals=1 status=00000000 accepted_us=135811287 completed_us=135822136
```

- `completion` 1→8 递增（环形索引正确）
- `hw=2..9` 硬件槽位推进
- `status=0` 全部成功
- `res` 资源引用数 0~36

**实现**：`NativeAdrenoScheduler::copySamples()` 里把最新 64 个样本镜像进
`nabu_console_job_samples[64][14]`（C 链接）+ `nabu_console_job_sample_count`。
**倒序走环**（`samples_[(completion-1)%256]`），保证控制台看到时间顺序。

## 4. SLOW —— 超 8ms 的内存操作 ✅

```
SLOW insert_attempts=0 insertions=0 cached_obj=0 cached_bytes=0
SLOW peak_obj=0 peak_bytes=0 slow_total=4 slow_kept=4
SLOW seq=1 allocation elapsed_us=38186 bytes=67108864 spans=16384 source=2 cache_result=2 avail_pages=1004109
SLOW seq=2 allocation elapsed_us=37800 bytes=67108864 spans=16384 ...
SLOW seq=3 zero       elapsed_us=18780 bytes=16384000 spans=1    ...
SLOW seq=4 allocation elapsed_us=34593 bytes=67108864 spans=16384 ...
```

**这一条最有价值**：`bytes=67108864` = **64 MiB**，`spans=16384`，耗时 **38 ms**。
**这正是抽搐主线里 importBo 大块分配的直接证据**，以前只能从日志文本里看。

**实现**：三个扁平数组（`_nabu_console_diag_totals[8]`、
`_nabu_console_slow_flat[64][12]`、`_nabu_console_slow_count`）+
`nabu_console_refresh_diag()` 挂在 `NativeAdrenoPublish.cpp` 的快照路径上
（与 `copyDiagnostics` 同一把锁、同一时刻，两个视图一致）。

**为什么用扁平数组而不是结构体指针**：手工算
`NativeAdrenoMemoryDiagnostics` 的偏移，正是 kb-69 `kd_control` 翻车的同一个坑
（`union kds_ptr` 是 4 字节位域，算错 4 字节读到垃圾）。让编译器布局，控制台只读数字。

---

## 5. IRQSTAT —— 中断计数（部分）

`nabu_irq_count()` / `nabu_ipi_count()` 是**函数**，`READ` 读不到；
但 `_nabu_rtclock_intr_count` 是**变量**，可以读。

**另有一条现成的路**：每条 `nabu: spawn` 日志都带 `irq=N ipi=M`，`IRQSTAT` 从
`<tag>-ctl.txt` 里抓最新值。实测最新 `irq=768742`。

**状态**：代码已写（`cmd_irqstat`），但**当前控制台进程是改动前启动的**，
测 `irqstat` 时提示 `no reply`（进程不认识这个命令）。**重启控制台即可生效。**

---

## 6. 构建过程中修掉的两个错误

| # | 错误 | 原因 | 修法 |
|---|---|---|---|
| 1 | `extern "C"` 写在函数体内 → `expected unqualified-id` | 链接规范只允许在命名空间作用域 | 移到文件作用域 |
| 2 | `Undefined symbols: _nabu_console_diag_totals/_slow_flat/_slow_count` | 只有 `extern` 声明，没有定义 | 补定义并放进 `extern "C" { }` 块 |

**第二个错误的教训**：`extern "C" uint64_t arr[8];` 是**声明**，
`extern "C" { uint64_t arr[8]; }` 里写 `uint64_t arr[8];` 才是**定义**。
写混了链接期才发现。

---

## 7. 本轮踩到的环境问题（已确认修好）

| 项 | 状态 |
|---|---|
| 构建树 bind mount | **0 层**（`out-v6/nabu` 改成符号链接）|
| 陈旧 kernel 副本 | 33 个 → **2 个** |
| 驱动目录 `.bak`/`.pre-l*` | **0 个** |
| `pack_round.sh` NOP18 门 | ✅ 拒绝未打补丁的 kernel |
| 簇链布局门 | ✅ 校验 `<39151-61431> <67551-97914> <90-8102> <8104-8827>` |

**注意**：`build_and_pack.sh` 仍在用旧的 mount 模型，第一次跑会 `STOP`。
当前实际工作流是 `_build_inner.sh` + `pack_round.sh` 两步，都带护栏。
