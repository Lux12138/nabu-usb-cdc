# 当前基线 kb-59-ufsoffset（2026-11-23 15:58）

> **这是目前走得最远且最干净的基线。**
> 它和 kb-60 是同一个 kernel（`9cc32cf0...`），能力完全一样；
> 差别只在 boot-args —— kb-59 **没有任何 gate**，kb-60 两个 gate 全开。

---

## 1. 为什么是它

| 候选 | kernel | boot-args | 判定 |
|---|---|---|---|
| kb-50-readall | `edc68313` | n=50/c=871 无 gate | 老基线，能力落后 |
| kb-56-readufs | `68d1ad7b` | n=48/c=862 无 gate | 有 READUFS，无 panic 槽分离/PANICTRIG/offset |
| kb-57-panicslot | `463ce9c2` | n=48/c=862 无 gate | 有槽分离，无 READUFS offset |
| kb-58-panictrig | `463ce9c2` | n=49/c=880 有 panic gate | 有 gate，不干净 |
| **kb-59-ufsoffset** | **`9cc32cf0`** | **n=48/c=862 无 gate** | ✅ **最远且最干净** |
| kb-60-sysregw | `9cc32cf0` | n=50/c=900 **两个 gate 全开** | 同 kernel，但危险开关开着 |

**选 kb-59**：能力满配，gate 全关。

## 2. 固化信息

| 项 | 值 |
|---|---|
| 包 | `hold/kb-59-ufsoffset/` |
| 不可变快照 | `hold/_baselines/kb-59-ufsoffset/`（kernel + adt + payload）|
| kernel sha16 | `9cc32cf0f07f693f940b` |
| boot-args | n=48 / c=862，**无 `nabu-panic-trig`、无 `nabu-sysreg-write`** |
| debug | `debug=0x34e`（含 `DB_KDP_GETC_ENA`）|
| boot 镜像 | `ead4b53135d67516` / 3,041,280 B（永不重建）|
| 代码锚点 | commit `36d7ca1`，tag `baseline-kb59-ufsoffset` |
| 生成器 | v42（panic 槽分离）|
| board_gate | 已通过（kb-59 轮）|

## 3. 这个基线包含的能力

- ✅ 方案 D 完整：4 MiB BSS 环 → UFS，`opened=1`、`flushed` 前进、周期落盘每 2 秒
- ✅ `READUFS <rel>[@<offset>]`：从 UFS 日志窗读回任意 64 字节（含块内偏移）
- ✅ panic 槽分离：周期 `rel 0/1..25`，panic `rel 8190/8166..8189`，互不覆盖
- ✅ `PANICTRIG`（代码在，但 gate 关，不会误触发）
- ✅ 48/48 verb 全部真机验证过（SYSREGW 是在 kb-60 上验的，但 kernel 与此处相同）
- ✅ 双槽 `is_panic` 语义：`0`=周期、`1`=panic，跨重启存活

## 4. 这个基线**不包含**的（刻意）

- ❌ `nabu-panic-trig=1`（不会误触发 panic）
- ❌ `nabu-sysreg-write=1`（不会误写危险寄存器）

需要用到这两个 gate 时，**单独做一轮加 arg 的包**，用完刷回本基线。这就是过去几轮验证的做法。

## 5. 已知缺口（非阻塞）

| 项 | 归属 |
|---|---|
| panic→KDP 软件退出 | 反编译侧 |
| 路线 A 命令发送（需定命令范围）| 等范围决策 |
| 第二串口 / `debug_terminal.py` 屏幕流 | 等 T40 |
| `debug_terminal.py` 日志 topic 分析 | ✅ 已可用（零改动）|

## 6. 恢复流程

```bash
# 设备回 recovery 后刷回本基线
cd /e/nabu && bash tmp/go_round_next.sh kb-59-ufsoffset

# 核对
sha256sum hold/kb-59-ufsoffset/kernel.macho | cut -c1-16   # 9cc32cf0f07f693f
```

## 7. 历史文档

- `BASELINE-20261123-kb-56-readufs.md` — 上一基线（保留为历史）
- `RESULT-20261123-kb-59-panicstr.md` — 方案 D 闭合
- `RESULT-20261123-kb-60b-sysregw.md` — 最后一个 verb 验证
- `RESULT-20261123-routeA-capture.md` — 路线 A 日志分析
