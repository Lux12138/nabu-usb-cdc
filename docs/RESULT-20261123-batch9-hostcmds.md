# 主机侧批量命令：9 条零内核改动新增（2026-11-23 20:03）

> 依据：`PLAN-20261122-console-commands-full.md` 第 3/4/7 组。
> **全部在主机侧实现，内核侧零改动、零刷机、零新增计数器。**

---

## 1. 为什么不用做 verb

方案 §3 写的是「这 6 个直接读现有数组，零改动」。查证后更进一步：

**这些数组的符号本来就导出，`READ` 直接能读。**

```
fffffff00c1aebc0  S  _native_adreno_vm_phase_us
fffffff00c1ac318  S  _native_adreno_backing_cache_stats
fffffff00c1ac398  S  _native_adreno_pool_stats
fffffff00c1ac1d8  S  _native_adreno_memory_stage_stats
fffffff00c1aec40  S  _nabu_alias_stats
fffffff00c1ae60c  S  _native_adreno_ib_log
fffffff00c1ae80c  S  _native_adreno_ib_log_count
fffffff00c1ae980  S  _native_adreno_method_stats
fffffff00c1aeb28  S  _native_adreno_client_stats
fffffff00c1adda0  S  _native_adreno_wait_outer
```

**⇒ 这正好是方案 §12 的原则：内核只做原语（`READ`），语义在主机侧组合。**

## 2. 新增的 9 条命令（`tools/nabu_console.py`）

| 命令 | 数据源 | 维度 |
|---|---|---|
| `VMSTAT` | `native_adreno_vm_phase_us` | `[2][8]` u64 |
| `MEMSTAT` | `native_adreno_backing_cache_stats` | `[12]` u64 |
| `POOLSTAT` | `native_adreno_pool_stats` | `[8]` u32 |
| `MEMSTAGE` | `native_adreno_memory_stage_stats` | `[10][4]` u64 |
| `ALIASSTAT` | `nabu_alias_stats` | `[6]` u64 |
| `IB [n]` | `native_adreno_ib_log[4][32]` + `_count` | u32 |
| `METHODS` | `native_adreno_method_stats` | `[20][4]` u32 |
| `CLIENTS` | `native_adreno_client_stats` | `[5][6]` u32 |
| `WAITOUTER` | `native_adreno_wait_outer` | `[4]` u64 |

## 3. 真机实测（一次 open，9 条全过）

```
VMSTAT map   calls=462 metadata_us=350674 cache_us=55 tables_us=173475
             index_us=211288 preflight_us=165738 max_us=125190 bytes=951443456
VMSTAT unmap calls=111 metadata_us=0 cache_us=31 tables_us=89786
             index_us=240265 release_us=8887 max_us=64246 bytes=491089920
MEMSTAT backing_cache s0=0 s1=323 s2=3 s3=0 s4=108 s5=3 s6=16384 s7=16384 s8..11=0
POOLSTAT pool_mib=0 pool_bos=0 fallback_bos=0 used_mib=0 peak_mib=0
         scattered_bos=336 scattered_spans=130780 imported_spans=93516
MEMSTAGE allocation         a=336 b=504844 c=118330 d=544698368
MEMSTAGE prepare            a=336 b=78     c=5      d=544698368
MEMSTAGE spans              a=336 b=17209  c=3230   d=544698368
MEMSTAGE zero               a=336 b=33341  c=6614   d=544698368
MEMSTAGE complete           a=108 b=20918  c=1706   d=491073536
MEMSTAGE descriptor_release a=108 b=169956 c=81160  d=491073536
ALIASSTAT builds=462 max_hits=23112 sum_hits=224296 s3=1 s4=3 s5=3
IB count=0 slots=0
METHOD  2 calls=323 failures=0 status=00000000 total_us=545171
METHOD  3 calls=452 failures=0 status=00000000 total_us=901507
METHOD  4 calls=111 failures=0 status=00000000 total_us=586630
CLIENT slot=0 s0=360 s1=378 s2=0 s3=0 s4=0 s5=700
WAITOUTER s0=331 s1=3722465 s2=91 s3=165353
```

**交叉验证**：`METHOD 2` 的 `calls=323` 与 `MEMSTAT` 的 `s1=323` 一致；
`MEMSTAGE spans` 的 `17209/3230` 与驱动日志 `memory_stage=spans` **逐字段相同**；
两次采样间 `map calls` 459→462、`builds` 459→462 **连续增长**，证明读的是实时值。

## 4. 踩到并修掉的坑：`READ` 的回复上限

第一版直接 `READ <addr> <320>`，板上回了 `len=320` 但 `data=` 只有 **192 字节**。

**根因**：`SXNU_CONTROL_REPLY_BYTES = 512`（`QcomUsbControl.h:15`），
减去协议开销后 `data=` 最多 192 字节。**`len=` 是请求长度，不是实际返回长度。**

**症状具有欺骗性**：`methods`/`memstage` 报"读取失败"，但板上明明答了。

**修法**：`read_bytes()` 按 192 字节分块循环读，拼满再解析。
**教训**：任何超过 192 字节的数组读取都必须分块。

## 5. 对方案完成度的影响

| 组 | 方案条数 | 新增后完成 | 说明 |
|---|---|---|---|
| **3. 内存管理** | 6 | **6/6** ✅ | `VMSTAT` `MEMSTAT` `POOLSTAT` `MEMSTAGE` `ALIASSTAT` `VMREGION`︎* |
| **4. GPU 执行** | 12 | **9/12** | 新增 `METHODS` `CLIENTS` `IB`；仍缺 `JOBSTAT` `SLOW` `CPSTAT` |
| **7. 系统与电源** | 6 | **4/6** | 新增 `WAITOUTER`；仍缺 `TSENS` `IRQSTAT` |

\* `VMREGION` 的数据（IOVA 范围）没有导出符号，只能从 `mapping_client` 日志里取。

**从 47% 提升到约 62%（严格口径）。**
