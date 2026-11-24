# 【§0】值班手册 —— 压缩后照着做，不要重新摸索

> 本节放在最前，因为压缩后的新会话只需要这一节就能继续干活。
> 后续 §1–§34 是历史与证据，查细节时才读。
>
> 最后更新：2026-11-25 02:40。当前内核 `kdpr6`（`bbd2a5c882076187`，含 0022 补丁）。

---

## 0.1 当前状态（一句话）

**显示通了、触屏可用、CDC 控制台+KDP 全通、lldb 能连。**
帧率未解决（交互态 `<20ms` 占 **0%**，`≥200ms` 占 **79%**，max 454 ms）。
GPU 每作业只跑 4.18 ms，**181 ms 在 CPU 侧**。
KDP resume 正在定位（0022 待验证 `resume=1`）。

| 项 | 值 |
|---|---|
| 在跑内核 | **`kdpr6`**，`bbd2a5c882076187`，编译 2026-11-25 02:35 |
| 包 | `/e/nabu/hold/kdpr6/` |
| boot 镜像 | `boot-merge-r3.img`，sha `ead4b53135d67516`（**红线，永不重建**）|
| boot-args | n=51，含 `debug=0x14e` `nabu-kdp-net=1` `nabu-cdc-rx=1` |
| 簇链 | `<90-61511>`，61,422 簇，单段连续 |
| 控制台 | `tools/nabu_mux.py`（**单进程同时给 SXNU1 + KDP**）|

## 0.2 禁止事项（违反必出事，都是事故换来的）

| # | 禁止 | 后果 |
|---|---|---|
| **J1** | **禁止对 `/` 或整树全量搜索**（`find /`、`grep -r /`、`grep -r 大目录`）| 卡死几分钟，用户明确禁止过多次。**已知路径定向查** |
| **J2** | **禁止 `grep` 125 MB 的 `kernel.macho`** | 卡死。用 `llvm-nm` / `strings` 定向查 |
| **J3** | **禁止 `sleep 30` / `sleep 48` / 死等** | 一律 **10 秒轮询**，每轮打印状态 |
| **J4** | **禁止在 `kill` 控制台进程后不检查 COM3 就继续** | 端口被占时下一步全失败 |
| **J5** | **禁止刷 `boot_a`** | 会清 misc 的 boot-recovery 标记，循环回同一内核 |
| **J6** | **禁止用 `mcopy` 换 kernel** | 会重排簇链 → 卡死。**4 轮实证** |
| **J7** | **禁止用 `unixnu payload` 全量重生** | 丢 trustcache / scratch-data / gpuaddon |
| **J8** | **禁止漏打 NOP18** | 必卡 `disk1s1` 挂载，只出 6 行 `nabu:`。**6 轮实证** |
| **J9** | **禁止 `wsl bash -lc`** / **`wsl bash -c "... $var ..."`** | 引号被吃，命令错乱 |
| **J10** | **禁止 `debug=` 带 `0x200`**（`DB_KDP_GETC_ENA`）| 会让 `kdp_getc()` 逐字节吃掉 KDPF 帧 |
| **J11** | **禁止在设备处于 KDP 时重启/杀掉承载进程** | `kdp_connection_wait` 无超时退出，EP_OUT 无人 arm，**只能断电** |
| **J12** | **禁止把 `FRAMESTAT` / `GPUSTATE` / `SUBMIT` / `IBCLASS` 当设备 verb 发** | 它们是主机侧伪命令，设备不认 |

## 0.3 上板流程（严格按此顺序，一步不省）

### 0.3.1 刷前检查（三条，缺一不可）

```bash
adb devices                       # 必须 f91fa664 recovery
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames() -join ','"
                                  # 必须只有 COM1（COM3 在 = 内核还在跑，先复位）
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "(Get-Process python -ErrorAction SilentlyContinue|Measure-Object).Count"
                                  # 必须是 0（有残留 = 端口被占）
```

### 0.3.2 ★ 先起 mux，确认它在等 COM3（**顺序不可颠倒**）

```bash
cd /d/_nabu-usb-cdc && rm -f /e/nabu/logs/<tag>-cmd.txt
nohup python -u tools/nabu_mux.py --tag <tag> --out E:/nabu/logs \
  --wait 400 --kernel /e/nabu/hold/<包>/kernel.macho \
  > E:/nabu/logs/<tag>-stdout.log 2>&1 &

sleep 8; tail -4 /e/nabu/logs/<tag>-stdout.log
# 必须看到：
#   >>> symbols loaded from ...
#   >>> waiting for COM3 (0s, 1 attempts)
#   >>> waiting for COM3 (2s, 11 attempts)
# **不是** could not open port（那说明代码退化了）
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "(Get-Process python -ErrorAction SilentlyContinue|Measure-Object).Count"
# 必须是 1
```

**理由**：一个 boot 只有一次 CDC open，开机最初几十秒是判断「内核有没有走到桌面」的唯一依据。控制台起晚了整段丢失。

### 0.3.3 刷机（四条，顺序固定）

```bash
cd /e/nabu
adb shell 'dd of=/dev/block/by-name/misc bs=4096 count=1' < /e/nabu/dwc3/build/misc-bootrecovery.bin 2>&1|tail -1
adb reboot bootloader
for i in $(seq 1 18); do f=$(fastboot devices 2>/dev/null|awk 'NF>=2{print $1}'|head -1); [ -n "$f" ] && { echo "fb=$f"; break; }; sleep 5; done

fastboot flash cust /e/nabu/hold/<包>/payload.img 2>&1|tail -1
fastboot flash misc /e/nabu/dwc3/build/misc-bootrecovery.bin 2>&1|tail -1
fastboot boot /e/nabu/hold/<包>/boot-merge-r3.img 2>&1|tail -1
```

**顺序必须**：刷 cust → **re-arm misc** → boot。

### 0.3.4 等 CDC（10 秒轮询）

```bash
sleep 10; tail -3 /e/nabu/logs/<tag>-stdout.log
# 基线 38–60 秒出现 CDC OPEN OK；慢的时候到 160 秒
```

### 0.3.5 第一件事：发 hello

```bash
cd /e/nabu/logs && printf 'hello\n' >> <tag>-cmd.txt
sleep 8; grep -a "HELLO OK" <tag>-stdout.log|tail -1
# 期望：RX SXNU1R <session> 00000000 HELLO OK session=... last_seq=... control=1 ...
```

**`CDC OPEN OK` 不能证明可用** —— 必须看到 `HELLO OK`。

### 0.3.6 数据到手立刻复位（纪律）

**一个 boot 只有一次 CDC open。** 采到本轮要的数据后马上复位，不要挂着。

```bash
cd /e/nabu/logs && printf 'reboot\n' >> <tag>-cmd.txt
sleep 12; grep -a "REBOOT" <tag>-stdout.log|tail -2; adb devices 2>&1|tail -2
# 期望 f91fa664 recovery
```

**复位成功的判据**：**COM3 消失 且 adb 出现 recovery**。`adb devices` 空 ≠ 离线。

## 0.4 构建内核（纪律 + 命令）

### 0.4.1 一条命令，必须后台

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash /mnt/e/nabu/tmp/_build_inner.sh <tag>
# 必须 run_in_background: true
```

**耗时 3–5 分钟。** 10 秒轮询：

```bash
sleep 10; MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c \
 'S=$(grep -o "sha256 [0-9a-f]\{16\}" /tmp/build_<tag>.log 2>/dev/null|tail -1);
  R=$(pgrep -f "unixnu build kernel" >/dev/null && echo YES || echo NO);
  E=$(grep -cE " error:" /tmp/build_<tag>.log 2>/dev/null);
  G=$(grep -c "git failed" /tmp/build_<tag>.log 2>/dev/null);
  echo "sha=$S run=$R errs=$E gitfail=$G"'
```

**成功标志**：出现 `sha256 <64hex>`，`errs=0`，**`gitfail=0`**。

### 0.4.2 构建树的三条校验（脚本已做，但要知道）

```bash
ls -ld /root/merge/out-v6/nabu /root/merge/out-v6/feeds   # 必须都是 symlink
mount | grep -c out-v6                                     # 必须 0（绝不能是 bind mount）
/opt/unixnu-linux/llvm/bin/clang-24 --version              # 必须能跑
```

**历史**：`out-v6` 曾是 bind mount，累积 91 层，底层是 11 月 5 日旧快照。

### 0.4.3 改 USB 模块源码后，必须重建 `.a`

**内核链接的是固定路径的 `.a`，不是模块源码！**

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c '
rm -rf /root/merge/l738-device/tmp/modbuild-<tag>
cd /mnt/d/_nabu-usb-cdc/module
REC=/root/merge/l738-device/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_kprintf.o.json
python3 -I scripts/build.py --compile-record "$REC" --soc soc/Sm8150.h \
    --output /root/merge/l738-device/tmp/modbuild-<tag>
cp -f /root/merge/l738-device/tmp/modbuild-<tag>/libQcomXnuUsbDebug.a /root/merge/l738-device/build/
'
```

**⚠️ `REC` 用 `l738-device/build/...` 那份** —— `out-v6/.../tree/build/...` 那份指向
`.cache/xnu-work`，那个目录会随构建重建而消失。**实测踩过**（`ValueError: reference
directory/source unavailable`）。

### 0.4.4 三棵树的真源（改文件前必须确认）

| 东西 | 真源 | 陷阱 |
|---|---|---|
| **GPU 驱动** | **`/root/merge/UniXNU-nabu/drivers/NabuAdreno`** | `tree/drivers/NabuAdreno` 是 symlink 指向它；`l738-device/drivers/NabuAdreno` 是**过期副本** |
| **XNU 补丁** | **`/root/merge/l738-device/patches/xnu/`** | `series` 决定顺序 |
| **干净 XNU 基线** | **`/root/merge/l738-device/refer/xnu`**（`git`, `d8b8029`）| **生成补丁只能从这里** |
| USB 模块源码 | `/mnt/d/_nabu-usb-cdc/module` | 改完必须重建 `.a`（见 0.4.3）|
| 控制台/mux | `/mnt/d/_nabu-usb-cdc/tools/` | — |

**绝对不能删 `.o`** —— XNU 编译前 corecrypto 用 `nm` 扫所有 `.o`，缺一个整轮失败。
强制重编用 `touch <源文件>`。

## 0.5 打补丁（纪律 + 完整流程）

### 0.5.1 三条铁律

| # | 规则 | 原因 |
|---|---|---|
| **P1** | **只能从 `refer/xnu` 生成补丁** | `.cache/xnu-work` 已应用其它补丁，`git diff` 会混进别人的改动（**实测生成出 289 行污染 diff**）|
| **P2** | **改 `.patch` 文件后必须重算 hunk 头** | 否则 `git apply` 报 `corrupt patch at line N` |
| **P3** | **改 `0006` 会破坏 `0018`** —— worker 周期实际由 **`0018`** 的 `freq/1000` 决定，`0006` 里的是死代码 | 实测踩过 |

### 0.5.2 生成补丁的标准流程

```bash
# 1) 确认 refer 干净
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c \
 'cd /root/merge/l738-device/refer/xnu && git status --short'

# 2) 用 Write 工具写补丁脚本到 /e/nabu/tmp/，再执行
#    （不要用 bash heredoc —— 反引号/$ 会被吃，实测多次失败）
MSYS_NO_PATHCONV=1 wsl.exe --exec python3 /mnt/e/nabu/tmp/patch_xxx.py

# 3) 从干净树生成 diff
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c \
 'cd /root/merge/l738-device/refer/xnu
  git diff -- <file1> <file2> > /root/merge/l738-device/patches/xnu/00NN-name.patch'

# 4) 恢复 refer
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c \
 'cd /root/merge/l738-device/refer/xnu && git checkout -- <files>'

# 5) 加入 series（位置在某个 anchor 之后）
#    用 python 插入，不要 sed

# 6) 清 work 树，重编验证
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c \
 'rm -rf /root/merge/l738-device/.cache/xnu-work /root/merge/UniXNU-nabu/.cache/xnu-work'
# 看 /tmp/build_<tag>.log 里 gitfail 是否为 0
```

### 0.5.3 多补丁改同一段代码时，合并成一个

**实测教训**：0019/0020/0021 都改 `debugger_collect_diagnostics` 附近，
分开维护时每加一个都要重算 hunk 头，且从干净树生成的 diff 本来就含全部改动。
**已合并成 0022**。备份在 `/tmp/series.bak.0022`。

### 0.5.4 补丁编号现状

```
series 当前有效顺序（省略中间）：
  ... 0018-nabu-reboot-worker-and-wake.patch
      0022-nabu-kdp-resume-flag-consume.patch   ← 含 0019+0020 全部 + 本次修正
      zzz-boot-progress.patch
      ...
```

**0019/0020/0021 已合并进 0022，不要再单独应用。**

## 0.6 使用新控制台（`nabu_mux.py`）

### 0.6.1 它是什么

**一个进程占 COM3，同时提供两条通道：**

| 通道 | 走法 |
|---|---|
| SXNU1 控制台 | `printf '<verb> [args]\n' >> <tag>-cmd.txt` |
| **KDP** | **UDP 41139**（`kdp_cli.py` / lldb 直接连，**不用重启进程**）|
| 内核日志 | 实时打印 + 落 `<tag>-ctl.txt` |
| 原始字节 | 落 `<tag>-com.bin`（**追加模式，重启不丢**）|

**分流依据**：`"KDPF"` magic 走 KDP，其余字节当文本扫 `SXNU1R`。

### 0.6.2 启动

```bash
cd /d/_nabu-usb-cdc
nohup python -u tools/nabu_mux.py --tag <tag> --out E:/nabu/logs \
  --wait 400 --kernel /e/nabu/hold/<包>/kernel.macho \
  > E:/nabu/logs/<tag>-stdout.log 2>&1 &
```

**`--kernel` 是伪命令必需的**（要解析符号地址）。不带就只能转发 verb。

### 0.6.3 发命令：只追加，绝不截断

```bash
cd /e/nabu/logs && printf '<命令>\n' >> <tag>-cmd.txt
```

**⚠️ 绝不能 `: > <tag>-cmd.txt` 或 `>` 覆盖。** 控制台按字节偏移追踪，
截断后新命令被静默跳过。**要重来就换 tag。**

**一次写多行是可以的**（已修 `cmd_pos` 预推进的 bug），但**推荐逐条发**，
这样每条的输出和命令一一对应，便于读日志。

### 0.6.4 设备端 verb（会转给设备）

```
HELLO PING STATUS REBOOT READ READNEXT WRITE SYM SYMS READPA READPANEXT
WRITEPA BOOTARGS DEBUGGER KDPPUSH KPDPOP SHOTNEXT DUMPUFS TSENS CPSTAT ...
```

**`kdp` 是主机侧简写**，等价于发 `DEBUGGER`（空 payload）。

### 0.6.5 ★ 主机侧伪命令（mux 自己解析，需要 `--kernel`）

| 命令 | 作用 |
|---|---|
| **`framestat`** | 帧时间直方图（26 words @ `_native_adreno_frame_hist`）|
| **`gpubusy`** | `native_adreno_gpu_busy[8]`：中断/轮询/advance 计数 |
| **`a6xxstats`** | `native_adreno_a6xx_stats[24]`：IRQ status / zap phase |

**⚠️ 这些不是设备 verb。** 直接发给设备会被拒。

### 0.6.6 ★ SXNU1 的 seq 规则（踩过三次）

`QcomUsbControl.h:516`：`frame->seq != state->last_seq + 1u` → 静默拒绝。

**HELLO 是唯一例外**（要求 `session==0 && seq==0`，且不改 state）。

**正确流程**（mux 已实现，手工发命令时要知道）：
1. `HELLO` → 从回复取 `session=` 与 `last_seq=`
2. **记住 `last_seq`，下一条命令用 `last_seq + 1`**
3. 之后每条 `+1`

**mux 已内置**：`_resync_if_stuck()` 超时 6 秒自动重发 HELLO 重新同步。

### 0.6.7 ★ 进 KDP 后 SXNU1 必然沉默

`QcomXnuUsbDebug.c:3701`：`kdp_rx_armed` 一旦置 1 **永不清除**，`control_service`
不再被调用。

| 通道 | KDP 会话期间 |
|---|---|
| SXNU1 | **永久沉默**（`hello` 也不回）|
| 内核日志 | 继续输出 |
| KDP | **唯一可用通道** |

**⇒ `hello` 不回时先看 `-ctl.txt` 里有没有 `TX DEBUGGER`。有 = 正常。**

### 0.6.8 ★ 设备在 KDP 时不要重启 mux

**会永久踢掉会话，只能断电。** 要换工具必须让会话正常结束（resume / reboot）。

## 0.7 KDP 使用（lldb 与 kdp_cli）

### 0.7.1 lldb 连（WSL）

```bash
# 取 Windows 宿主 IP —— 不能用 ip route，也不能用 resolv.conf 的 nameserver
GW_HEX=$(awk '$2=="00000000"{print $3; exit}' /proc/net/route)
HOSTIP=$(printf "%d.%d.%d.%d" 0x${GW_HEX:6:2} 0x${GW_HEX:4:2} 0x${GW_HEX:2:2} 0x${GW_HEX:0:2})

cd ~/nabu-lldb/build/bin
./lldb -o "target create /mnt/e/nabu/hold/<包>/kernel.macho" \
       -o "process connect --plugin kdp-remote udp://${HOSTIP}:41139"
```

**实测可用**：`memory read`（虚拟地址）、`register read`、`bt`、
`image lookup -a/-t`、`expr -- x = v`（**写内存可用**）。

**实测不可用**：
- `process continue` **不能恢复内核**（见 0.7.4）
- `memory read` 读 `0xfffffff8...` IO 映射区 → `kdp read memory failed`
- `expr` 里调函数（`getVirtualAddress()`）→ `Interpreter doesn't handle opcode`

### 0.7.2 `kdp_cli`（UDP，需 mux 或网桥）

```bash
cd /d/_nabu-usb-cdc/kdp_client
python kdp_cli.py --transport udp --host 127.0.0.1 --udp-port 41139 --fresh connect
python kdp_cli.py --transport udp --host 127.0.0.1 --udp-port 41139 readmem <addr> <len>
python kdp_cli.py --transport udp --host 127.0.0.1 --udp-port 41139 reboot
```

**⚠️ `timeout` 命令会吞输出**（Git Bash + Windows python）—— 用
`(cmd > out.txt 2>&1 &) ; sleep N; cat out.txt` 代替。

### 0.7.3 独立网桥（mux 不适用时）

```bash
cd /d/_nabu-usb-cdc/kdp_net_bridge
nohup python -u kdp_net_bridge.py --port COM3 --cdc-framing kdpf \
  --udp-mode kdp --poll 0.01 > /e/nabu/logs/bridge.log 2>&1 &
```

**三个参数一个都不能错**：
- `--cdc-framing kdpf`（不是 `serial`）
- `--udp-mode kdp`（lldb 发裸 KDP 包）
- `--poll 0.01`（默认 0.1 吃掉 90 ms/往返）

### 0.7.4 KDP resume 的已知状态（**未解决**）

**现象**：`Continuing (host resume).` 打印后同一秒 USB 掉线 → 重启。

**已排除**：`kdp_en_setmode`（`QcomKdpNet.c:353` 清成 NULL）、`kdp_trap` 的 PC 调整、
`usb_busy` 死锁、`debugger_safe_to_return`（0019 已改成显式赋值）、
`debugger_is_panic`（0020 已改）、**`pending` 提前消费**（0022 已修，待验证）。

**诊断 printf 已内置**（0022 里）：
```
nabu-kdp: test op=%d safe=%d count=%u is_panic=%d resume=%d
```
**判据**：`resume=1` 就该返回内核；若 `resume=1` 仍重启，问题在 `return` 之后的
`panicDebugging` 段（`debug.c:1543`）。

## 0.8 读 GPU 寄存器（帧率排查）

```bash
readpa 0x02c00000 4      # chip_id     -> 0x00010053
readpa 0x02c00840 4      # RBBM_STATUS -> bit0 = CP idle
readpa 0x02c000e0 4      # RBBM_INT_0_MASK
readpa 0x02c00804 4      # RBBM_INT_0_STATUS
```

**白名单**：`kdpr4` 起的版本已含 `{0x02c00000, 0x02c40000, "gpu"}`（只读）。

**注意窗口有洞**：`0x02c00100` / `0x02c000dc` 读回 `0xdeafbead`（未映射）。
**连读三次，值稳定的是真寄存器。**

**寄存器偏移来自 `a6xx.xml.h`（与 Linux msm 一致）：**
`RBBM_STATUS=0x210*4=0x840`、`INT_0_STATUS=0x201*4=0x804`、
`INT_0_MASK=0x38*4=0xE0`、`CP_CACHE_FLUSH_TS=bit20=0x100000`。

**⚠️ `WRITEPA` 白名单不含 GPU 核**（只有 `0x02c90000` GPUCC）。

## 0.9 常用观测命令速查

| 目的 | 命令 |
|---|---|
| 帧时间直方图 | `printf 'framestat\n' >> <tag>-cmd.txt` |
| GPU 计数 | `printf 'gpubusy\n' >> <tag>-cmd.txt` |
| a6xx 状态 | `printf 'a6xxstats\n' >> <tag>-cmd.txt` |
| 读内存 | `printf 'read 0x<va> <len>\n' >> <tag>-cmd.txt` |
| 读物理/MMIO | `printf 'readpa 0x<pa> 4\n' >> <tag>-cmd.txt` |
| 复位 | `printf 'reboot\n' >> <tag>-cmd.txt` |
| 进调试器 | `printf 'kdp\n' >> <tag>-cmd.txt` |
| 符号地址 | `llvm-nm <kernel> \| grep <name>`（**不要 grep macho 全文**）|

**帧率的正确读法**（关键）：
1. 取基线 `framestat` → 记 `n`
2. **让用户划屏 30 秒**
3. 再取 `framestat` → `n` 必须涨
4. **只有 `n` 涨了，直方图才有意义**；静止时不提交，看到的全是启动期样本

## 0.10 失败信号对照表（省一轮板的事）

| 现象 | 真因 | 修 |
|---|---|---|
| `could not open port 'COM3'` 且进程退出 | 控制台没等端口 | 用 `--wait`，日志文件先开会 |
| `read cmd.txt` 有新命令但无任何输出 | `cmd_pos` 预推进吃掉了 | 已修（逐行推进）|
| 命令发出设备不回，`hello` 也不回 | 进了 DEBUGGER（`kdp_rx_armed`）| 正常，走 KDP |
| `seq` 一直错位 | 失败命令消耗了 seq 但设备没 commit | `_resync_if_stuck()` 自动重发 HELLO |
| `FRAMESTAT` 发出去零回应 | 它是伪命令不是 verb | 用 mux 的 `framestat` |
| `kdp read memory failed` 读 `0xfffffff8...` | IO 映射区无 pmap 后端 | 用 `READPA` |
| `git failed (1)` 在 build log | 补丁行号/上下文不匹配 | 从 `refer/xnu` 重新生成 |
| `corrupt patch at line N` | hunk 头行数不对 | 重算 `@@ -a,b +c,d @@` |
| `reference directory/source unavailable` | compile record 指向 `.cache/xnu-work` | 用 `l738-device/build/...` 那份 |
| 设备无 COM3 无 adb | 卡在 KDP 且会话已断 | **只能断电** |
| `adb devices` 空但 COM3 在 | 内核在跑但 adb 未暴露 | 正常（我们的 CDC 占 USB）|

## 0.11 本轮（0022）待验证的判据

**`kdpr6` 已打包（`bbd2a5c882076187`），未上板。**

上板后：
1. 进 KDP：`printf 'kdp\n' >> <tag>-cmd.txt`
2. lldb connect + `process continue`
3. **看日志里的 `nabu-kdp: test`**：
   - `resume=1` 且内核继续跑 → **成功**
   - `resume=1` 但重启 → 查 `return` 之后的 `panicDebugging` 段
   - `resume=0` → 标志又被别处清了
   - 没有这行 → `kdp_raise_exception` 没返回

---

