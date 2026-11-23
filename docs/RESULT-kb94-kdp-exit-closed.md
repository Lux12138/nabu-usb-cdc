# KDP 退出：闭合（2026-11-23 21:25，kb-94 真机验证）

> 设备跑 `hold/kb-92-nop18`（`e3cb7046d0f8eec1c6fa`，已打 NOP18）。
> **`Debugger()` 进得去也出得来。关键在预置字节的数量，而不在事后补发。**

---

## 1. 决定性证据（kb-94 实测）

```
Using serial KDP.
Using serial KDP.
Waiting for remote debugger connection.
Waiting for remote debugger connection.
Options.....    Type
------------    ----
continue....    'c'
reboot......    'r'
Continuing...                                        ← 'c' 生效，kdp_connection_wait 返回

SXNU1R ... DEBUGGER OK state=entering queued=8 pending=8 ovf=0
NativeAdrenoA6xx: automatic hardware diagnostic accepted
NabuGPU: state=starting step=0 status=00000000        ← 内核继续跑
NabuGPU: state=starting step=1 status=00000000
NativeAdrenoA6xx: mapping SM8150 board resources
```

**`Continuing...` 之后 GPU 初始化继续推进** ⇒ `Debugger()` 确实返回了，内核完全恢复。

## 2. 命令

```
kdp enter 6363636363636363
```

等价于发 `DEBUGGER 6363636363636363` 控制帧。

**模块回复里三个关键字段：**

```
DEBUGGER OK state=entering queued=8 pending=8 ovf=0
```

| 字段 | 含义 |
|---|---|
| `queued=8` | 8 个字节成功压入 `kdp_rx` |
| `pending=8` | 进入调试器那一刻环里还有 8 个 |
| `ovf=0` | 零溢出 |

## 3. 为什么必须是 8 个而不是 2 个

`kdp_connection_wait()`（`kdp_udp.c`）有**两处**消费字节：

```c
/* 预检查：消费 1 个，只为决定是否打印菜单 */
if (reattach_wait == 0) {
    if ((kdp_flag & KDP_GETC_ENA) && (0 != kdp_getc())) {
        printf("Options.....    Type\n"); ...

do {
    while (!pkt.input) {
        if (kdp_flag & KDP_GETC_ENA) {
            switch (kdp_getc()) {
            case 'c': printf("Continuing...\n"); return;    /* 再消费 1 个 */
```

**理论上是 2 个。实测 2 个不够**（kb-93 用 `6363` 失败），说明预检查与循环之间还有别的消费者。
**8 个是实测有效的最小可用量，留了余量。**

## 4. 事后补发无效 —— **只对"未 CONNECT"的场景成立**

**进入调试器之后**、**在未 CONNECT 的情况下**再发 `c`（`kdp resume`）**没有任何效果**，连发 12 个也读不到。

**根因链：**

```
kdp_connection_wait() 循环（未 CONNECT 走这里）
  → kdp_getc() → console_try_read_char() → qcom_usb_debug_serial_getc()
    → kdp_rx_used == 0 → serial_rx_drain()
      → acquire()
        → if (cpu_number() != master_cpu) return 0;    ← 在这里返回
          → drain 什么都没做 → kdp_rx 永远是空的 → kdp_getc() 恒返回 0
```

**陷住调试器的线程不在 master CPU 上，`acquire()` 第一行就拒绝。**

**日志里的 `QCOM USB LOSS overflow=0 contention=218 transport=0` 是同一件事的旁证。**

**⇒ 在未 CONNECT 的场景下，所有字节都必须在 `Debugger()` 之前压进 `kdp_rx`
（走 `qcom_usb_debug_kdp_push`，那条路不经过 `acquire`）。**

### 4.1 【更正】已 CONNECT 的场景，`kdp resume` 是可用的

上面说的"事后补发无效"**只适用于未 CONNECT**。两条路必须分开（反编译侧 T38 的界定）：

| 场景 | 退出方式 | 为什么 |
|---|---|---|
| **未 CONNECT**（没有 `Connected to remote debugger.`）| **裸 `'c'`**，且必须在陷入前预置 | 走 `kdp_getc()` 循环，事后字节要经 `serial_rx_drain()`，被 `acquire()` 挡住 |
| **已 CONNECT** | **`kdp resume`**（`KDP_RESUMECPUS` 请求）| 走 KDP 协议请求处理，**不经过 `kdp_getc()`**，因此不受 `acquire()` 限制 |

**⇒ `kdp resume` 不是"没用"，是"只在已 CONNECT 时有用"。**
kb-94 那一轮没有 CONNECT，所以起作用的是预置的裸 `'c'`。

## 5. panic 之后仍然出不来（与 Debugger 是两回事）

| | `Debugger()`（DEBUGGER verb）| panic 之后 |
|---|---|---|
| `kdp_rx_armed` | **1**（`QcomXnuUsbDebug.c:2411`，`Debugger()` 上一行置位）| **0** |
| `serial_getc()` 读哪个环 | `kdp_rx` | `serial_rx`（控制环）|
| 预置字节能用吗 | ✅ | ✅（`kdp_push` 不检查 armed）|
| **能退出吗** | ✅ **能，只要预置 ≥8 个** | ❌ **不能**（panic 路径要先经过 `Debugger` 才能预置，而我们无法在 panic 前预置）|

**⇒ panic 那条路只能断电，除非改 XNU。这一点 T39 早就论证过，没有变化。**

## 6. 控制台行为（已按实测改好）

- `kdp enter 6363636363636363` —— **推荐写法**
- `kdp resume` —— **已改成直接报错并打印正确命令**，不再假装能救（它发多少都没用，见 §4）
- `send_frame_only()` —— 改成等 2 s 回复，这样能看到 `queued=/pending=/ovf=`，
  这三个字段是判断预置是否成功的唯一窗口

## 7. `'r'` 走不通（反编译侧指出的，代码已印证）

```c
static int qcom_usb_control_halt_restart(unsigned int type)
{
    if (type == CONTROL_PE_RESTART_CPU &&            /* == 1 */
        control_reboot_requested && control_reboot_armed)
        platform_smc_reset();
    return -1;                                        /* type 4 落这里，空转 */
}
```

`'r'` → `kdp_machine_reboot()` → `kPEPanicRestartCPU = 4` → 被拒 → `return -1` → 空转。
**所以 `'r'` 目前只能断电。**

若要让它通：halt 钩子接受 `type == 4` 时也走 `platform_smc_reset()`
（只改模块，不动 XNU）。**与 debugger 退出是两件事，没做。**

## 8. 一句话

> **未 CONNECT：进入用 `DEBUGGER`（空参），退出用预置的裸 `'c'`（≥8 个），
> 因为事后字节过不了 `serial_rx_drain()` 的 `acquire()`。**
> **已 CONNECT：`kdp resume`（`KDP_RESUMECPUS`）可用，它不走 `kdp_getc()`。**
> **`'r'` 两种场景都走不通（halt 钩子只认 type 1）。**
