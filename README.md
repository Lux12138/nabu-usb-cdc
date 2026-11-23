# nabu USB CDC 控制台

Xiaomi Pad 5（nabu / SM8150）上的 XNU 17.7.11 调试控制台。
主机看到一个 USB CDC 串口：实时读内核日志，同时反向发控制命令。

目标板：Xiaomi Pad 5 · SoC SM8150 · XNU 17.7.11 (iPadOS 21H461)
代码仓库：[Lux12138/nabu-usb-cdc](https://github.com/Lux12138/nabu-usb-cdc)

---

## 现在的状态（2026-11-23）

**48 个命令全部真机验证，panic 日志跨重启存活。**

| 指标 | 状态 |
|---|---|
| 命令总数 | **48**，全部真机验证 |
| 内核日志 | 开机到复位连续，无需第二串口 |
| 读内存 | 内核虚拟地址 + 物理地址/MMIO，带白名单防护 |
| 写内存 | `WRITEPA` 白名单内已验证 |
| panic 日志 | **BSS 环 → UFS，`PANICSTR` 从介质读回，跨断电重启存活** |
| 链路 | CDC 约 42–57 s 枚举，命令往返正常 |

**最新可用基线**：`baseline-kb59-ufsoffset` tag（kernel `9cc32cf0f07f693f`，boot-args 无危险 gate）。
详细基线说明见 [`docs/BASELINE.md`](docs/BASELINE.md)。

---

## 快速开始

### 1. 单元测试（可选）

```bash
cd module
clang -std=c11 -D_GNU_SOURCE -Wall -Wextra -I. \
  tests/test_usb_control_args.c -o /tmp/t && /tmp/t   # 应 ALL PASS
```

### 2. 上板一次（先起控制台，再刷机）

```bash
# 终端 1：控制台（务必在刷机前启动）
cd tools
python -u nabu_console.py --tag run1 --out /path/to/logs \
  --wait 300 --deadline 900 --kernel /path/to/kernel.macho

# 终端 2：刷机
cd /path/to/build
bash go_round_next.sh <包名>
```

控制台每 2 秒扫描端口，发现 COM3 后打开，日志写入 `<tag>-com.bin`，
命令写入 `<tag>-cmd.txt`（**只追加，勿截断**）。

### 3. 发命令

向 `<tag>-cmd.txt` 追加一行即可：

```
hello
ptdump _gPhysBase
readpa 0x17c10008 16
sysreg TCR_EL1
readufs panic
```

回复写入 `<tag>-ctl.txt`。`readufs panic` 读回 panic 槽头；
`readufs 8166@4032` 读 panic 文本所在的块尾。

---

## 命令一览（48 个）

| 类别 | 命令 |
|---|---|
| 基础 | `HELLO` `PING` `STATUS` `BOOTARGS` `VERSION` `UPTIME` `REBOOT` |
| 内存读 | `READ` `READNEXT` `READPA` `READPANEXT` `RDBLK` `DUMP` `DUMPPA` |
| 内存写 | `WRITEPA` `WRITE` |
| UFS | `DUMPUFS` `DUMLIST` `READUFS` |
| 调试 | `DEBUGGER` `KDPPUSH` `KDPREAD` `KDPWRITE` `KDPREBOOT` `KDPRAW` |
| 页表 / panic | `PTDUMP` `PANICDUMP` `PANICTRIG` |
| 系统信息 | `SYSCTL` `TASKDUMP` `THREADDUMP` `SYSREG` `SYSREGW` `SYM` `SYMS` |
| 显示 | `SCREENSHOT` `SHOTNEXT` `VSYNCON` `FLIPNOW` `DISPREGS` `SMMUDUMP` |
| GPU | `GPUPROFILE` `GPUFREQ` `GPUHWCG` `GMUSTATE` `PERFSTART` `PERFSTOP` `PERFREAD` `PERFCLEAR` |
| 断点 | `BKPADD` `BKPDEL` `BKPLIST` |

`SYM`/`SYMS` 按设计回 `unsupported`（符号表在主机侧解析）。
`PANICTRIG` 和 `SYSREGW` 是破坏性命令，各有 boot-arg 安全阀，默认关闭。

---

## panic 日志（方案 D）

内核已有 4 MiB BSS 环；本仓库把它接到 UFS 预留区：

```
内核输出 → nabu_log_ring (BSS) → 每 2 秒落盘 → UFS userdata 末尾
```

panic 时 `nabu_log_land_now()` 强制落盘到**专用槽**，与周期槽分离：

| 槽 | 块偏移 | 内容 |
|---|---|---|
| 周期 | `rel 0` | `NABULOG1` 头（`is_panic=0`）|
| 周期 | `rel 1..25` | 最近日志 |
| panic | `rel 8190` | `NABULOG1` 头（`is_panic=1`）|
| panic | `rel 8166..8189` | panic 时刻的日志尾部 |

读回：

```
readufs header            # 周期槽头，is_panic 应为 0
readufs panic             # panic 槽头，is_panic 应为 1
readufs 8166@4032         # panic 文本（PANICSTR: ...）在最新尾部块的最后 64 字节
```

`NABULOG1` 头布局（小端）：magic(8) + version(4) + is_panic(4) +
available(4) + dropped(4) + total_lo(4) + total_hi(4)。

方案 A（外部保留区 `0x9b000000`）已作废：两轮真机证明该地址无后备物理页。

---

## 重要操作纪律

1. **一个 tag 用到底**：`<tag>-cmd.txt` 只用 `>>` 追加，不截断。要重来就换 tag。
2. **一个控制台、一个句柄**：不要同时开第二个串口工具。
3. **先起控制台，再刷机**：否则早期日志全丢。
4. **复位判据**：COM3 消失 **且** adb 出现 `f91fa664 recovery`；`adb devices` 空不算。
5. **复位发 `reboot`**（走 PSCI，可靠）；**禁 `--arm-at` 自动复位**。
6. **panic 后进 KDP 退不出来**：已知限制（已交反编译侧排查），只能断电。
7. **写危险寄存器前先看原值**：像 `0xfffffff9...` 这种内核指针，写随机值会直接卡死内核。

更完整的纪律见 [`docs/DISCIPLINE.md`](docs/DISCIPLINE.md)。

---

## 目录

| 路径 | 内容 |
|---|---|
| `tools/nabu_console.py` | **唯一控制台**：日志采集 + 命令 + KDP |
| `tools/pipeline.py` | 日志分析流水线 |
| `tools/symtab.py` | 主机侧符号表（53k 符号）|
| `module/kernel/` | 控制协议与 verb 实现（C）|
| `module/tests/` | 命令解析单元测试 |
| `patches/` | 内核/驱动补丁 |
| `docs/` | 详细文档、历史记录、备份说明 |

---

## 备份

| 内容 | 位置 | 远端 |
|---|---|---|
| 源码 / 设备树活源 | `docs/backup/source/`（本仓库）| ✅ 已提交 |
| 编译产物（7 基线，2.3 GB）| 本地 `hold/_baseline_archive/20261123/` | ❌ 太大，不上传；见 [`docs/BACKUP.md`](docs/BACKUP.md) |

---

## 背景

- 项目起点：把 DWC3 USB 控制器做成 XNU 上的 CDC ACM 控制台，替代刷探针调试。
- 早期历史（已撤回的判断、黑洞争议等）见 [`docs/README-history-20261122.md`](docs/README-history-20261122.md)。
- 反编译侧协作记录见 `E:/nabu/state/SYNC-TO-RE-*.md`（本机文档）。

## 许可

见 [LICENSE](LICENSE)。`module/` 基于
[xiaoleGun/QcomXnuUsbDebug](https://github.com/xiaoleGun/QcomXnuUsbDebug)（MIT），改动见 `docs/module-changes.patch`。
部分分析参考 m1n1（GPL-2.0），仅吸收方法，未整文件拷贝。所有内容仅用于自有设备的移植研究。
