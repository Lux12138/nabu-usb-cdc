# 内核移植 patch（相对 XNU 上游 d8b8029）

这些 patch 由 `git diff` 从真实的编译源
`out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/.cache/xnu-work` 导出。

## 为什么需要它们

`.cache/xnu-work` 是 `unixnu tree` 会**重建**的目录。
重建后所有手工改动会丢失（2026-11-23 已因此踩坑一次，白烧两轮编译）。

**⇒ `unixnu tree` 之后必须重新应用这些 patch。**

## 应用

```bash
W=/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/.cache/xnu-work
cd "$W"
git apply /mnt/d/_nabu-usb-cdc/patches/0001-debug-verbose-and-arm64-gate.patch
git apply /mnt/d/_nabu-usb-cdc/patches/0002-kdp-serial-reply-tx.patch
git apply /mnt/d/_nabu-usb-cdc/patches/0003-pe-serial-cdc-tx-gate.patch
# 0004 是驱动，不在 xnu-work：
patch -p0 -d /root/merge/l738-device/drivers/NativeUfs \
      < /mnt/d/_nabu-usb-cdc/patches/0004-native-ufs-wdt-off.patch
# 同步到第二棵树
cp /root/merge/l738-device/drivers/NativeUfs/NativeUfs.cpp \
   /root/merge/UniXNU-nabu/drivers/NativeUfs/NativeUfs.cpp
```

## 每个 patch 的作用

### 0001-debug-verbose-and-arm64-gate.patch — `osfmk/kern/debug.c`

解决 **KDP 无法退出**的两个根因：

1. **`static volatile boolean_t debugger_is_panic`**
   原来没有 `volatile`，编译器把它当译文单元内已知常量，
   把 `if (cond) debugger_is_panic = FALSE;` **整块删除**。
   证据：链接后 `_panic_init` 只有一处 `strb #1`（写 TRUE），没有 `strb #0`；
   哨兵实验（`= 0x42`）也看不到该常量。
   加 `volatile` 后：变量从 `__bss` 移到 `__data`，出现 `str wzr`（写 0），
   `strb` 变成正确的 `str`（4 字节）。

2. **arm64 gate 去掉 `PE_i_can_has_debugger()`**
   该函数返回 `debug_enabled`，本板实测 **0** ⇒ 整个条件恒假。
   arm64 分支改为只用 `kdp_explicitly_requested`（只由 `kdp_match_name=` 置位，
   看门狗触发的 `Debugger()` 仍是 0，panic log 照写）。

同时包含 `qcom_usb_debug_panic_prepare()` 等移植挂钩。

### 0002-kdp-serial-reply-tx.patch — `osfmk/kdp/kdp_udp.c`

1. **`reattach_wait` 守卫扩展到所有架构**（plan B #1）
   上游只在 x86 编译这个守卫，arm64 上 `goto again` 无条件执行 ⇒
   会话永远无法返回内核。

2. **KDP 回复的发送门** `KDP_TX_BEGIN()/END()`
   `kdp_serial_send()` 用它包住 `kdp_serialize_packet()`，
   使 CDC 后端的 `transmit_data()` 只在回复序列化期间转发。
   否则 KDP 回复走 `pal_serial_putc_nocr → uart_putc → gPESF →` 空实现，全部丢弃。

### 0003-pe-serial-cdc-tx-gate.patch — `pexpert/arm/pe_serial.c`

CDC 串口后端：`transmit_data()` 只在 `qcom_usb_debug_kdp_tx_active()` 为真时转发，
其余时间为空操作（防止 `kprintf` 重入 USB 写路径 —— 曾两次导致启动期死亡）。

同时含 T37 的 `nabu_cdc_rx_attach()` 幂等注册门
（`serial_init()` 每启动调 ≥3 次，重复注册会让 `gPESF` 自环）。

### 0004-native-ufs-wdt-off.patch — `drivers/NativeUfs/NativeUfs.cpp`

`nabu_wdt_arm()` 开头检查 `nabu-wdt-off` boot-arg，为真则直接 return
（在 `EN 0->1` 之前，所以 bootloader 留下的 deadline 已被调用者清掉）。

**注意**：`nabu-wdt-arm-delay` **不是**开关，它只决定何时武装。
`86400` 只是把咬推迟 24 小时。真开关是本 patch 的 `nabu-wdt-off`。

**验证**：CDC 日志出现 `ufs:wdt-off`；或读 `0x17C10000+0x08` 应为 0。

## 验证过的状态（2026-11-23）

| 项 | 结果 |
|---|---|
| KDP CONNECT / READMEM64 / DISCONNECT | ✅ |
| DISCONNECT 后内核继续运行 | ✅ |
| `debugger_is_panic` = 0 | ✅ |
| `0x17C10000+0x08`（WDT EN）= 0 | ✅ |

详细文档见 `E:\nabu\state\ROOTCAUSE-kb36-static-volatile.md` 与
`RESULT-20261123-kb-38-baseline.md`。
