# 备份说明（2026-11-23）

两套备份，**都带 SHA256 manifest**，任何污染都能 `sha256sum -c` 检出。

---

## 1. 源码 / 设备树活源（已随本仓库提交）

`docs/backup/source/` —— 36 文件，3.5 MB。

这不是整棵源码树（那是 5+ GB 的 build 缓存），而是**所有手工改过的活源**，
足以在干净树上重建当前内核：

```
sources/
  scripts/nabu-panic-log.py      生成器 v42（panic 槽分离 / 两层锁修复）
  board.toml                     设备配置（注意：当前带两个 gate，是验证态）
  drivers/NativeUfs/NativeUfs.cpp  native_ufs_log_read（READUFS 的驱动侧）
  drivers/NabuAdreno/...           GPU profiling / HFI 手工改动
  build/libQcomXnuUsbDebug.a       模块静态库（最新，含 READUFS offset）
  UniXNU-nabu/...                  另一棵树的配置与 .a
  generator-history/               14 个生成器历史版本
xnu-work/                           XNU 编译树 9 个手工改动
  bsd/kern/bsd_init.c              PSCI 手动 REBOOT（不走会卡死的 reboot_kernel）
  osfmk/kern/debug.c              nabu_log_land_now 调用点
  osfmk/console/serial_general.c  落盘逻辑
  pexpert/arm/pe_serial.c 等
module/                             模块仓库 HEAD + tag 锚点 + git status
SHA256SUMS                          全部文件的校验清单
```

**校验**：

```bash
cd docs/backup/source
sha256sum -c SHA256SUMS      # 应全部 : OK
```

**恢复**：把 `sources/` 下文件按相对路径复制回对应根目录，
`xnu-work/` 下文件复制回 `tree/.cache/xnu-work/`，然后重新构建。

⚠️ 注意 `board.toml` 是**验证态**（带 `nabu-panic-trig=1` + `nabu-sysreg-write=1`）。
回滚到干净基线请用 [`BASELINE.md`](BASELINE.md) 的 args（n=48/c=862，无 gate），
不要直接用这份 board.toml 打包。

---

## 2. 编译产物（不上传远端）

7 个基线包 × 6 文件 = 42 文件，约 2.3 GB。
每个 payload.img 192 MB，超过 GitHub 单文件 100 MB 上限，**不能也不适合上传**。

本地路径：

```
E:/nabu/hold/_baseline_archive/20261123/
  SHA256SUMS
  kb-50-readall/  kb-55-alreadyopen/  kb-56-readufs/
  kb-57-panicslot/  kb-58-panictrig/  kb-59-ufsoffset/  kb-60-sysregw/
```

每包含：`kernel.macho` `kernel.macho.adt` `payload.img`
`boot-merge-r3.img`（`ead4b53135d67516`，永不重建）`gpu-addon.bin` `owner-trust.bundle`。

kernel sha16：

| 包 | sha16 | 说明 |
|---|---|---|
| kb-50-readall | `edc6831300eb65d6` | 方案 D 之前 |
| kb-55-alreadyopen | `50c40f72d55ef364` | 落盘打通 |
| kb-56-readufs | `68d1ad7b873fa9b4` | READUFS |
| kb-57-panicslot | `463ce9c2df9ace5d` | 槽位分离 |
| kb-58-panictrig | `463ce9c2df9ace5d` | is_panic=1 跨重启 |
| **kb-59-ufsoffset** | **`9cc32cf0f07f693f`** | **当前基线** |
| kb-60-sysregw | `9cc32cf0f07f693f` | 同 kernel，带验证 gate |

校验（在有归档的机器上）：

```bash
cd _baseline_archive/20261123
sha256sum -c SHA256SUMS
```

---

## 3. git 锚点（远端可查）

```
HEAD   36d7ca1   readufs: add block-internal offset to reach PANICSTR
tags:  baseline-kb56-readufs
       baseline-kb59-ufsoffset   ← 当前基线
       backup-20261122-plan-implement
```

远端：`https://github.com/Lux12138/nabu-usb-cdc`
