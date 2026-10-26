# 架构与关键寄存器事实

## 平台事实（实测）

| 项 | 值 |
|---|---|
| SoC | SM8150-AC（Xiaomi Pad 5，nabu） |
| GCC 窗口 | `0x00100000`，长度 `0x100000`（USB 需要 `0x8C008/0x8C028` 两路 clkref） |
| DWC3 | `0x0A600000` |
| apps SMMU | `0x15000000`，DT `compatible = "qcom,qsmmu-v500"` |
| USB stream ID | `0x140` |
| QUSB2 PHY | `0x088E2000`（femto）；PHY reset BCR `0x12000`、`0x6A000` |
| GDSC | `0xF004/0xF008`，legacy power-on 用 `GDSCR.bit31` |
| 链路速率 | Full Speed（`DSTS.CONNECTSPD=1`）；`DCFG.DEVSPD=0` |

## prepare：四级

`qcom_usb_prepare=1` 时按顺序执行，失败即带 stage 返回：

1. **CLOCKS**：5 路 CBCR `0xF010/14/18/50/54` + `pipe 0xF058`（不轮询）
   + `0xF078/0xF07C` + `clkref 0x8C008/0x8C028`（不轮询）；轮询 `CLK_OFF`。
2. **ID**：GDSC 上电（清 `SW_COLLAPSE`，轮询 `PWR_ON bit31`）、`GSNPSID` 校验。
3. **PHY**：femto 初始化（`0x40 XCVRSEL`、DP pulse），两路 PHY reset 1→0。
4. **CORE**：显式清 `GCTL.CORESOFTRESET`（bit11）→ 写 `GCTL=DEVICE|DSBLCLKGTNG`
   → 10×10 ms 沉降 → 读 `GSTS.CURMOD==0` 验收 → `GUSB3PIPECTL.SUSPHY=1`
   → `DCFG.DEVSPD=0` → 撤销 `DCTL.CSFTRST` → `RUN_STOP=1`（只要求 `DSTS.DEVCTRLHLT==0`）。

成功读数：`prep=0 stage=4 rb=00920001`（`rb` 是最后一次成功访问的值，不是失败点）。

## SMMU：为什么必须是 bypass-bank

启动时固件已经为 USB SID 建好条目：

- `SMR = 0x80000140`（valid + SID 0x140）
- `S2CR = 0x3`（CBNDX=3，`TYPE[17:16]=0` translate）

因此私有翻译域装不进去（`QusbDomainStreamExists`）。把该条目的 TYPE 改成 BYPASS
（写 `0x10003`）会**读回 `0x20003`**：bit16 被清、bit17 被置，即 TYPE 被重编码成 FAULT。
这不是写失败，是固件不接受 TYPE=BYPASS。

同板证据：

- `ref-sheng` 移植 `package/SmmuBypass.h:4-6`：
  “a TYPE=BYPASS S2CR reads back as FAULT on this firmware”，正确做法是映射到
  固件预留的 bypass bank（`SCTLR=0`、`CBAR.TYPE=S1_TRANS_S2_BYPASS`、`CBA2R.VA64=1`），
  S2CR 用 **TYPE=TRANSLATE**，且先写 S2CR 再写 SMR。
- 本板 UFS `drivers/NativeUfs/SmmuTakeover.h:106-131`：`write(0xc00+4*slot, p.bank)`，
  并注明 “SM8150 does not return the written value”，所以回读校验全部停用。
- 老仓库 `native_smmu_bypass`：从 `banks-2` 向下找未被其他有效 stream 占用的 bank，
  配置同上；该路径让事件环立刻可读。

### 本仓库的实现（`soc/QcomUsbBypass.c`）

`QcomUsbBypassPlan`（只读）：

1. 校验 `IDR0/1/2` 几何与三个地址宽度；
2. 找本 SID 的唯一私有 SMR（重复/共享/缺失都拒绝）；
3. 统计其他有效 stream 占用的 bank；
4. 优先用当前条目已指向且 `SCTLR.M=0` 的 bank，否则从顶部向下选第一个 `SCTLR.M=0`
   且未被占用的 bank；保存该 bank 的 `SCTLR/CBAR/CBA2R`；
5. 若它已经是 bypass 态且 S2CR 已指向它 → **零写入**（`changed=0, active=0`）。

`QcomUsbBypassEnter`（最小写集）：`SCTLR=0 → CBA2R=1 → CBAR=0x10000 → S2CR=bank`。
`QcomUsbBypassRestore`：先 `S2CR=原值`，再回写改过的 CB 寄存器。
读回只作诊断（`lr`），不是判据；验收看真实 DMA/枚举。

`physical_write` 的授权按 lease 放行：domain、identity、bypass 三套互斥，
bypass 只允许计划里的“地址/值”对。

## CDC 通道

- `PE_init_kprintf` 在 `qcom_usb_debug_init()` 返回后把 `PE_kputc` 换成
  `qcom_usb_debug_putc`；`init=1` 表示核心已在 device 模式运行。
- 设备描述符：VID `0x1209` / PID `0x316D` / serial `QCOMXNUUSBDEBUG`；
  FS 下 EP0 64、通知端点 16、bulk 64。
- IN 方向有 DTR 门：`pump()` 只在 `dtr=1` 时发送，主机必须发
  `SET_CONTROL_LINE_STATE`（置 DTR）。裸 `ReadFile` 前先 `EscapeCommFunction(SETDTR)`。
- 写有 backlog：主机打开端口后先补发更早的日志（最早可见从 `zones` 开始）。

## 状态字段（`qcom_usb_debug_status`，内核打印用）

bypass 路径下的映射（结构体未加字段）：

| 打印 | 字段 | 含义 |
|---|---|---|
| `dr` | `domain_result` | plan/enter 结果（0 成功；6/9/10/11/24/17/2 为失败码） |
| `ds` | `domain_stage` | 0 零写入 / 1 commit / 2 restore |
| `bank` | `domain_bank` | 选中的 context bank |
| `asid` | `domain_asid` | 原 `S2CR` 所在 CR0（bit0=1 表示 CLIENTPD 全局 bypass） |
| `la` | `domain_last_address` | 原 SMR 值 |
| `le` | `domain_last_expected` | 原 S2CR |
| `lr` | `domain_last_readback` | 写完 S2CR 的读回（仅诊断） |
| `lt/tr/ta/tb` | `last_trace/traces/trace_a/trace_b` | 最后一次 CDC 事件、事件数、参数 |

`lt` 枚举：1 ID / 2 TIMEOUT / 3 COMMAND / 4 DEVICE_EVENT / 5 SETUP /
6 UNSUPPORTED（被 STALL）/ 7 BAD_EVENT / 8 READY（SET_CONTROL_LINE_STATE，`tb`=DTR）。
