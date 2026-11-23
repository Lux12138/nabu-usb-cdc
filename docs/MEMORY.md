# 最终记忆总档 2026-11-23 18:35（压缩上下文前的完整快照）

> **自包含。** 新会话读完这份就能接手，不需要任何前置上下文。
> 上一份：`MEMORY-MASTER-20261123-1825.md`（680 行，kb-70 前状态；本文是它的更新版）。
>
> **阅读顺序：§1 现状 → §2 纪律 → §3 禁止命令 → §11 操作流程 → §12 坑 → §13 下一步。**

---

## 1. 一句话现状 + 全部关键数字

**XNU 17.7.11 移植到 Xiaomi Pad 5（nabu/SM8150）的 USB CDC 调试控制台，
48 个设备端 verb 全部真机验证；方案 D（panic 日志落 UFS）完全闭合；
T47 偏移全部验证；STACKSHOT 打通；T50 的两个解法（priority 拆分 + KDBG 绕锁）验证通过。**

| 项 | 值 |
|---|---|
| **最新基线** | **`hold/kb-99-full`**，kernel **`f14f0ce6d33315272b29`** |
| **最新 tag** | **`baseline-kb99-fivefeatures`**（`24113a8`）|
| 模块仓库 | `D:/_nabu-usb-cdc` HEAD **`fb1a396`**（已 push）|
| **设备端 verb 数** | **48**（枚举去重，排除 `INVALID`/`LAST`）|
| 主机侧 KDP live 命令 | 4 个（`KDPREAD`/`KDPWRITE`/`KDPREBOOT`/`KDPRAW`），**不计入 verb** |
| 设备 | 跑 **kb-70**（COM3 在、控制台 tag `kb70`）|
| 归档 | `hold/_baseline_archive/20261123/`（**60 文件**，SHA256SUMS 全 OK）|
| 源码备份 | 仓库内 `docs/backup/source/`（36 文件 + SHA256SUMS）|

**boot-args（n=50 / c=887）**：
```
-v serial=3 debug=0x34e nabu-gpu-profile=30000 kdp_match_name=serial
-enable_kprintf_spam native-ufs-root=1 nabu-unverified-root=1 sheng-pmgr=2
nabu-log=1 nabu-panic-restart=2 nabu-timer-intid=19 sheng-log=1
qcom-usb-debug=1 qcom-usb-prepare=1 qcom-usb-ctl=1 qcom-usb-write=1
nabu-cdc-rx=1 armcpu-self-ipi=1 offline-mobile-graphics-load=1
offline-legacy-register=1 offline-legacy-gop=1 nabu-io-throttle-off=1
-apfs_shared_datavolume nabu-ufs-fua=1 nabu-ufs-lock-wait=1
offline-legacy-validate=1 offline-legacy-pm=1 offline-legacy-phase=4
offline-legacy-retire=1 nabu-fbcon=0 nabu-gpu=1 nabu-touch=1
nabu-wdt-arm-delay=0 nabu-wdt-off=1 sheng-smp=1 nabu-usb-reboot=1
sheng-aks=1 sheng-gpu-addon=1 offline-legacy-transfer=1 sheng-fb-gpu=1
t733-managed-rt-normal=1 sheng-mig=1 offline-key-events=1 nabu-state-ring=10
nabu-cpufreq=1 nabu-ddr-readback=1 nabu-gpuclk=1 trace=16384 trace_wrap=1
```
**无 gate**：`nabu-panic-trig` / `nabu-sysreg-write` 都不在。

**boot 镜像红线**：`ead4b53135d67516` / 3,041,280 B（**永不重建**）

---

## 2. 【最高优先级】纪律（每条都由事故换来，违反一次白烧一轮构建+上板）

### 2.1 构建侧防污染五步（改任何内核/驱动文件之前）

**完整版**：`E:/nabu/state/DISCIPLINE-20261123-anti-pollution.md`（434 行）

```bash
# ① 是源还是产物？命中 scripts/*.py 就是产物 -> 必须改生成器
X=/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree
grep -l "$(basename <file>)" $X/scripts/*.py

# ② 哪份被读？读构建记录的 "file" 字段（唯一权威判据）
python3 -I -c "import json;o,_=json.JSONDecoder().raw_decode(open('<build>/<unit>.o.json').read());print(o['file'])"

# ③ 挂载生效？两份 inode 必须相同
stat -c '%i %n' <out-v6路径> <l738-full路径>

# ④ 改前快照
python E:/nabu/tools/kernel_edit_guard.py snapshot <tag> <file>

# ⑤ 编后验证对象比源码新
python E:/nabu/tools/kernel_edit_guard.py object <unit>.o.json
```

**四个「产物」（由 `scripts/nabu-panic-log.py` 生成，只能改生成器）：**
```
osfmk/console/serial_general.c
osfmk/kern/debug.c
pexpert/arm/pe_init.c
osfmk/arm/sleh.c
```

### 2.2 【三个真源，全部踩过坑——必背】

| 东西 | **真源** | 陷阱 |
|---|---|---|
| **生成器** `nabu-panic-log.py` | **`/root/merge/l738-device/scripts/`**（`ctx.device`）| `tree.py:assemble()` 的 `bases` 最后一个是 device，覆盖前面；`out-v6` 那份永不刷新（size+mtime skip 规则）；`/root/fresh` 那份**构建完全看不见** |
| **UFS 驱动** | `l738-device/drivers/NativeUfs` | 经 `tree/drivers/ShengUfs` 符号链接（名字不一致）；目录里几十个 `.pre-l*` 是备份，活源是不带后缀的 |
| **GPU 驱动** | **`UniXNU-nabu/drivers/NabuAdreno`** | `build-nabu-adreno-step1.py:30-34` 的回退路径；`tree/drivers/NabuAdreno` **不存在**；两份内容相同，改错那份"看起来改成了"但构建看不到 |

**生成器版本号是**三处**，必须同步 +1：**
```python
LOG_VERSION = 42                              # ① 判断/引用
LOG_BLOCK = r'''/* nabu-log-version: 42 */''' # ② 硬编码在产物里（曾长期停在 31）
marker = '/* nabu-log-version: %d */' % LOG_VERSION   # ③ 查找
```
**不同步的后果**：①③ 一致但 ② 落后 ⇒ 每次构建重复 splice；改了 `LOG_BLOCK` 但没 bump ⇒ 生成器 skip，改动永不生效。

**绝对不能删 `.o`** —— `archive-01-build-corecrypto-14` 在 XNU 编译**之前**用 `nm` 扫所有 `.o`，缺一个整轮失败。**强制重编用 `touch <源文件>`。**

### 2.3 上板侧八条（用户亲自纠正，原话引用）

| # | 规则 | 用户原话 |
|---|---|---|
| 1 | **10 秒轮询，永不用 `sleep 30/48/60`** | 「他的42秒枚举出来，是你已经跑了第二个48了」|
| 2 | **先起控制台，再刷机** | 否则早期日志全丢 |
| 3 | **WDT 咬回判据：COM3 消失 AND adb `f91fa664 recovery`** | ❌ **`adb devices` 空 ≠ 掉线**（系统态 adb 本来不上线）|
| 4 | **复位一律手动发，禁 `--arm-at`** | 「我需要的是手动复位的能力，我让你发就发」|
| 5 | **只用整合控制台，不开第二个串口工具** | 单句柄约束；抢 COM3 ⇒ `PermissionError(13)` |
| 6 | **不要盲试，用现成控制台读状态** | 「你为什么要用探针？我们不是有现成的控制台吗」|
| 7 | **不要自己下需求单** | 「你不要自己下需求侧，我要转才行」|
| 8 | **改源，不改产物** | 「你这样不碰构建源，以后他妈的再打包怎么办？又被污染了」|

**不要自己扫端口** —— 控制台每 2 秒报 `>>> scanning N.Ns` / `>>> CDC FOUND after N.Ns`。

### 2.4 【单句柄 CDC 铁律，已违反两次】

**一个 boot 周期只能 open 一次 CDC。`close()` 后再 `open()` 要么阻塞 ~120s，
要么 `ERROR_GEN_FAILURE(31)` / `PermissionError(13)`，只有板子复位才能恢复。**

**⇒ 换工具 = 破坏性操作。要么一个工具跑到底，要么复位重来。**

实测错误序列（kb-46 后第二次踩）：
```
控制台运行中 → 停掉 → sxnu_ctl open（第2次）→ PowerShell 裸读（第3次）
→ 手工 python（第4次）→ COM3 锁死，只能断电
```

**正确做法**：一个 tag、一个工具、中途不换。要换就复位。

### 2.5 【KDP 退出：两种场景，别搞混】

| 场景 | 能否退出 | 方法 |
|---|---|---|
| `Debugger()` 触发（`debugger` verb）| ✅ | `kdp resume` 发裸 `'c'` |
| **`panic()` 触发** | ❌ **只能断电** | `kdp reset`（裸 `'r'`）**也没用** |

**源码原因**：`kdp_getc()` → `console_try_read_char()` → `nabu_cdc_receive_ready()` →
`qcom_usb_debug_serial_pending()`：
```c
if (!kdp_rx_armed) return 0;        /* 既不 drain 也不报数 */
```
panic 时未 arm ⇒ 主机字节永远留在 DWC3 RX 环里没人读。

**两次修复尝试都失败**：
- kb-61（`!armed` 也 drain）：drain 需要 `acquire(&usb_busy)`，而 panic 常从**持有该锁的路径**发起（panic 打印走 `qcom_usb_debug_write`）⇒ **自锁**
- kb-62（无锁 `dwc3_read`）：**更糟**，panic 后完全静默，连 `panic(...)` 都不打印。worker 停了不等于硬件安全，TRB 环破坏连 TX 也死

**⇒ 代码已回滚，结论：不改 XNU 不可行。**

---

## 3. 【禁止命令】绝对不能用 / 会静默失败 / 会超时

### 3.1 绝对禁止

| 命令 | 后果 |
|---|---|
| **`wsl bash -c "... $var ..."`** | **变量被吞**。违反 ~8 次。**一律写 `.sh`/`.py` 文件再 `wsl bash /mnt/.../x.sh`** |
| **`wsl bash -lc`** | **会卡住**（login shell 加载 profile）。用 `MSYS_NO_PATHCONV=1 wsl.exe --exec bash /path/x.sh` 或 `--exec bash -c '...'` |
| **`grep` 125 MB `kernel.macho`** | 极慢。用 `/usr/bin/llvm-nm` / `llvm-objdump` |
| **`grep` 136 MB `logs/kernel.log`** | 1,123,563 行。用 `tail` 或 `grep -n " error:"` |
| **删 `.o`** | 破坏 corecrypto 的 `nm` 扫描，整轮失败 |
| **`--arm-at` 自动复位** | 用户明令删除 |
| **`sleep 30/48/60`** | 用户明令 |
| **多工具反复开关 COM3** | 锁死，只能断电 |
| **截断 `<tag>-cmd.txt`** | `cmd_pos` 按字节偏移，截断后命令被跳过 |

### 3.2 会静默失败 / 超时

| 命令 | 现象 | 正确做法 |
|---|---|---|
| `git push` | 卡在 credential-manager（rc=124）| `GIT_TERMINAL_PROMPT=0` + 重试；用户网页已登录 |
| `awk 'strtonum()'` | mawk 无此函数 | Python（`/e/nabu/tmp/sym_lookup.py`）|
| 裸 `nm`/`objdump`/`strings` | WSL 的不认 Mach-O | `/usr/bin/llvm-nm`、`llvm-objdump`、`llvm-strings` |
| `python3 -I /mnt/c/...` | Windows 路径被改写 | `MSYS_NO_PATHCONV=1 wsl ...` |
| **`python3` 在 Git Bash** | **不存在** | 用 `wsl.exe --exec bash -c '...'`，或 shell 算术 `$(( ))` |
| **`llvm-strings \| grep -x "str"`** | **带前导空格的字符串查不到**（控制台输出多是 `" state=..."`）| 用非精确 `grep "str"` |
| `bash -c` 里 heredoc + `"""` | 冲突 | 写独立 `.py` 文件 |
| `grep -c` 返回 1 | `set -e` 脚本提前退出 | 用 `|| true` |

### 3.3 本项目特有禁忌

| 禁忌 | 原因 |
|---|---|
| 对 `0x9b000000` 任何写/映射 | 无后备内存（kb-51 panic / kb-52 卡死）|
| `READPA`/`WRITEPA` 打未白名单地址 | 卡死 worker（kb-46/47）|
| `RDBLK smmu` / 写 SMMU | 崩内核（kb-47/49）|
| `WRITEPA 0x17c10008 00000001` | 会武装看门狗 |
| `ml_io_unmap` | panic `vm_map_delete` |
| `phystokv` 读 MMIO | panic `illegal PA` |
| `delay_us(>10ms)` | 置 `io_failed`，日志停发 |
| **在 panic 路径调 worker 驱动的硬件函数** | kb-62：TRB 环损坏，连 TX 都死 |
| **写 `TPIDR_EL1`** | 它是 XNU 的 `current_thread()`，写随机值内核静默卡死（kb-60）|
| **用旧内核的符号地址读新内核** | 每次重链接 BSS/data 地址都变（`_kdebug_enable` 从 `0xc185a18` → `0xc189a18`）|
| 主动触发 panic 去测 KDP 退出 | 拿设备冒险换不需要的功能 |

---

## 4. 全部基线 tag（5 个）

| tag | kernel sha16 | 能力 | 用途 |
|---|---|---|---|
| **`baseline-kb99-fivefeatures`** | **`f14f0ce6d3331527`** | ✅ **最全**：48 verb + TSENS + JOBSTAT + SLOW + CPSTAT + IRQSTAT | **当前基线** |
| `baseline-kb70-kdbgfix` | `3480d5c12b7edb2a` | 上一个稳定基线（启动 207 s）| 回滚点 |
| `baseline-kb68-stackshot` | `c129d83cee9ceb63` | 有 STACKSHOT，无 KDBG 修正 | 上一基线 |
| `baseline-kb67-hfifix` | `2222a807b9c98e63` | 有 HFI，无 STACKSHOT | |
| `baseline-kb59-ufsoffset` | `9cc32cf0f07f693f` | 方案 D 闭合，无 HFI/STACKSHOT | |
| `baseline-kb56-readufs` | `68d1ad7b873fa9b4` | READUFS | |

**失败包（勿用，仅历史）**：
`kb-51-panicregion`（boot panic）、`kb-52-panicdefer`（卡死）、
`kb-60-sysregw`（TPIDR 卡死）、`kb-61-panicreboot`（drain 自锁）、
`kb-62-lockfree`（panic 静默）——**注意 kb-62 是回滚过的，代码里保留失败注释**

**归档**：`hold/_baseline_archive/20261123/`（**60 文件**，SHA256SUMS 全 OK）
**快照**：`hold/_baselines/<tag>/`（kernel + adt + payload）

---

## 5. 48 个 verb 全表（全部真机验证）

| 类别 | verb | 关键输出 |
|---|---|---|
| 基础 7 | `HELLO` `PING` `STATUS` `BOOTARGS` `VERSION` `UPTIME` `REBOOT` | |
| 内存读 7 | `READ` `READNEXT` `READPA` `READPANEXT` `RDBLK` `DUMP` `DUMPPA` | |
| 内存写 2 | `WRITE` `WRITEPA` | 白名单 |
| UFS 3 | `DUMPUFS` `DUMLIST` `READUFS` | `readufs header/panic/8166@4032` |
| 调试 2 | `DEBUGGER` `KDPPUSH` | |
| 页表/panic 3 | `PTDUMP` `PANICDUMP` `PANICTRIG` | |
| 系统 7 | `SYSCTL` `TASKDUMP` `THREADDUMP` `SYSREG` `SYSREGW` `SYM` `SYMS` | |
| 显示 6 | `SCREENSHOT` `SHOTNEXT` `VSYNCON` `FLIPNOW` `DISPREGS` `SMMUDUMP` | |
| GPU 6 | `GMUSTATE` `GPUFREQ` `GPUHWCG` `GPUPROFILE` `PERFSTART` `PERFSTOP` | `hfi_q/hdr/msg/err/ack` |
| PERF 2 | `PERFREAD` `PERFCLEAR` | |
| 断点 3 | `BKPADD` `BKPDEL` `BKPLIST` | |
| **诊断 2** | **`STACKSHOT`** **`KDBG`** | `size=134736` / `oldest_time=0x…` |

**`SYM`/`SYMS` 设计回 `unsupported`**（符号表在主机侧，53k 符号）。

### 5.1 关键 verb 的实测输出

```
KDBG        OK enabled=0 mode=0 live_flags=0x00000000 kdc_flags=0x00000001 cpus=40 oldest_time=0x2fcfa247
TASKDUMP    OK ... layout=ok task_map=0x… threads=0x… nthreads=75 prio=81 maxprio=95 prio_raw=005f0051 task_ok=1
THREADDUMP  OK ptr=0x… owner_task=0x… thread_ok=1 w0=1234abcddcba4321(THREAD_MAGIC) …
STACKSHOT   OK state=captured rc=0 buf=0xfffffff9b4e0b000 size=134736
GMUSTATE    OK step=7 chip=6401cafe fence=1 completed=1 hfi_q=1 hfi_hdr=0061030a hfi_msg=0xa hfi_err=0 hfi_ack=7
READUFS 8166@4032 → "PANICSTR: nabu: PANICTRIG requested by host"
```

---

## 6. 已完成的三大能力（都有真机证据）

### 6.1 方案 D（panic 日志落 UFS）：完全闭合

**7 环证据链：**
```
1. 环在产出        nabu_log_total 单调递增
2. 落盘被调用      nabu_log_opened=1（两次独立启动复现）
3. 落盘成功        flushed 跟随 total（124,516 → 584,725）
4. 介质内容正确    READUFS 1 → "bin/bluetoothd rd=... rt=... vb=..."
5. panic 落盘      READUFS panic(rel 8190) → NABULOG1 is_panic=1
6. 两槽不污染      周期 is_panic=0 与 panic is_panic=1 并存
7. panic 文本      READUFS 8166@4032 → "PANICSTR: nabu: PANICTRIG requested by host"
```

**槽位布局**（窗口 = 8192 块 = 32 MiB）：
```
周期落盘：rel 0 (头, is_panic=0) + rel 1..25 (尾)
panic 落盘：rel 8190 (头, is_panic=1) + rel 8166..8189 (尾)
```

**`NABULOG1` 头（7 个 u32 = 32 字节，小端）**：
```
+0  "NABULOG1"(8B)  +8 version=1  +12 is_panic  +16 available
+20 dropped         +24 total_lo   +28 total_hi
```

**两层锁的修复（生成器 v40/v41）**：
1. `nabu_log_periodic()` 删掉 `nabu_log_opened` 门
2. `native_ufs_log_open()` 的 `"already open"` 当成功

### 6.2 T47 偏移：全部验证 + 结构化

| 项 | 实测 |
|---|---|
| `_proc_struct_size` | **`0x790`**（T47 地址笔误：`0x7b011a8` → 实际 `0x7b051a8`）|
| `p_pid` @ +0x60 | `kernel_task` = 0 ✅ |
| `p_comm` @ +0x5bc | `"kernel_task"` ✅ |
| `task->map`/+0x50 `threads`/+0x80 `thread_count`/+0xa8 | 合法指针 / **75** ✅ |
| **`priority` @ +0xbc** | **两个相邻 int16**：`prio=0x51=81`、`max_priority=0x5f=95` ✅ |
| **`thread→task`：`t_tro(+0x438)` → `*(u64*)(tro+0x28)`** | `owner_task` 完全一致 ✅ |

**自检门**：`_proc_struct_size != 0x790` 就报 `layout=stale`，不解码。

### 6.3 T48 三条

| 项 | 结果 |
|---|---|
| ② STACKSHOT | ✅ **打通**（`kern_stack_snapshot_with_reason`，34→131 KB kcdata）|
| ③ SYSCTL 巡检 | ⚠️ `kern.kdbg.*` 是 `CTLFLAG_LOCKED` 读不到；**已用 `KDBG` verb 替代** |
| ① kdebug 镜像 | ❌ **建议不做**（需改 XNU；`ktrace_lock` 是 mutex 会冻结 CDC）|

**`panic.trace` 读不回来**：落在 Data 卷 APFS，我们只有 `READ`/`READPA`/`READUFS`。

### 6.4 Q2 HFI：逐字段对上

`hfi_msg=0xa`、`hfi_err=0`、`hfi_ack=7` 与驱动日志 `gmu=15 ... hfi=a/0 ack=7` **逐字一致**。

**第一版失败**：导出放在 `observeInitialization()`（`idle()` 回调），
**`idle()` 在 a640 发布后停止调用** ⇒ kb-66 全 0。移到 `A640State::poll()` 后正常。

---

## 7. 关键地址（kb-70 实测，**每次重链接会变，必须现查**）

**查法**：
```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c '/usr/bin/llvm-nm /e/nabu/hold/<tag>/kernel.macho | grep _符号名'
```

| 符号 | kb-69/70 地址 | 说明 |
|---|---|---|
| `_kdebug_enable` | `0xfffffff00c189a18` | 全局开关（kb-65 时 `0xc185a18`）|
| `_kd_control_trace` | `0xfffffff007b04600` | `struct kd_control` |
| `_write_trace_on_panic` | `0xfffffff0071f4204` | `trace_panic` tunable |
| `_proc_struct_size` | `0xfffffff007b051a8` | `= 0x790` |
| `_kernproc` / `_tasks` | `0xfffffff0071f0cb8` / `0xfffffff00c11d6d0` | 导出锚点 |
| `_kernel_stackshot_buf` / `_size` | `0xfffffff00c118048` / `...050` | STACKSHOT 结果 |
| `_native_adreno_a6xx_stats` | `0xfffffff00c1b2520` | 24 个 u32（HFI 在槽 2/3/14/15/19）|

**`kd_control_trace` 字段偏移（反汇编验证，不是手算）**：
```
+0x08  enabled:1, mode:3
+0x1c  kdebug_cpus
+0x24  kdc_flags
+0x2c  kdc_live_flags
+0x30  kdc_oldest_time   ← 注意不是 +0x38（union kds_ptr 是 4 字节位域不是指针）
```

**运行时变量**：
```
nabu_log_opened   0xfffffff00c5d5cc8   (=1 表示落盘窗口已开)
nabu_log_flushed  0xfffffff00c5d5cd0
nabu_log_total    0xfffffff00c166960
nabu_log_dropped  0xfffffff00c5d5cb8
gLogRegionFirst   0xfffffff00c5fd730
```

**控制协议**：
```
SXNU_CONTROL_LINE_BYTES=127  REPLY_BYTES=512  FRAME_BYTES=128
usb_control_context 0xfffffff00c81e008  (+0x90 session, +0x98 last_seq, +0x9c generation)
```

**KDP**：
```
exception_seq              0xfffffff00c1aa628  ⚠️ unsigned char（1 字节！）
kdp_explicitly_requested   0xfffffff0071b9514
debug_boot_arg             0xfffffff0071b9518
```
**请求号是十六进制**：`CONNECT=0x00 DISCONNECT=0x01 HOSTINFO=0x02 READMEM64=0x14 WRITEMEM64=0x15 REBOOT=0x13`

---

## 8. 构建流程（逐条，复制粘贴可用）

### 8.1 模块单测

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'cd /mnt/d/_nabu-usb-cdc/module && clang -std=c11 -D_GNU_SOURCE -Wall -Wextra -I. tests/test_usb_control_args.c -o /tmp/t && /tmp/t | tail -3'
# 期望 ALL PASS
```

### 8.2 编模块 .a

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'rm -rf /tmp/qud-x && cd /mnt/d/_nabu-usb-cdc/module && python3 -I scripts/build.py \
  --compile-record /root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_kprintf.o.json \
  --soc soc/Sm8150.h --ar /usr/bin/llvm-ar --nm /usr/bin/llvm-nm --output /tmp/qud-x'

# 查错误（build.py 会吞）
grep -hE "error:" /tmp/qud-x/*.log

# 放两处（sha 必须一致）
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c '
cp /tmp/qud-x/libQcomXnuUsbDebug.a /root/merge/l738-device/build/
cp /tmp/qud-x/libQcomXnuUsbDebug.a /root/merge/UniXNU-nabu/build/
sha256sum /root/merge/{l738-device,UniXNU-nabu}/build/libQcomXnuUsbDebug.a | cut -c1-16'
```

**⚠️ 白名单会拦新符号**：`RuntimeError: unexpected XNU imports: ['_xxx']`
⇒ 加到 `module/scripts/build.py` 的 `allowed` 集合。

### 8.3 内核构建（后台，等通知）

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash /mnt/e/nabu/tmp/build_kb53.sh
# 后台跑。里面是 /tmp/_build_inner.sh：mount --bind 三处 + unixnu build kernel -j4
# 成功标志：日志出现 "sha256 <64hex>"，且 grep -cE " error:" = 0
# 约 3-4 分钟
```

### 8.4 NOP18 + 打包

```bash
# NOP18（期望 PATCHED / nop18 1）
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'python3 /mnt/e/nabu/tools/_nop18_l800.py'

# 新脚本（不改旧的！）
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'sed "s/^TAG=kb-56-readufs$/TAG=<newtag>/" /mnt/e/nabu/tmp/pack_kb56.sh > /mnt/e/nabu/tmp/pack_<newtag>.sh; chmod +x /mnt/e/nabu/tmp/pack_<newtag>.sh'
MSYS_NO_PATHCONV=1 wsl.exe --exec bash /mnt/e/nabu/tmp/pack_<newtag>.sh

# 同步 args（会覆盖！先 diff）
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'cd /mnt/d/_nabu-usb-cdc/tools && python3 -I sync_payload_args.py /mnt/e/nabu/hold/<tag>'
```

**必须核对**：
- 三条 sha 一致：host kernel == payload 内 kernel == hold 副本
- boot 红线：`sha256sum boot-merge-r3.img | cut -c1-16` = `ead4b53135d67516`
- args：`python3 -I /mnt/e/nabu/tmp/args_count.py <payload.img>`

**⚠️ 打包模板的致命陷阱（kb-73 踩过）**：
`pack_kb56.sh` 里是 **`BASE=/mnt/e/nabu/hold/kb-50-readall`** ——它从 **kb-50** 复制 payload！
kb-50 的 args 是 **`n=48 c=862`**（那时还没有 `trace=16384 trace_wrap=1` 等）。
**直接从模板打出来的包，args 是旧配置。**
**正确做法**：打完包后**再把当前基线的 payload 覆盖回去**，或改 `BASE` 指向当前基线：
```bash
cp -f /e/nabu/hold/<当前基线>/payload.img /e/nabu/hold/<新tag>/payload.img
# 重新 mcopy kernel 进去
mcopy -o -i <新tag>/payload.img "$K" ::/EFI/UniXNU/Payloads/kernel.macho
# 再 sync args
python3 -I tools/sync_payload_args.py /e/nabu/hold/<新tag>
```
**验收：args 条数必须 == board.toml 的条目数**（kb-73：board.toml 51 条 → `n=51 c=907`）。
**并从 payload 内按 FAT 路径读 `unixnu.toml` 确认 gate 真的在里面**（读 `args_fp.py` 会读到 FAT 残留的旧串）：
```bash
mtype -i <payload.img> ::/EFI/UniXNU/unixnu.toml | grep -o "nabu-sysreg-write=[0-9]*"
```

### 8.5 boot-args 改动（必须走 board.toml）

```bash
# 两棵树都要改
/root/merge/l738-device/board.toml
/root/merge/UniXNU-nabu/board.toml

# dry-run 确认
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'cd /root/fresh/UniXNU-latest && python3 -I unixnu payload --out /root/merge/out-v6 -D /root/merge/l738-device --dry-run | grep "== kernel args:"'
```

**⚠️ `sync_payload_args.py` 无条件用 `board.toml` 覆盖**（曾静默丢掉 `DB_KDP_GETC_ENA`）。

**⚠️ payload 里有两份 args 字符串**（FAT 残留没擦），`args_fp.py` 会读到**旧的**。
**正确方法**：`mtype -i <img> ::/EFI/UniXNU/unixnu.toml` 按路径读。

---

## 9. 上板流程（逐条）

### 9.1 前置检查

```bash
adb devices                                    # 必须 f91fa664 recovery
powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames()"   # 必须只有 COM1
powershell -NoProfile -Command "Get-Process python -ErrorAction SilentlyContinue | Select Id"
```

### 9.2 六问报告 + gate

`E:/nabu/state/board-report-<tag>.md` 六节：
① 唯一问题 ② 已闭合到哪 ③ 为什么离线不行 ④ 失败也有信息 ⑤ 唯一变量+回滚点 ⑥ 恢复预案

```bash
cd /e/nabu && python tmp/board_gate.py --tag <tag> --baseline <上一包> \
  --question "..." --logs "E:/nabu/logs/<t>-ctl.txt" --window "41-70s" --args "n=50/c=887" \
  --device "recovery f91fa664" --chain "..." --why-board "..." \
  --peer "kernel=<16hex>,payload=<16hex>,adt=211568,args=n50/c887" \
  --mine "kernel=<16hex>,payload=<16hex>,adt=211568,args=n50/c887"
# 期望 [board_gate] ALL PASS
```

**双审计 `peer` 语义**：**独立测量值**（从 payload 内 `mcopy` 出 kernel 算 sha），**不是基线值**。
`args=` 必须写实际值（如 `n50/c887`，不是 `n=50/c=887`）。

**常见拒绝**：`D-board-report`（六问缺）、`F-dual-audit`（peer/mine 不等或缺 args=）、
`smp-arm`（`sheng-smp=1` 时 arm-delay 必须 0）、`token-diff`（`--expect-added` 与实际不符）

### 9.3 起控制台（**必须早于刷机**）

```bash
cd /d/_nabu-usb-cdc && python -u tools/nabu_console.py --tag <t> --out E:/nabu/logs \
  --wait 300 --deadline 900 --stall-exit 400 --kernel /e/nabu/hold/<tag>/kernel.macho
# 后台跑。2 秒间隔扫 COM，扫到立刻打 >>> CDC FOUND after N.Ns
```

### 9.4 刷机

```bash
cd /e/nabu && bash tmp/go_round_next.sh <tag>
# 内部：gate → boot sha 校验 → arm misc → adb reboot bootloader
#      → 等 fastboot（5 秒一次/90 秒）→ flash cust → re-arm misc → fastboot boot
# ⚠️ 绝不能刷 boot_a（会清 misc 的 boot-recovery 标记，导致循环回同一内核）
```

### 9.5 等 CDC（**10 秒轮询**）

```bash
sleep 10; tail -2 "<console-stdout.log>"    # 看 >>> CDC FOUND / OPEN OK
# 基线枚举 42–57 秒
```

### 9.6 发命令（**只用 `>>` 追加，绝不截断**）

```
hello
kdbg
stackshot
gmustate
taskdump <kernproc+0x790>
threaddump <threads>
readufs panic
readufs 8166@4032
dump 0x<addr> <len> out.bin
```

**回复在 `<tag>-ctl.txt`，控制台 stdout 也有。**

### 9.7 收尾

```bash
printf 'reboot\n' >> E:/nabu/logs/<t>-cmd.txt     # PSCI 直发，~16 秒回 recovery
```

### 9.8 判据强度

| 判据 | 强度 |
|---|---|
| `Using serial KDP.` | 弱（须恰好 2 行）|
| `Options.` 只出一行 | **无效**（会假阴性，必须同包重跑）|
| `Continuing...` / `Connected to remote debugger.` | ★★★ |
| `Please go to panic.apple.com` 消失 | ★★★（DISCONNECT 干净）|
| 日志出现 `0xFA…0xFB` 二进制帧 | ★★★ |
| `kdp: bad sequence N (want M)` | ★★★（负面）**`M` 就是该用的值** |

---

## 10. 为什么某些功能不做（技术结论，附源码依据）

| 功能 | 不可行原因 |
|---|---|
| **panic→KDP 继续运行** | 返回条件要 `DBOP_DEBUGGER`，panic 是 `DBOP_PANIC`；且 `serial_pending` 在 `!armed` 时 return 0，两条读路径都不通 |
| **panic→KDP 软件复位** | 同上；两次修复尝试都失败（kb-61 自锁 / kb-62 破坏 TRB 环）|
| **`trace_panic=1`** | 落 `/var/log/panic.trace` = Data 卷 APFS，我们无 APFS 通道 |
| **原生 panic stackshot** | `panic_stackshot_to_disk_enabled()` arm64 恒 FALSE 且**无调用者**；`panic_stackshot_buf` 是 x86-only |
| **`gPanicBase`（方案 A）** | `0x9b000000` 无后备物理页，kb-51 panic / kb-52 卡死两轮证伪 |
| **kdebug 事件镜像** | `kd_buffer_trace` 未导出；`kdbg_dump_trace_to_file` 的 `ktrace_lock` 是 mutex（会冻结 CDC）+ 内部走 VFS。**安全版**：从 `nabu_log_periodic`（2 秒后台线程）触发 + `reenable=true`，但目标卷是 APFS ⇒ 仍读不回 |
| **DTrace** | `CONFIG_CSR` 关（不用过 CSR），但缺用户态 `dtrace`/`dtruss` 工具 |

---

## 11. 踩坑清单（67 条，编号 1–68，按主题分组）

### 11.1 构建/环境（最致命）

| # | 坑 | 后果 | 正确做法 |
|---|---|---|---|
| 36 | **陈旧 `.o` 让源码改动静默失效** | `debug.o` 曾是一周前 | 改完核对对象比源码新 |
| 37 | **删 `.o` 破坏构建** | corecrypto 的 `nm` 扫不到 | `touch` 源文件 |
| 40 | **生成器每次构建重写源文件** | 改 4 个产物无效 | 改生成器 |
| 41 | **生成器有两份，构建不刷新副本** | 改真源不生效 | 两份都改或 `cp` |
| 42 | **生成器版本号是三处** | 不同步就重复 splice / 永久 skip | 三处一起 +1 |
| 43 | **生成器真源是 `ctx.device`** | 改 `/root/fresh` 那份构建看不见 | 改 `l738-device/scripts/` |
| 44 | **GPU 驱动真源是 `UniXNU-nabu`** | 改 `l738-device` 那份构建看不见 | 查 `build-nabu-adreno-step1.py` |
| 2 | **`wsl bash -c "... $var ..."`** | 变量被吞 | 写 `.sh` 文件 |
| 3 | **`grep` 125 MB 内核** | 极慢 | `llvm-nm`/`llvm-objdump` |
| 9 | `-fsanitize-minimal-runtime` 报"多输出文件" | 单文件重编失败 | 只改 `args[2]`(源) 和 `args[4]`(输出) |
| 11 | `.o` 段偏移 ≠ 链接后地址 | 错误结论 | 只在链接后 `kernel.macho` 反汇编 |
| 12 | objdump 符号注释会瞎标 | 误导 | 用 `llvm-nm` 查真地址 |
| 26 | `adrp` 后跟 `nop` 不是 bug | 一度以为 GOT 没解析 | 那是 `GOT_LOAD` 正常形态 |
| 45 | **`llvm-strings \| grep -x` 查不到带空格串** | 误判"没编译进去" | 用非精确 `grep` |
| 46 | **每次重链接 BSS/data 地址都变** | 用旧地址读到 0 | 换包后重新 `llvm-nm` |
| 47 | `bash -c` 里 heredoc + `"""` 冲突 | 脚本语法错 | 写独立 `.py` |
| 55 | **`wsl bash -lc` 会卡住** | 命令超时 | 用 `--exec bash -c` 或 `--exec bash /path` |

### 11.2 协议/控制台

| # | 坑 | 正确做法 |
|---|---|---|
| 13 | `DUMPLIST` vs `DUMLIST` 一字之差 | 两种都接受 |
| 14 | `wait_reply` 按 verb 名匹配 | **按 seq 匹配** |
| 15 | 一次失败让会话永久失配 | 超时后 `HELLO` 重同步 |
| 16 | `READPA`/`RDBLK` 10s 不够 | 慢 verb 用 3× 超时 |
| 17 | 杀占用 COM3 的 python | 单句柄约束 |
| 19 | `KDP_REBOOT` 是 `0x13` 不是 13 | 分发表注释是**十六进制** |
| 20 | `exception_seq` 是 `unsigned char` | 按真实宽度写 |
| 24 | 用编译期常量推断运行时配置 | 看 `TCR_EL1.TG1` 运行时值 |
| 29 | `board_config.h` 覆盖 `pe_init.c` 顶部 `#define` | `clang -E -dM` 看最终值 |
| 30 | `adb devices` 空 ≠ 内核死 | 判活：COM3 能 open + HELLO 有回复 |
| 48 | **截断 `<tag>-cmd.txt`** | **只用 `>>`**，要重来就换 tag |
| 49 | **多工具反复开关 COM3** | 一个工具跑到底 |

### 11.3 硬件/内存（最危险）

| # | 坑 | 正确做法 |
|---|---|---|
| 21 | **XNU 的 `TCR_TG0`/`TG1` 编码与 ARM 手册相反** | `1=16K, 2=4K, 3=64K` |
| 22 | 模块里手算 V=P 地址 | `physmap_base` 随机滑动；用 `ml_io_map` |
| 27 | `READPA` 打不存在的 PA 卡死 worker | 白名单 + `pa-rd:` 日志 |
| 31 | 写白名单不能当读白名单 | 两张表分开 |
| 32 | MMIO「存在」≠「读它安全」 | 只放真机验证过的块 |
| 33 | 砖板后靠猜 = 每次一轮断电 | `memcpy`/写之前打 `pa-rd:`/`pa-wr:` |
| 35 | **写白名单 ≠ 可安全写** | `WRITEPA 0x01000000 0` 写死内核 |
| 38 | **`ml_io_map` 两条路径行为相反** | boot 早期不能映射任意地址 |
| 39 | **「能读」≠「能写」** | 读返回 0 只证明事务完成 |
| 50 | **`TPIDR_EL1` 是 `current_thread()`** | 写随机值内核静默卡死（kb-60）|
| 51 | **`gLogRegionBlocks` 被编译器折叠成布尔** | 读到 1 是正常的，不要判驱动坏 |
| 52 | **panic 路径不能调 worker 驱动的硬件函数** | kb-62：TRB 环损坏，连 TX 都死 |
| 53 | **"无锁"不等于"安全"** | 锁保护的不只是互斥，还有状态一致性 |
| 56 | **`union kds_ptr` 是 4 字节位域不是指针** | 结构镜像会算错偏移，用反汇编验证的硬偏移 |

### 11.4 其它

| # | 坑 |
|---|---|
| 4 | 写 `exception_seq`（1 字节）4 字节 ⇒ 协议坏死，`KDP_REBOOT` 也救不回 |
| 5 | **`QcomUsbControl.h` 有两份**，`build.py` 的 `-I` 里 `/root/dwc3/vendor/` 排前面 |
| 8 | 回退时按文件名判断备份属于哪一步（`debug.c.REALSRC.orig` 名字像"原始"实际更早）|
| 18 | `nabu-wdt-arm-delay=86400` 不是关狗（只延迟武装），真开关是 `nabu-wdt-off=1` |
| 23 | 失败分支不给诊断字段 ⇒ 一次板轮只学到"不行" |
| 25 | 只报 `unmapped` 的 ERR 无法定位 |
| 34 | `board.toml` 会与实际 payload 漂移；`sync_payload_args.py` 会静默弄丢 `DB_KDP_GETC_ENA` |
| 54 | 主动触发 panic 去测 KDP 退出 = 拿设备冒险换不需要的功能 |
| 57 | `idle()` 是 IOKit 初始化回调，**a640 发布后停止调用**（HFI 导出放错位置，kb-66 全 0）|
| **58** | **`nabu: spawn … dur=/t=/rt=` 的单位是 `mach_absolute_time()` 裸 tick（19.2 MHz），不是微秒。** 把 `dur=25857324` 读成 "25.8 s" 实际是 1.35 s，差 1920 万倍。**换算一律 `/19200000`** |
| **59** | **`QCOM USB LOSS contention=` 是真丢字节**，`dropped = overflow + contended`（`QcomUsbBuffer.h:73-77`）。**`STATUS rxo=0` 只代表控制环没溢出，不代表广播日志零丢失** |
| **60** | **`native_ufs_block_reads` 不是块数**，是 `RootWindowRead.h:53` 的窗口 copy **迭代次数**（每次 while 迭代调一次 `ops.copy`）。**不能乘 4096 当字节数** |
| **61** | **`native_ufs_read_ticks` 是全局累计值**，跨所有读者；**不能把某窗口的增量全算在单个进程头上**，只能作趋势证据 |
| **62** | **在白名单里 ≠ 现在能读。** kb-72：`readpa 0x0105c000`（GCC，**白名单内**）照样卡死 worker，日志与 HELLO 同时停。全因是「时钟域此刻是否是开的」静态表无从得知。**GCC 已在 kb-73 移出、kb-75 临时放回做二分**——用前先看当前白名单状态 |
| **63** | **CTL_FLUSH（`0xae02018`）是自清零寄存器。** 驱动每帧写 `0x3`（`NativeLegacyDisplay.cpp:535`），写前必须读到 `0`（`:506`）。**写完立刻 `READPA` 读回 `0` 是成功（已消费），不是失败。** 拿它做 WRITEPA 值变化验证最安全 |
| **64** | **不要用 READPA 探测未知寄存器。** 正确方法是**从驱动源码里找驱动自己会写的寄存器**（如 `nabu_wdt_pet()` 写 WDT `+0x08`、`flip()` 写 `CTL_FLUSH`），用源码证据定目标。kb-72 的卡死就是"探测"方法本身的错，不是地址选得不巧 |
| **65** | **两种失效模式要分清**：① `open` 成功但 OUT 死（IN 仍通，日志还在流）= 第二次 open 同一 boot；② IN/OUT 全死（日志停更）= worker 卡在 MMIO。判据分别是「HELLO 有无回复」和「`com.bin` 是否还在长」|
| **66** | **`pack_kb56.sh` 的 `BASE` 指向 kb-50，args 是旧的 `n=48 c=862`。** 直接从模板打出来的包 args 全是旧配置。**打完必须用当前基线的 payload 覆盖 + 重 mcopy kernel + sync args**，验收看 `args 条数 == board.toml 条目数` |
| **67** | **改 boot-arg = 必须重打 payload = 镜像 62% 字节全变。** kb-70 与 kb-75 比：`unixnu.toml` 只差 args 一行、FAT 目录全同、`scratch-data.img` 逐字节相同、模块符号地址相同——**但 payload 有 125,511,466 字节不同**（`mcopy` 写 125 MB 内核时重排了 FAT 簇）。两次（kb-73/kb-75）都因此卡在 `disk1s1` Data 卷挂载，`mount-complete` 永不出现。**⇒ 能不改 boot-arg 就不改。** |
| **68** | **gate 可以运行时打开，不用刷机。** `_qud_sysreg_write_enabled`（`0xfffffff00c0f5a70`，`.data` 导出）初值 `-1`；`WRITE 0xfffffff00c0f5a70 01000000` 后 gate 即开，`if (< 0)` 永不成立、boot-arg 根本不会被读。**同类 gate 都可用这招**（找 `volatile`/`static int xxx_enabled = -1` 的导出变量）|

---

## 12. 剩余工作 / 下一阶段

| 优先 | 项 | 状态 |
|---|---|---|
| **高** | **保持 kb-70 为基线，收尾归档** | ✅ 已完成 |
| 中 | T48 ① kdebug 事件镜像进 ring | **建议不做**（需改 XNU，风险碰 CDC 生命线）|
| 中 | STACKSHOT 可重复抓（置 NULL 不释放）| 可选（34→131 KB 泄几次无所谓）|
| 低 | `trace_panic=1` | **不可行**（APFS 读不回）|
| 低 | panic→KDP 软件复位 | 需改 XNU（`debug.c` 在 `kdp_connection_wait` 前复位）|
| 低 | 路线 A 的 20 条 NOMAP | `gpu.hfi.*` 已通过 GMUSTATE 部分覆盖 |
| — | DTrace | 中期（缺用户态工具）|
| — | 第二串口 / 屏幕流 | 等 T40 |

**反编译侧待办（T46/T47/T48/T50 已全部交付并落地）**：
- Q1 的"软件复位"如果要做，需要 XNU 侧配合
- 路线 A 剩余命令如果要做，需要定范围

---

## 13. 快速开始下一轮（复制粘贴）

```bash
# 1. 设备状态（唯一判据）
adb devices
powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames()"

# 2. 若卡死 -> 让用户断电；不要自己尝试软件复位

# 3. 起控制台（必须早于刷机）
cd /d/_nabu-usb-cdc && python -u tools/nabu_console.py --tag <t> --out E:/nabu/logs \
  --wait 300 --deadline 900 --stall-exit 400 --kernel /e/nabu/hold/kb-70-kdbgfix/kernel.macho

# 4. 刷机（如果设备已在系统态，先发 reboot 回 recovery）
cd /e/nabu && bash tmp/go_round_next.sh kb-70-kdbgfix

# 5. 10 秒轮询等 >>> CDC FOUND

# 6. 发命令（只用 >>）
printf 'hello\n' >> E:/nabu/logs/<t>-cmd.txt

# 7. 收尾
printf 'reboot\n' >> E:/nabu/logs/<t>-cmd.txt
```

---

## 14. 文档与工具索引

### 14.1 记忆与纪律

| 文档 | 内容 |
|---|---|
| **本文** `MEMORY-MASTER-20261123-1835.md` | **最新，680+ 行，自包含** |
| `MEMORY-MASTER-20261123-1825.md` | 上一份（kb-70 前）|
| **`DISCIPLINE-20261123-anti-pollution.md`** | **防污染纪律 434 行**（含 §2.2b/2.2d/2.2e 三个真源、§9 四条新红线）|
| `bridge/STATUS.md` | 状态快照 + 指向本文 |

### 14.2 本轮结果文档

| 文档 | 内容 |
|---|---|
| `RESULT-kb70-t50-done.md` | **T50 两个解法验证通过（最新）** |
| `RESULT-kb68-stackshot.md` | STACKSHOT 打通 |
| `RESULT-kb67-hfi.md` | HFI 逐字段对上 |
| `RESULT-kb65-taskdump.md` | T47 偏移全部验证 |
| `RESULT-kb69-t50-partial.md` | KDBG 偏移错误记录 |
| `CONCLUSION-Q1-panic-reboot-closed.md` | Q1 两次失败结案 |
| `T48-DELIVERY-20261123.md` | T48 结论核对 |
| `SYNC-TO-RE-20261123-1810.md` | **给反编译侧的同步（最新）** |
| `GAP-ANALYSIS-20261123.md` | 与原方案差距 |
| `BACKUP-INVENTORY-20261123-1600.md` | 备份盘点 |

### 14.3 工具

| 工具 | 用途 |
|---|---|
| **`D:/_nabu-usb-cdc/tools/nabu_console.py`** | **唯一串口控制台**（含 `stackshot`/`kdbg`/`readufs`/`panictrig`/`kdp reset`/`kdp resume`）|
| `D:/_nabu-usb-cdc/tools/route_a_bridge.py` | giulia 命令 → 我们 verb（13 OK / 20 NOMAP）|
| `E:/nabu/tools/kernel_edit_guard.py` | `snapshot` / `object` |
| `E:/nabu/tools/_nop18_l800.py` | NOP18 补丁 |
| `E:/nabu/tmp/args_count.py` | **按 FAT 路径读 payload args（正确方法）** |
| `E:/nabu/tmp/build_kb53.sh` | 内核构建入口 |
| `E:/nabu/tmp/pack_kb56.sh` | 打包模板（**复制成新脚本，别改它**）|
| `E:/nabu/tmp/sym_lookup.py` | 地址→符号（Python，替代 awk）|
| `D:/_nabu-usb-cdc/tools/sync_payload_args.py` | **会覆盖 args，先 diff** |
| `D:/_nabu-usb-cdc/tools/readpa_whitelist_check.py` | 读写白名单回归（22+7+6 条）|
| `D:/_nabu-usb-cdc/tools/pt_geometry_check.py` | PTE 几何离线验证 |
| `D:/_nabu-usb-cdc/tools/panic_header_check.py` | panic 头解析自测 |

---

## 15. 一句话交接

> **48 个 verb 全部真机验证；方案 D 完全闭合（`PANICSTR` 从 UFS 介质读回）；
> T47 偏移全部验证；STACKSHOT 打通（131 KB kcdata）；KDBG 绕过 sysctl 锁读到真
> `oldest_time`；`task->priority` 拆成 `prio=81 maxprio=95` 精确命中 T50 的推断。**
>
> **当前基线 `hold/kb-99-full`（kernel `f14f0ce6d33315272b29`），
> tag `baseline-kb99-fivefeatures`，HEAD `24113a8` 已推远端。**
>
> **改任何内核/驱动文件前：§2.1 五步 + §2.2 三个真源（生成器在 `l738-device/scripts`、
> UFS 驱动在 `l738-device/drivers/NativeUfs`、GPU 驱动在 `UniXNU-nabu/drivers/NabuAdreno`）。**
>
> **上板：一个 boot 只能 open 一次 CDC（§2.4），换工具就是破坏性操作。**
>
> **换内核包后按地址读任何变量前，先 `llvm-nm` 重查地址（§11.1 #46）。**
>
> **命令文件只用 `>>` 追加，绝不截断（§11.2 #48）。**
