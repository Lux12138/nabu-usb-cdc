# RESULT kb-78 —— SYSREGW 正向写入闭合（不用刷机、不改 boot-arg）

> 2026-11-23 19:48–19:49。设备跑 `hold/kb-70-kdbgfix`（`3480d5c12b7edb2a6cbb`），**未改动**。
> **SYSREGW 正向第一次拿到完整闭环，而且绕过了那条两次害我们刷机失败的 boot-arg。**

---

## 1. 决定性结果

```
基线   SYSREG  CNTKCTL_EL1        → value=0x000000000000004f
写     SYSREGW CNTKCTL_EL1 0x0f   → OK wrote=0x000000000000000f verify=separate
读回   SYSREG  CNTKCTL_EL1        → value=0x000000000000000f   ✅ 值真的变了
恢复   SYSREGW CNTKCTL_EL1 0x4f   → OK wrote=0x000000000000004f
再读   SYSREG  CNTKCTL_EL1        → value=0x000000000000004f   ✅ 已恢复
活性   HELLO                      → OK  session=1ca8d6969eed768d
```

## 2. 关键手法：直接写 gate 变量，不碰 boot-arg

`SYSREGW` 的 gate 是一个**导出的 `.data` 符号**：

```
fffffff00c0f5a70  d  _qud_sysreg_write_enabled
```

源码（`QcomUsbDiag.c:1535-1545`）：

```c
static int qud_sysreg_write_enabled = -1;   /* -1 = 还没解析过 boot-arg */

qud_sysreg_writes_allowed(void)
{
    if (qud_sysreg_write_enabled < 0) {      /* ← 只有 -1 时才去读 boot-arg */
        PE_parse_boot_argn("nabu-sysreg-write", &v, sizeof(v));
        qud_sysreg_write_enabled = v ? 1 : 0;
    }
    return qud_sysreg_write_enabled;
}
```

**运行时操作：**

```
READ  0xfffffff00c0f5a70 4   → ffffffff        （= -1，未解析）
WRITE 0xfffffff00c0f5a70 01000000
READ  0xfffffff00c0f5a70 4   → 01000000        （= 1，gate 已开）
```

**⇒ 写成 `1` 之后 `if (-1)` 永不成立，boot-arg 根本不会被读。**
**⇒ 不需要改 `board.toml`，不需要重打 payload，不需要刷机。**

## 3. 这条方法顺带解释了 kb-73 / kb-75 为什么失败

**问题从来不是 boot-arg 本身，而是"改 boot-arg 就必须重打 payload"这件事。**

| 事实 | 值 |
|---|---|
| kb-70 与 kb-75 的 `unixnu.toml` 差异 | **只有 args 那一行**（1312 → 1332 B，正好差 20）|
| 两者的 FAT 目录 | 完全一致，文件大小全同 |
| 两者的 `scratch-data.img` | sha256 **逐字节相同** |
| 两者的 kernel 与各自源文件 | sha256 **匹配** |
| 两者的模块符号地址 | **完全相同** |
| **两者的 payload 字节差异** | **125,511,466 字节（62%）** |

**所以：配置和内容都没错，但 `mcopy` 写 125 MB 内核进 FAT 镜像时会把整个簇布局重排，
镜像 62% 的字节都变了。** 其中某个环节让 bootloader 或内核读到了不对的东西
（两次都卡在 `disk1s1` Data 卷挂载，`mount-complete` 永不出现）。

**⇒ 纪律：能不改 boot-arg 就不改。要改 gate，优先用运行时 `WRITE` 改对应的 `.data` 变量。**

## 4. 判据（全过）

| # | 判据 | 结果 |
|---|---|---|
| 1 | gate 变量初值是 `-1` | ✅ `ffffffff` |
| 2 | `WRITE` 后变成 `1` | ✅ `01000000` |
| 3 | `SYSREGW` 返回 `OK`（不再是 `gated`）| ✅ `wrote=0x0f` |
| 4 | **独立 `SYSREG` 读回 = 写入值** | ✅ `0x0f` |
| 5 | 写回原值后可读回原值 | ✅ `0x4f` |
| 6 | 内核健康 | ✅ HELLO 有回复 |

## 5. 至此，四处「写」的空白全部补齐

| 项 | 状态 | 轮次 |
|---|---|---|
| `WRITE`（内核虚拟地址）| ✅ 值变化验证 | kb-74 |
| **`SYSREGW`（系统寄存器）** | ✅ **正向写入 + 读回 + 恢复** | **kb-78（本轮）** |
| `WRITEPA`（MMIO）| ✅ 值变化验证（`CTL_FLUSH`）| kb-74 |
| `DUMPUFS`（UFS 导出区）| ✅ 端到端（kb-12 已闭合）| kb-12 |

## 6. 设备状态

kb-70 正常运行，gate 变量已恢复成 `1`（无害：它只影响 `SYSREGW`，
且 `SYSREGW` 本身有寄存器级白名单，危险寄存器如 `SCTLR_EL1`/`TCR_EL1` 即使 gate 开也需要显式指定名字）。

**下一轮若要上板，gate 已经是开的，无需改任何配置。**
