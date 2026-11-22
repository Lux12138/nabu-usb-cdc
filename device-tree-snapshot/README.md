# 设备树改动快照（非本仓库源码，供移植/回滚）

这些文件**不是**本仓库（USB 控制台模块）的源码，而是**设备编译树（WSL `/root/merge/*`）里手工改动**的归档。
因为设备树不在 git 管理下，改动会随 `unixnu tree` 重建而丢失，所以在这里留一份可追溯的副本。

快照时间：**2026-11-23 00:38**（对应内核 `kb-11-perf`，sha16 `9d5ef933`）

## drivers/ — 两个 Adreno 驱动文件

| 文件 | 改了什么 | 对应阶段 |
|---|---|---|
| `NativeAdrenoService.cpp` | `profileStartWaitMs` 递减到 0 时调 `native_adreno_gpu_profile_start(3000)`（之前归零后什么都不做 → profiling 从未真正启用） | S1 |
| `NativeAdrenoGpuProfile.cpp` | ① 新增 `native_adreno_gpu_profile_read()`（PERFREAD 的数据源）② `releaseFinishedWindow()` 里窗口结束后自动 `dump()`（之前采样数据留在 ring 里从不打印） | S4 |
| `NativeAdrenoGpuProfile.h` | read 接口声明 | S4 |

部署位置：`/root/merge/{l738-device,UniXNU-nabu}/drivers/NabuAdreno/`（两份必须一致；编译树用 symlink 指向 `UniXNU-nabu` 那份）

## xnu-work/ — XNU 侧改动

| 文件 | 改了什么 |
|---|---|
| `bsd_init.c` | reboot 线程在 `arm_reboot()` 成功后改调 `qcom_usb_debug_platform_reset()`（PSCI SYSTEM_RESET），不再走 `reboot_kernel()`——后者在本移植上会卡死在 `vfs_unmountall()` |
| `bsd_init.psci.diff` | 上述改动的 unified diff（相对 0006 补丁应用后的原文） |

部署位置：`out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/.cache/xnu-work/bsd/kern/bsd_init.c`
⚠️ **此改动只在编译树里，不在 `patches/xnu/series`**。若 `unixnu tree` 重建，需要重新应用（或正式做成 0018 补丁）。

## 配套的内核侧改动（不在此目录，在编译树）

- `pexpert/arm/pe_serial.c` / `pexpert/pexpert/arm/protos.h`：T30 的 pe_serial CDC 注册（`SERIAL_NABU_CDC=0x40`）。
  **注意：快照时发现这两处实际未进入编译树**（`nabu_cdc_serial_functions=0`），T30 只有模块侧生效。
  详见 `MANIFEST.txt` 的"快照时发现的事实"。

## 重建提示

完整重建顺序见 `MANIFEST.txt`。核心要点：
1. 先 `unixnu tree`（若需要）
2. **同步 drivers 改动到两棵树**（否则 `build-nabu-adreno-step1.py` 编的是树里副本）
3. 应用 `bsd_init.c` PSCI 改动（若树被重建）
4. `unixnu build kernel`
5. NOP18 → 打包 → gate → 上板

## 2026-11-23 01:15 追加：GPU 运行时控制（GPUFREQ / GPUHWCG）

| 文件 | 改了什么 |
|---|---|
| `NativeAdrenoService.cpp` | ① `A640State::setHwcg(bool)` + `native_adreno_set_hwcg()`（GPUHWCG）② `A640State::powerTables()` + `native_adreno_get_gpu_freq()`（GPUFREQ 读驱动性能表） |
| `NativeAdrenoA6xxHwInit.cpp/.h` | `setClockGating(io, enable)`：enable=false 把每个 HWCG 寄存器写 0，true 重放原表 |

**关键教训**：`ml_io_map(0x2c90000)`（GPUCC）在 **GPU 未上电时返回不可访问的窗口**，直接解引用 = Unaligned kernel data abort（实测 panic 两次）。
⇒ 读 GPU 寄存器必须走驱动接口（它自己管电源和状态），不要裸 ml_io_map。
MDSS（0xae01000）不受影响，一直可读。
