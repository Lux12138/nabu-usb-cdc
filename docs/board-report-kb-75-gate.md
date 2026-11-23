# board-report kb-75 —— 只加 `nabu-sysreg-write=1`（二分 kb-73 的启动失败）

> 基线：`hold/kb-70-kdbgfix`（`3480d5c12b7edb2a6cbb`）
> 增量：**只有一条 boot-arg**。白名单代码与 kb-70 完全相同。

---

## 1. 唯一要回答的问题

**`nabu-sysreg-write=1` 本身会不会导致启动失败？**

### 1.1 为什么这么问

kb-73 启动失败（`nabu:` 只出 6 行就静默，卡在 root owner 认证之后、scanout 交接之前）。
那一轮同时改了两处：GCC 白名单移除 + 加 gate。必须二分。

**本轮把 GCC 白名单恢复成 kb-70 的样子**，只留 gate 这一个变量。

### 1.2 已有的排除（来自 kb-74 的核对）

| 归因 | 排除依据 |
|---|---|
| `nabu-sysreg-write=1` 影响启动 | `qud_sysreg_writes_allowed()` 全文只有一个调用点（`QcomUsbDiag.c:1632`，`SYSREGW` 路径），**启动期无调用者** |
| GCC 白名单移除影响启动 | `qud_read_whitelist[]` **只在 `READPA` 时被查询**，启动期不碰 |
| payload 损坏 | `scratch-data.img` 与 kb-70 **sha 逐字节相同** |
| args 字符串损坏 | payload 内按 FAT 路径读出 51 条完整，无截断 |

**⇒ 三个可能：① kb-73 是偶发 ② args 条数/顺序有隐性上限 ③ 我们还没想到的东西。**

## 2. 改动（一处）

`board.toml` 两棵树都加：

```toml
"qcom-usb-write=1",
"nabu-sysreg-write=1",   ← 新增，只这一行
```

**module 代码**：GCC 白名单已恢复到 kb-70 状态（含注释说明为什么暂时放回来）。
`libQcomXnuUsbDebug.a` = `8a6b325641b56568`。

## 3. 判据（4 条）

| # | 判据 | 期望 |
|---|---|---|
| 1 | **正常启动到用户态** | `nabu:` 行数 **数百**（kb-73 只有 6），出现 `nabu: scanout hand-off` |
| 2 | CDC 枚举 | 40–70 s |
| 3 | **`SYSREGW CNTKCTL_EL1 0x0f`** | `OK wrote=0x000000000000000f`（**不再是 gated**）|
| 4 | `SYSREG CNTKCTL_EL1` 读回 + 写回 `0x4f` | `0x0f` → `0x4f`，HELLO 健康 |

## 4. 失败也有信息（这是二分，失败也能定位）

| 现象 | 结论 |
|---|---|
| **正常启动 + `SYSREGW` 成功** | ✅ **kb-73 的失败是「GCC 白名单移除」或偶发**。gate 无辜，SYSREGW 正向达成 |
| **正常启动 + `SYSREGW` 仍 gated** | gate 没进 payload（查 `args_count` 与 `mtype` 直读）|
| **卡在与 kb-73 完全相同的点**（`nabu:` 6 行静默）| **`nabu-sysreg-write=1` 有隐性影响**，需要查 `PE_parse_boot_argn` 对非法/多余 token 的行为 |
| 卡在别处 | 新问题，单独记 |

## 5. 唯一变量 + 回滚点

**唯一变量**：`nabu-sysreg-write=1`（module 代码与 kb-70 一致）。
**回滚点**：`hold/kb-70-kdbgfix`（`3480d5c12b7edb2a6cbb`）——**已在 kb-74 上板验证健康**。

## 6. 已闭合

| 项 | 证据 |
|---|---|
| kb-70 基线健康 | kb-74 实测：HELLO 秒回、`nabu:` 日志数百行、`WRITEPA` 可用、显示 `nswap 2→5` |
| **`WRITEPA` 正向值变化** | kb-74 实测闭合（`CTL_FLUSH` 写 `0x3`，硬件消费，见 `RESULT-kb74-writepa-positive.md`）|
| 模块单测 | `ALL PASS` |
| 模块编译 | 零 `error:` |
| 改前快照 | `hold/_snapshots/20261123-kb73/` |

## 7. 打包注意（kb-73 踩过的坑）

`pack_kb56.sh` 的 `BASE` 指向 **kb-50**，args 是旧的 `n=48 c=862`。
**必须**先用当前基线 payload 覆盖，再 `mcopy` kernel，再 `sync_payload_args.py`。
验收：`args 条数 == board.toml 条目数`（本轮应为 **51**）。
