# T64 需求单：arm64 KDP resume 后内核立即重启 —— 需要定位

- 日期：2026-11-25
- 提出方：主线（板侧实测）
- 优先级：中（不阻塞当前帧率/吞吐排查；resume 是「动态调试」的最后一块）
- 现状：**第一层已修好并可观测，第二层疑点在 `DebuggerXCallReturn` 之后的核间恢复**

---

## 1. 已完成的部分（不需要再研究）

补丁 `patches/xnu/0019-nabu-kdp-resume.patch`（148 行，3 文件）已编译上板，
**实测确认前两层生效**：

```
板侧串口输出（nabu_mux.py 捕获）：
  01:26:0x  Connected to remote debugger.
  01:26:0x  Continuing (host resume).        ← 补丁里的 printf，说明走到了 exit
  01:26:06  ! serial read failed: ClearCommError ... PermissionError(13)
  01:26:06  ! waiting for the device to come back
  （同一秒内 USB 掉线，之后 adb 显示 f91fa664 recovery）
```

**⇒ `kdp_raise_exception` 的循环确实退出了，`kdp_udp.c` 的改动有效。**

`debug.c` 的改动也按复核意见修正为：

```c
/* 阈值 > 1，不是 != 0 —— 最外层进来时 count 已经被 DebuggerWithContext 的
 * CPUDEBUGGERCOUNT++ 变成 1，而 :1597 的赋值早于 :1678 的调用，
 * 所以这里读到的是新值。!= 0 会恒真，改动等于没做。 */
if (debugger_current_op == DBOP_PANIC ||
    (debugger_current_op == DBOP_DEBUGGER &&
     kernel_debugger_entry_count > 1)) {
    debugger_safe_to_return = FALSE;
}

/* 另：这个全局从来只被置 FALSE、从不复位（全树唯一 TRUE 在定义处 :320），
 * 所以每次进入显式决定，而不是只在需要时清。 */
debugger_safe_to_return = (kernel_debugger_entry_count <= 1);
```

---

## 2. 待研究的问题

**`Continuing (host resume).` 打印后，USB 在 1 秒内掉线，设备进 recovery。**
没有 panic 日志落盘（`panicstr` 读不到内容），也没有任何告别输出 ——
**像是被直接切断，不是 panic 走的正常路径。**

### 2.1 已排除

| 候选 | 排除依据 |
|---|---|
| `kdp_en_setmode(FALSE)` 拆链路 | **`QcomKdpNet.c:353` 已把该钩子清成 NULL**（`kdp_register_link((void*)0,(void*)0)`），`exit_debugger_loop` 里 `if (kdp_en_setmode)` 根本不执行 |
| `kdp_trap` 的 PC 调整 | `TRAP_DEBUGGER = 0xe7ffdeff = GDB_TRAP_INSTR2`，`instr` 匹配后 `pc += 4` 正常 |
| `usb_busy` 死锁 | `control_service()` 在 `serial_rx_drain` 的 `release(flags)` **之后**调用，锁已释放 |
| 这是 panic | `panicstr` 读不到内容；重启由 `nabu-panic-restart=2` 驱动也应有 panic 记录 |
| `debugger_safe_to_return` 恒 FALSE | 已按 > 1 修正并每次进入显式赋值 |

### 2.2 重点怀疑方向（请按此顺序）

**① `DebuggerXCallReturn()` 之后的核间恢复**（`debug.c:644-651`）

```c
void DebuggerResumeOtherCores(void) {
#if defined(__arm64__)
	DebuggerXCallReturn();      /* ← 唤醒被 DebuggerHaltOtherCores 停掉的核 */
#else
	mp_kdp_exit();
#endif
}
```

**这是 arm64 独有的路径，也是 x86 没有的那一段。** 需要回答：
- `DebuggerXCallReturn` 依赖的 per-cpu 状态（`DebuggerState`）在
  「从 SXNU1 verb 进入、而不是从异常向量进入」时是否完整？
- 我们从 `control_handle_frame` → `Debugger()` 进入，**不是在异常上下文里**，
  其它核是被 `DebuggerHaltOtherCores` 用 IPI 停的。恢复时若有核正好在
  中断处理中（尤其 USB 的 DWC3 IRQ 或 worker），状态是否会不一致？

**② USB worker 线程在 `Debugger()` 期间的状态**

`Debugger()` 是在 USB worker 线程里调的（`qcom_usb_debug_poll` →
`serial_rx_drain` → `control_service` → `control_handle_frame` → `Debugger`）。
- `DebuggerHaltOtherCores` 是否会把这个线程所在的核也停掉？（自己停自己？）
- `DebuggerXCallReturn` 恢复后，DWC3 的 TRB 环状态是否还有效？
- 我们的 `QcomKdpNet.c` 的 `rx()` 在调试器态靠
  `qcom_usb_debug_serial_poll()` 手动服务控制器 —— 返回后这个手动服务停止，
  worker 的 4 ms 轮询恢复，**两者交接时是否需要显式重新 arm EP_OUT？**

**③ `magic` 级的可能性：`kdp_machine_reboot` 被误触发**

`kdp_handler` 的 dispatch 里有 `kdp_reboot`（`kdp.c:1018`）。如果 `KDP_RESUMECPUS`
之后残留的某个 `pkt.input` 让 `kdp_packet` 跑到了 `kdp_reboot`，就会
`kdp_machine_reboot()` —— 那正好解释「1 秒内重启、无 panic 记录」。
**判据**：在 `kdp_reboot()` 里加一行 `printf("kdp_reboot: entered\n")`，
看是否会打印。这条最便宜，建议先做。

---

## 3. 需要的产出

| # | 交付 | 形式 |
|---|---|---|
| 1 | 上面 ① / ② / ③ 的判断：哪一条是真因，依据是什么 | 分析 + 源码位置 |
| 2 | 若是 ①，`DebuggerXCallReturn` 在「非异常上下文进入」时缺什么 | 具体代码 |
| 3 | 若是 ②，返回后 USB 需要做哪些重新初始化 | 具体调用序列 |
| 4 | 修正补丁（若需要） | `git apply --check` 通过 |

## 4. 复现步骤（板侧）

```bash
# 1. 起 mux（单进程占 COM3，同时给 SXNU1 和 KDP）
python -u tools/nabu_mux.py --tag live --out E:/nabu/logs

# 2. 进 KDP
printf 'kdp\n' >> E:/nabu/logs/live-cmd.txt     # 后台发 DEBUGGER verb
# 看到串口输出 "Waiting for remote debugger connection." 即已就绪

# 3. lldb 连并 resume
cd ~/nabu-lldb/build/bin
./lldb -o "target create <kdpr3 内核>" \
       -o "process connect --plugin kdp-remote udp://<HOSTIP>:41139" \
       -o "process continue"      # 或 kdp_cli.py resume
```

**现象**：串口出现 `Continuing (host resume).`，同一秒内 COM3 消失，
随后 `adb devices` 出现 `f91fa664 recovery`。

**注意**：`--poll 0.01` 和 `--cdc-framing kdpf` 是必需的，否则另有故障。

## 5. 相关文件

| 文件 | 说明 |
|---|---|
| `patches/xnu/0019-nabu-kdp-resume.patch` | 本次补丁（148 行） |
| `deliverable/T63-lldb-kdp-manual.md` §4.2.1 | 用户手册里的现状描述（待更新为「部分支持」） |
| `tools/nabu_mux.py` | 单进程控制台+KDP，实测工具 |
| `MEMORY-MASTER` §28 | 本补丁的完整设计与复核记录 |
