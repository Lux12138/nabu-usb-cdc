# T59 · USB CDC 软件侧提速补丁包（S1 多 TRB 环 + S2 事件预算）

- 日期：2026-11-24
- 性质：实现类交付（补丁文本 + `git apply --check` + 交叉编译 + 主机模型验证）。**未修改 `D:\_nabu-usb-cdc` 工作树，未上板。**
- 交付物：`usb-sw-throughput.patch`、本 `README.md`。（原 QSCRATCH/PHY 方案的 `usb-fs-to-hs.patch`/`usb-hs-probe.patch` 已作废删除：红线 R29 禁止在 USB 活动期访问 `QSCRATCH_GENERAL_CFG(0x0a6f8808)`。）

---

## 0. 前提更正（重要）

主线 2026-11-24 22:44 上板实测推翻了 T55 的核心前提：

- 运行时 `DSTS(0x0a60c70c)=0x00025610`，`CONNECTSPD[2:0]=0`，**链路本来就是 High Speed**；T55 里「CONNECTSPD=1」是 prepare 期端口复位前的陈旧回读。
- 62 KB/s 的真实来源是**软件节拍**：非调试器态每 tick 只处理 **1 个事件**（`dwc3_poll(...,1)`）、每端点只有 **1 个 TRB 在飞**（`active[]` 拒绝排队），发完必须等 XFERCOMPLETE；实测 ~15.1 transfers/s × 4096 B 即 62 KB/s。
- 因此本包不再做任何 PHY/寄存器改动，只做**软件并发度**提升。

补丁触及 3 个文件：`module/usb/Dwc3.c`、`module/usb/Dwc3.h`、`module/kernel/QcomXnuUsbDebug.c`。**不含任何 QSCRATCH/PHY 访问**（已 grep 确认）。

---

## 1. 这版做了什么

### S1 · IN 多 TRB 环（boot-arg `nabu-usb-s1=1`，默认关）

现状：`pump()` 在 `d->active[EP_IN]` 时直接返回，`transfer()` 每端点只有一份 TRB（`Dwc3.c` 的 `trb(d,ep)`），所以同时最多一笔 IN 传输。

改动：

- DMA 布局**零扩容**：把 EP_IN 原有的 4096 B 缓冲切成 **8 个 512 B 槽**；TRB 环放在原本空置的间隙（`TRB_OFFSET + 6*64 = 4480`，8×64=512 B，仍在 `BUFFER_OFFSET=8192` 之前）。
- 新增 `dwc3_set_in_ring(dev, enable)`：必须在 `dwc3_init()` 之后、`dwc3_start()` 之前调用；运行中拒绝切换（有在飞槽时返回 -1）。
- `in_ring=1` 时 `pump()` 循环填充，保持最多 8 笔 IN 传输在飞；`handle_event()` 用 `in_complete()` 按**发出顺序**完成并回记字节；复位/停止走 `in_reset()`（保留 tx 环，与既有语义一致）。
- `in_ring=0`（默认）时代码路径与原来**逐语句相同**：仍是单 TRB、`tx_used` 在完成时才回记。

记账要点：tx 环的字节直到对应槽完成才扣减（与单 TRB 路径同语义）；`in_fetch/in_outstanding` 是环私有的拷贝游标。**residual≠0 在环模式下视为控制器故障（-8）**——部分传输会破坏槽间顺序，宁可显式失败也不静默错序；正常 HS bulk 不会产生 residual。

### S2 · 非调试器事件预算（boot-arg `nabu-usb-s2=N`，N∈[1,64]，默认=当前值 8）

`QcomXnuUsbDebug.c` 的 `poll_owned()` 里 `dwc3_poll(..., kdp_rx_armed ? 64 : <budget>)`。本包把它变成变量 `usb_sw_poll_budget`，默认取**当前工作树里的值 8**（保留并发同事未提交的修改，不覆盖），`nabu-usb-s2=N` 可覆盖（越界忽略）。调试器态仍是 64，不受影响。

> 注：工作树里已存在一处未提交改动把该值从 1 改成 8 并附有实测注释（1 KiB 包在 budget=1 下要 2.7 tick）。本包以此为基线，不回复它。

### 默认关的等价性

两个开关都是模块级静态量（`usb_tx_ring=0`、`usb_sw_poll_budget=8`），无 boot-arg 时：S1 完全走原单 TRB 路径；S2 使用现有预算 8（即当前行为）。主机模型实测（fake GCC/DWC3/PHY）：

```
default: one TRB armed, size=4096 queued=6000
PASS default single-TRB path
ring: 8 slots armed, in flight=4096 queued=12000
PASS ring: 24 transfers drained, zero residue
PASS ring: packet-aligned ZLP
PASS ring: bus reset preserves tx ring and clears ring
harness passed
```

即关闭时单 TRB 记账不变；开启时 8 槽在飞、24 块按序排空为零残留、ZLP 语义保留、bus reset 保留 tx 环。

---

## 2. 应用顺序

```sh
cd /mnt/d/_nabu-usb-cdc
git apply --check /mnt/e/nabu/state/bridge/answers/T59/usb-sw-throughput.patch   # 必须先过
git apply           /mnt/e/nabu/state/bridge/answers/T59/usb-sw-throughput.patch
# 编译（§3）→ 上板：boot-args 增加 nabu-usb-s1=1 （先单开 S1）
# 需要复现基线时：去掉该 boot-arg 即可（S2 默认=当前行为）
# 回滚源码：git apply -R /mnt/e/nabu/state/bridge/answers/T59/usb-sw-throughput.patch
```

建议轮次（单变量）：① 不打补丁基线 → ② 只 `nabu-usb-s2=8`（若树里已是 8，则基线即此档）→ ③ 只 `nabu-usb-s1=1` → ④ 两者同时。每轮用同一导出脚本、同一 payload 尺寸。

> 补丁是对**当前工作树**（含他人未提交的 `control_reboot_fallback` 等改动）生成的，`git apply --check` 已对该状态通过；那些改动若先被提交，内容不变仍可应用。

---

## 3. 编译接入

**不需要改 `module/scripts/build.py`**：`usb/Dwc3.c` 已在 `UNITS`；新增的 `dwc3_set_in_ring` 是模块内部非 static 符号，由同归档的 `QcomXnuUsbDebug.c` 引用；共享 DWC3 无新增外部导入（仅用已有 `memcpy/memset` 类内置）。无新增白名单需求。

```sh
python3 module/scripts/build.py \
  --compile-record <xnu>/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_kprintf.o.json \
  --soc soc/Sm8150.h --output <workspace>/tmp/<new-dir>
```

本次实际记录：`/root/merge/l738-device/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_kprintf.o.json`。结果：**error 数 = 0**，`libQcomXnuUsbDebug.a` 生成（8 个单元日志全 0 error）。模块自带 `tests/test_dwc3.c` 模型（默认路径）全过；`tests/check.py` 因本机缺 clang ASan 运行库无法整体执行（与补丁无关）。

---

## 4. 上板验证协议（精简）

通用前提：只刷 cust、`fastboot boot`；WDT 兜底；单变量；等待 10 s 轮询；**严禁读写 `0x3c0fc000`、严禁 erase/flash boot/boot_a**；**严禁在 USB 活动期访问 `QSCRATCH`(R29)**。

| # | 配置 | 动作 | 判据 / 期望 |
|---|---|---|---|
| **V1** | 不打补丁（或去掉新 boot-arg） | 固定 payload（如 4 MiB `READPHYSMEM64` 裸流或一次 `SCREENSHOT`）走同一脚本，记录吞吐与 STATUS 计数 | 基线：与历史 62 KB/s 同档；`dropped/rx_overflow` 不增；作为对照 |
| **V2** | 仅 `nabu-usb-s2=8`（若树内已是 8，改试 16/32） | 同 V1 | 控制类回复（1 KiB 包）单 tick 完成率上升；大 payload 吞吐应明显高于 V1；无 STALL/丢包 |
| **V3** | 仅 `nabu-usb-s1=1` | 同 V1 | 大 payload 持续吞吐提升；STATUS 的 `transport_pending` 峰值更高但 `dropped` 不增；TX 内容校验（CRC/快照像素）与 V1 一致 |
| **V4** | `nabu-usb-s1=1` + `nabu-usb-s2=8` | 同 V1 | 取三者最优且不劣化；记录最终稳态 B/s |
| **回归** | 去掉所有新 boot-arg | 重复 V1 | 与 V1 数值同档（证明默认路径未被改变） |

停机判据（任一成立立即拔线/回退）：COM 口消失、`lt`/`ta` 显示 STALL 增多、STATUS `dropped`/`rx_overflow` 增长、导出内容 CRC 不符、panic/看门狗咬回。

---

## 5. 预期收益

【推断】S1 把「每 tick 最多一笔」变成「常数笔在飞」，消除重 arm 间隙；S2 让一次 poll 排空多个完成事件并立即续发。二者对**突发导出**（截图、内存 dump、panic ring）收益最大；稳态还受主机 CDC 读取节拍限制。参照树内实测（budget 1→8 使 1 KiB 包从 2.7 tick 降到 1 tick）与单 TRB 结构，保守预期数倍量级提升；具体倍数以上板 V2–V4 为准，本包不做先验承诺。

---

## 6. 风险与回滚（务必先读）

- **S1/S2 动的是控制器状态机**。历史前科：T35-s3「TX 环占用导致 reply 丢失」。本包用 boot-arg 把新路径完全隔离，默认路径逐语句不变；但一旦开启，`active[]`→`in_active[]`、`tx_tail` 记账时机、ZLP 判定都换了实现。
- 环模式**要求按序完成**且 **residual≠0 即判故障（-8 → worker 停）**。这是刻意选择：部分传输无法在不错序的前提下回滚槽间记账。正常 HS bulk 无 residual；若上板因此停机，说明链路已有真实故障。
- 8×512 B 槽位复用 EP_IN 的 4096 B 缓冲与 TRB 间隙，**DMA 总尺寸不变**（32768 B），但若将来有人改动 `EP_BUFFER_BYTES/TRB_OFFSET/BUFFER_OFFSET`，需重新核对布局。
- 回滚：删 `nabu-usb-s1=1`/`nabu-usb-s2=N` 重启即回原行为；源码 `git apply -R`（apply→reverse 往返已在临时仓库验证）。运行中的内核不能在线卸载。
- 上板顺序：先 WDT 兜底轮次做**单变量**验证；每次先备份可读寄存器状态；异常优先保调试口。

---

## 7. 证据分级

【实证】
- 链路为 HS（主线运行时 `DSTS=0x00025610`，`CONNECTSPD=0`）；62 KB/s 的软件归因（budget=1、单 TRB、~15.1 transfers/s）。
- 代码事实：`dwc3_poll(...,1)`（非调试器）、`active[EP_IN]` 拒绝排队、单 TRB/端点、worker 5 ms tick（`patches/darwin22/0002`）、`SOC_USB_LOG_DRAIN_BUDGET=4096`（未在本包改动）。
- 交付验证：`git apply --check` 通过；apply→reverse 往返通过；build.py 两变体（默认/打补丁）error=0；`test_dwc3` 默认路径全过；自建主机模型验证 S1 的 8 槽在飞、按序排空、ZLP、reset 语义，及默认路径等价。

【推断】
- S1+S2 可带来数倍突发吞吐提升；具体倍数取决于主机 CDC 读取节拍与 payload 形状。

【未定位 / 未做】
- S3（worker 5 ms→1 ms，位于设备树 `patches/darwin22/0002`）、S4（drain 4096→16384，`QcomUsbBuffer.h`）、S5（描述符 high-bandwidth，`wMPS` bits[12:11]）**本包未实现**，留作后续批次；它们各自也会改变节流/描述符语义，需单独门控与验证。
- 环深度 8 与槽长 512 为固定常量，未做参数化/自动调优。
- 主机侧 CDC 实际轮询周期未测；若干轮后吞吐仍不达标，应先怀疑主机节拍而非继续加设备侧并发。

---

## 8. 附：关键引用

- `MODULE/usb/Dwc3.c`：`transfer()/trb()/pump()/handle_event()/device_event()/dwc3_stop()`，新增 `IN_*` 宏、`in_buffer/in_trb/in_reset/in_transfer/in_complete`、`dwc3_set_in_ring()`。
- `MODULE/usb/Dwc3.h`：新增 `dwc3_set_in_ring()` 声明。
- `MODULE/kernel/QcomXnuUsbDebug.c`：`poll_owned()` 预算、`qcom_usb_debug_init()` 的 boot-arg 解析与 ring 使能点。
- `MODULE/patches/darwin22/0002-qcom-usb-debug-worker.patch`：worker 5 ms tick（S3 目标，未改）。
- `MODULE/kernel/QcomUsbBuffer.h`：`SOC_USB_LOG_DRAIN_BUDGET=4096`（S4 目标，未改）。
