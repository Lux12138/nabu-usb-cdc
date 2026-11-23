# FAILED-ROUND kb71 —— 第二次 open 同一 boot：IN 通、OUT 不通

> 2026-11-23 19:00–19:01。**未获得任何控制帧回复，本轮作废。**
> 目的记录：这是单句柄 CDC 铁律的**第三次实证**，且是"看起来成功"的一次。

---

## 1. 判据（全部为实测）

| 判据 | 结果 | 证据 |
|---|---|---|
| CDC 设备出现 | ✅ | `CDC FOUND after 0.0s (attempt 1): COM3` |
| `open()` 返回成功 | ✅ **但无意义** | `CDC OPEN OK on COM3 (try 1)` |
| **设备 → 主机（IN）** | ✅ **通** | `kb71-com.bin` 持续增长：4,384,279 → 4,949,736 B（19:01）|
| **主机 → 设备（OUT）** | ❌ **不通** | `SXNU1R count: 0`，HELLO 无回复 |
| 设备本身健康 | ✅ | 日志持续输出 `nabu: spawn`、`nabu-cpufreq`、`NativeAdreno*` |
| 设备 uptime | **2138 s（35.6 min）** | 与上一轮 18:40 关端口是**同一个 boot** |

## 2. 复现条件（关键）

```
18:24:44  boot
18:25:22  第一次 open → 正常，13 条命令全部有回复
18:40:22  close（--deadline 900 到期，正常退出）
19:00:00  第二次 open 同一 boot → open 报成功，IN 通，OUT 死
```

**⇒ "open 成功" 这个返回值不能用来判断 CDC 可用。**
**唯一可靠的活性判据是「发 HELLO 能收到 SXNU1R」。**

## 3. 为什么不是代码问题

| 假设 | 排除依据 |
|---|---|
| 控制 hook 没装上 | `control_hook_owned` 是 boot 期一次装好、不随 open/close 变（`QcomXnuUsbDebug.c:1446-1448`）|
| 控制接收依赖 `kdp_rx_armed` | 不依赖。`kdp_rx_armed` 只影响 `dwc3_poll` 的 budget（64 vs 1，`:1090`）|
| 命令文件没读到 | `kb71-cmd.txt` = 25 B，`hello` + `sysreg CNTKCTL_EL1` 都在，控制台日志确认 `TX HELLO (seq=0)` |
| HELLO 被 gate 拦 | HELLO 无 gate，且上一轮同一内核正常回复 |

**⇒ 是 USB 层 OUT 方向在 close 之后失效，与模块逻辑无关。**

## 4. 纪律更新（本条要进 MEMORY）

**单句柄 CDC 铁律的准确表述应该是：**

> **一个 boot 周期只能 open 一次 CDC。`close()` 之后再 `open()`，即使返回成功、
> 即使 IN 方向还能收到日志，OUT 方向也已失效。**
> **活性判据只能是「HELLO 有 SXNU1R 回复」，不能是「open 成功」或「日志在流」。**

**恢复手段：只有板子复位。**（本次未尝试任何软件复位——那是另一条已被证伪的路。）

## 5. 本轮资源

| 项 | 值 |
|---|---|
| 日志 | `E:/nabu/logs/kb71-com.bin`（4.9 MB，**只有 IN 方向**）|
| 控制台 stdout | `E:/nabu/logs/kb71-stdout.log` |
| 命令文件 | `E:/nabu/logs/kb71-cmd.txt`（25 B）|
| 内核 | kb-70（`3480d5c12b7edb2a6cbb`），**未改动** |
| 设备状态 | 健康运行中，未 panic，未卡死 |
