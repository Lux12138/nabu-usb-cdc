# 最终记忆总档 2026-11-23 22:35（压缩上下文前的完整快照）

> **自包含。** 新会话读完这份就能接手，不需要任何前置上下文。
> 上一份：`MEMORY-MASTER-20261123-1835.md`（725 行，kb-70 时代）。
>
> **阅读顺序：§1 现状 → §2 三条铁律 → §3 构建（逐条命令）→ §4 打包 → §5 刷机 → §6 控制台 → §7 排查进度 → §8 坑 → §9 下一步。**

---

## 1. 一句话现状 + 全部关键数字

**XNU 17.7.11 移植到 Xiaomi Pad 5（nabu/SM8150）的 USB CDC 调试控制台。**
**48 个设备端 verb + 14 条主机侧命令全部真机验证。桌面抽搐已定位到「90 MiB BO 每帧重新分配」，**
**缓存已从 64/16 MiB 提到 512/128 MiB，效果明显但未根治。**

| 项 | 值 |
|---|---|
| **当前跑的包** | **`hold/kb-101-cache`**（kernel `a7cb66a649979df98212`）|
| **刚构建完待打包** | `kb-102`（kernel **`ee3ddd8bc7fee5ab497e`**，加了 reject/evict 导出）|
| **最强基线 tag** | **`baseline-kb99-fivefeatures`**（commit `8c706b4`）|
| 模块仓库 | `D:/_nabu-usb-cdc`，HEAD `8c706b4`（已 push）|
| 设备端 verb | **48**（枚举去重，排除 `INVALID`/`LAST`）|
| 主机侧命令 | **14**（`VMSTAT/MEMSTAT/POOLSTAT/MEMSTAGE/ALIASSTAT/IB/METHODS/CLIENTS/WAITOUTER/TSENS/CPSTAT/JOBSTAT/SLOW/IRQSTAT`）|
| 设备 | 跑 kb-101，已进桌面 |
| 归档 | `hold/_baseline_archive/20261123/`（**66 文件**，SHA256SUMS 全 OK）|
| 源码快照 | `hold/_source_archive/20261123-kb99/`（6 文件 + SHA256SUMS）|

**boot-args（n=50 / c=887，无 gate）**：
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
nabu-cpufreq=1 nabu-ddr-readback=1 trace=16384 trace_wrap=1 nabu-gpuclk=1
```

**boot 镜像红线**：`ead4b53135d67516` / 3,041,280 B（**永不重建**）
**boot 镜像参数**：`ANDROID!` v1，`page_size=2048`，**`ramdisk_size=0`**

---

## 2. 【三条铁律】每条都由事故换来

### 2.1 NOP18 不打，内核必卡死（6 轮实证）

```bash
python3 /e/nabu/tools/_nop18_l800.py
# 期望输出：PATCHED / nop18 1
```

**它做什么**：签名 `6100005408214439` 后面 4 字节从 `a8000037`（`cbnz`）改成
`1f2003d5`（`nop`）。

**不打会怎样**：内核卡在 `disk1s1` 挂载，日志只出 6 行 `nabu:`，COM3 在、adb 不在。
kb-73/75/82/83/89/90/91 全是这个。

**验证**：
```bash
python3 -c "d=open('kernel.macho','rb').read();i=d.find(bytes.fromhex('6100005408214439'));print(d[i+8:i+12].hex())"
# 期望 1f2003d5
```

### 2.2 改用 mcopy 换 kernel，必卡死（4 轮实证）

**`mcopy -o -i payload.img kernel.macho ::/EFI/UniXNU/Payloads/kernel.macho` 会重排簇链：**

| | 簇链 |
|---|---|
| 能启动（kb-56..kb-99）| `<39151-61431> <67551-97914> <90-8102> <8104-8827>` |
| mcopy 之后（kb-82、kb-101 首版）| `<8829-61431> <67551-76329>` ← **卡死** |

**正确做法**：`payload_swap_kernel.py` 原地写：
```bash
python3 -I /mnt/d/_nabu-usb-cdc/tools/payload_swap_kernel.py <源payload> <新kernel> <输出payload>
```
它按原簇链把字节写进去，目录项/FAT/布局一个字节不动。

**验证**：
```bash
python3 -I /mnt/e/nabu/tools/payload_layout_check.py <payload.img>
# 期望 LAYOUT OK
```

### 2.3 用 `unixnu payload` 全量重生，会丢三个文件

**`unixnu payload` 只带 `--data-ramdisk` / `--trust-cache` / `--gpu-addon` 显式给的附属文件。**
不传就**丢掉**：
```
kernel.macho.trustcache    61,456 B
scratch-data.img          106,672 B   (DBRDISK1 格式，不是 Android ramdisk)
kernel.macho.gpuaddon  12,317,544 B
```

kb-100 就是这么坏的（payload 只剩 kernel + adt）。

**正确做法**：payload 永远从上一个能启动的包 `cp` 复制，只换 kernel（原地写）。

---

## 3. 构建内核（逐条命令）

### 3.1 前置：构建树是符号链接，没有 bind mount

```bash
ls -ld /root/merge/out-v6/nabu /root/merge/out-v6/feeds
# 必须都是 symlink：
#   out-v6/nabu  -> /root/merge/l738-full/out/nabu
#   out-v6/feeds -> /root/merge/l738-full/out/feeds
mount | grep -c out-v6     # 必须是 0
```

**历史**：以前用 `mount --bind` 挂这三处，从不卸载，**累积到 91 层**。
`out-v6/nabu` 底层是 **11 月 5 日的旧快照**，mount 一失效构建就读旧树。
现在改成符号链接，**不需要任何 mount**。

### 3.2 一条命令构建

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash /mnt/e/nabu/tmp/_build_inner.sh <tag>
```

**脚本做什么**（`/mnt/e/nabu/tmp/_build_inner.sh`，副本在 `/mnt/e/nabu/tools/`）：
1. 校验三个路径是符号链接、目标正确
2. 校验 `mount | grep -c out-v6` == 0
3. 校验 `/opt/unixnu-linux/llvm/bin/clang-24` 能跑
4. `rm -f /tmp/build_<tag>.log`
5. 删掉 tree/build 下所有 `compile-command.json`
6. `cd /root/fresh/UniXNU-latest && python3 -I unixnu build kernel --out /root/merge/out-v6 -D /root/merge/l738-device --kernelcache .../kernelcache.decompressed -j 4`
7. 打印 rc / error 数 / sha256

**耗时约 3–5 分钟。必须后台跑**（`run_in_background: true`）。

### 3.3 轮询（10 秒一轮，绝不用 sleep 30/48/60）

```bash
sleep 10; MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'grep -o "sha256 [0-9a-f]\{16\}" /tmp/build_<tag>.log 2>/dev/null | tail -1; echo "running: $(pgrep -f "unixnu build kernel" >/dev/null && echo YES || echo NO)"'
```

**成功标志**：出现 `sha256 <64hex>`，且 `grep -cE " error:" == 0`。

### 3.4 三个真源（改文件前必须确认）

| 东西 | **真源** | 陷阱 |
|---|---|---|
| **生成器** `nabu-panic-log.py` | **`/root/merge/l738-device/scripts/`**（`ctx.device`）| `out-v6` 那份永不刷新；`/root/fresh` 那份构建看不见 |
| **UFS 驱动** | `l738-device/drivers/NativeUfs` | 经 `tree/drivers/ShengUfs` 符号链接 |
| **GPU 驱动** | **`UniXNU-nabu/drivers/NabuAdreno`** | `build-nabu-adreno-step1.py:30-34` 回退路径；`tree/drivers/NabuAdreno` **不存在** |

**`tree/drivers/NabuAdreno` 现在是符号链接** → `/root/merge/UniXNU-nabu/drivers/NabuAdreno`。
**改完要 `cp` 同步到 `l738-device/drivers/NabuAdreno`**（两份都要，虽然构建只读一份）。

**生成器版本号是三处**，必须同步 +1：
```python
LOG_VERSION = 42
LOG_BLOCK = r'''/* nabu-log-version: 42 */'''
marker = '/* nabu-log-version: %d */' % LOG_VERSION
```

**绝对不能删 `.o`** —— XNU 编译前 corecrypto 用 `nm` 扫所有 `.o`，缺一个整轮失败。
**强制重编用 `touch <源文件>`。**

---

## 4. 打包（逐条命令）

### 4.1 从能启动的包复制 payload，只换 kernel

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c '
TAG=<新包名>
BASE=/mnt/e/nabu/hold/<上一个能启动的包>     # 例如 kb-99-full 或 kb-101-cache
H=/mnt/e/nabu/hold/$TAG
K=/root/merge/l738-full/out/nabu/ipados-17.7.11-21H461/nabu/kernel/kernel.macho
rm -rf "$H"; mkdir -p "$H"
python3 -I /mnt/d/_nabu-usb-cdc/tools/payload_swap_kernel.py "$BASE/payload.img" "$K" "$H/payload.img"
cp -f "$K" "$H/kernel.macho"
cp -f "$BASE"/{boot-merge-r3.img,gpu-addon.bin,owner-trust.bundle,kernel.macho.adt} "$H"/
'
```

### 4.2 必须核对的五项

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c '
H=/mnt/e/nabu/hold/<TAG>
echo "== 五个文件都在? =="
mdir -i "$H/payload.img" ::/EFI/UniXNU/Payloads/ 2>/dev/null | grep -E "macho|scratch|trust|gpuaddon|adt"
echo "== 三条 sha =="
rm -f /tmp/v.macho
mcopy -o -i "$H/payload.img" ::/EFI/UniXNU/Payloads/kernel.macho /tmp/v.macho
sha256sum /tmp/v.macho "$H/kernel.macho" | cut -c1-20
echo "== NOP18 =="
python3 -c "d=open(\"/tmp/v.macho\",\"rb\").read();i=d.find(bytes.fromhex(\"6100005408214439\"));print(d[i+8:i+12].hex())"
echo "== boot 红线 =="
sha256sum "$H/boot-merge-r3.img" | cut -c1-16
echo "== 布局 =="
python3 -I /mnt/e/nabu/tools/payload_layout_check.py "$H/payload.img"
echo "== args =="
python3 -I /mnt/e/nabu/tmp/args_count.py "$H/payload.img" | head -2
'
```

**五项全部通过才能刷。**

### 4.3 打包脚本（可选）

- `/mnt/e/nabu/tmp/pack_round.sh <tag> [<base>]` —— 带 NOP18 门 + 布局门，
  但它内部用 `payload_swap_kernel.py`，**要求 BASE 的 payload 已经完整**
- `/mnt/e/nabu/tmp/pack_kb101.sh` —— kb-101 那轮的完整脚本，可作模板

**⚠️ `pack_kb56.sh` 的 `BASE` 指向 kb-50，args 是旧的 `n=48 c=862`。不要直接用。**

---

## 5. 上板（逐条命令）

### 5.1 刷前检查

```bash
adb devices
# 必须 f91fa664 recovery

MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames()"
# 必须只有 COM1（COM3 在说明内核还在跑，先复位）

MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "(Get-Process python -ErrorAction SilentlyContinue | Measure-Object).Count"
# 必须是 0（有残留控制台会占端口）
```

### 5.2 先起控制台（必须早于刷机）

```bash
cd /d/_nabu-usb-cdc && rm -f /e/nabu/logs/<tag>-cmd.txt && \
nohup python -u tools/nabu_console.py --tag <tag> --out E:/nabu/logs \
  --wait 400 --deadline 2400 --stall-exit 0 \
  --kernel /e/nabu/hold/<包名>/kernel.macho > E:/nabu/logs/<tag>-stdout.log 2>&1 &
sleep 6; tail -2 /e/nabu/logs/<tag>-stdout.log
```

**参数含义**：`--wait 400` 等 CDC 最多 400 秒；`--deadline 2400` 总时限 40 分钟；
`--stall-exit 0` 关掉「无输出就退出」（保存现场）。

### 5.3 进 bootloader

```bash
cd /e/nabu && adb shell 'dd of=/dev/block/by-name/misc bs=4096 count=1' \
  < /e/nabu/dwc3/build/misc-bootrecovery.bin 2>&1 | tail -1
adb reboot bootloader
for i in $(seq 1 18); do
  f=$(fastboot devices 2>/dev/null | awk 'NF>=2{print $1}' | head -1)
  [ -n "$f" ] && { echo "fb=$f"; break; }
  sleep 5
done
```

**⚠️ 绝不能刷 `boot_a`** —— 会清掉 misc 的 boot-recovery 标记，导致循环回同一内核。

### 5.4 刷 + 启动

```bash
cd /e/nabu && \
fastboot flash cust /e/nabu/hold/<包名>/payload.img 2>&1 | tail -1 && \
fastboot flash misc /e/nabu/dwc3/build/misc-bootrecovery.bin 2>&1 | tail -1 && \
fastboot boot /e/nabu/hold/<包名>/boot-merge-r3.img 2>&1 | tail -1
```

**顺序必须**：刷 cust → **re-arm misc** → boot。

### 5.5 等 CDC（10 秒轮询）

```bash
sleep 12; tail -2 /e/nabu/logs/<tag>-stdout.log
# 基线 38–60 秒出现 >>> CDC FOUND / CDC OPEN OK
```

### 5.6 第一件事：发 hello 验证 OUT 方向

```bash
cd /e/nabu/logs && printf 'hello\n' >> <tag>-cmd.txt && sleep 10 && tail -3 <tag>-ctl.txt
# 必须看到 RX SXNU1R ... HELLO OK session=...
```

**`CDC OPEN OK` 不能证明可用** —— 第二次 open 同一 boot 时，open 成功、日志还在流、
但 OUT 方向已死（kb-71 实证）。

### 5.7 通过控制台复位（不用手动断电）

```bash
cd /e/nabu/logs && printf 'reboot\n' >> <tag>-cmd.txt && sleep 15
# 期望 RX ... REBOOT OK state=queued，随后 adb 出现 f91fa664 recovery
```

**这条在 kb-94 验证有效。** 如果 HELLO 都没回复，说明控制通道死了，只能手动断电。

### 5.8 收尾

```bash
printf 'reboot\n' >> /e/nabu/logs/<tag>-cmd.txt
```

---

## 6. 控制台使用（唯一工具）

### 6.1 发命令：只追加，绝不截断

```bash
cd /e/nabu/logs && printf '<命令>\n' >> <tag>-cmd.txt
```

**⚠️ 绝不能 `: > <tag>-cmd.txt` 或用 `>` 覆盖。** 控制台按**字节偏移**（`cmd_pos`）追踪，
截断后 `size > cmd_pos` 不成立，**新命令被静默跳过**（实测：`read 0x… 4` 被读成 `4`）。

**要重来就换 tag。**

### 6.2 读回复

```bash
grep -a "<VERB>" /e/nabu/logs/<tag>-ctl.txt | tail -3
# 或看全部
tail -20 /e/nabu/logs/<tag>-ctl.txt
```

### 6.3 三个文件

| 文件 | 内容 |
|---|---|
| `<tag>-com.bin` | 原始串口字节（日志 + 回复混在一起）|
| `<tag>-ctl.txt` | 控制台的 TX/RX 记录（人类可读）|
| `<tag>-cmd.txt` | 你写入的命令 |
| `<tag>-events.jsonl` | 每 10 秒的字节统计 |

### 6.4 停控制台

```bash
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "Get-Process python -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id"
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "Stop-Process -Id <PID> -Force"
```

**⚠️ PowerShell 要加 `MSYS_NO_PATHCONV=1`**，否则 Git Bash 会把 `-Id` 后面的值当路径转换。

### 6.5 14 条主机侧命令

```
VMSTAT MEMSTAT POOLSTAT MEMSTAGE ALIASSTAT IB METHODS CLIENTS WAITOUTER
TSENS CPSTAT JOBSTAT SLOW IRQSTAT
```

**全部读已导出的全局数组，零内核改动**（`READ` + 主机侧解析）。

### 6.6 KDP 进出（Debugger() 场景）

```
kdp enter 6363636363636363     # 预置 8 个 'c'，进 + 能出
```

**判据**：`DEBUGGER OK state=entering queued=8 pending=8 ovf=0`
然后日志出现 `Continuing...`，紧接着内核继续跑。

**2 个 `c` 不够**（kb-93 实证）。**事后补发无效**（`serial_rx_drain` 的 `acquire()` 拒绝非 master CPU）。

---

## 7. 桌面抽搐：排查进度

### 7.1 已确认的机制

**`SLOW` 记录的 13 条慢操作全是 `source=2`（UserOwned）、`cache_result=2`（Miss）：**

```
seq=5 allocation elapsed_us=128331 bytes=94666752 spans=23111   ← 90 MiB / 128 ms
seq=7 allocation elapsed_us= 67588 bytes=67108864 spans=16384   ← 64 MiB /  68 ms
```

**慢在 `IOBufferMemoryDescriptor::inTaskWithPhysicalMask`**（`NativeAdrenoMemory.cpp:630-631`），
分配 16384/23111 个物理页并建立 span 表。

**GPU 本身只要 15–20 ms**（JOBSTAT：`accepted_us → completed_us` 差 15–20 ms）。
**所以 130 ms 里 110 ms 是分配。**

### 7.2 kb-101（缓存 512/128 MiB）的效果

| 指标 | kb-99（64/16 MiB）| **kb-101（512/128 MiB）** |
|---|---|---|
| 缓存对象 | 6 | **18** |
| 缓存字节 | 610 KB | **64 MB** |
| 峰值 | 610 KB | **384 MB** |
| **`nswap`** | **5** | **399** |
| **进桌面** | **427 s** | **300 s** |
| hits / misses | 25 / 3667 | 68 / 1985 |

**明显改善但抽搐仍在。**

### 7.3 l772 的历史结论（重要，别再踩）

**`board-report-l772-cache-result.md`**：把缓存提到 64/256 MiB 是**负收益**
（≥500ms 占比 61% → 81.7%），因为它只让**小于上限的对象**进了缓存，
90 MiB 的仍被 `Oversized` 拒，只增加了驱逐开销。

**kb-101 的区别**：`BackingCacheObjectBytes` 提到 **128 MiB**，**超过了 90 MiB 的最大对象**。
这是 l772 没做到的。

### 7.4 待确认的下一个疑点

**缓存里已有 18 个对象、64 MB、零驱逐，但 64 MiB 的分配仍 miss。**

`takeBacking()` 要求 `candidate.bytes == bytes`（**精确匹配**）。
**如果 backboardd 每帧的 BO 大小在变，就永远对不上。**

**kb-102 已加导出 `_nabu_console_reject[16]` / `_nabu_console_evict[8]`**，
会给出 13 种拒绝原因（`OVERSIZED`/`NoSlot`/`no_evictable`/`imported`/`pressure`/`busy`…）的直方图。

**kb-102 已构建：kernel `ee3ddd8bc7fee5ab497e`，零 error，符号已确认存在。待 NOP18 + 打包 + 上板。**

### 7.5 其它已确认的事实

- **`reservePool()` 没有任何调用者**，`POOLSTAT pool_mib=0` —— 池从未建立
- 它需要 `kIOMemoryPhysicallyContiguous` 大块连续内存，碎片化下不可行
- **`imported_` 走 `BackingRejectImported`**，在 `cacheBacking()` 之前就拒掉（设计如此）
- **`RAMPROBE`: `total_runs=112429 / total_pages=121923`** —— 平均每 run 只有 1.08 页，**物理内存极度碎片化**

### 7.6 启动慢

kb-99 是 427 s，kb-101 是 300 s。`xpcproxy` 重试 768 次（kb-70 是 340 次）。
`bluetoothd` / `fairplayd` / `MobileAccessoryUpdater` 反复重启。

---

## 8. 踩坑清单（新增部分）

| # | 坑 |
|---|---|
| **69** | **NOP18 不打必卡死**（6 轮实证），`pack_round.sh` 有门但手动打包会漏 |
| **70** | **`mcopy` 换 kernel 必改簇链**（4 轮实证），只能用 `payload_swap_kernel.py` |
| **71** | **`unixnu payload` 全量重生会丢 trustcache/scratch-data/gpuaddon**，payload 只能从旧包复制 |
| **72** | **`CDC OPEN OK` 不代表可用**，第二次 open 同一 boot 时 OUT 方向已死，判据只能是 HELLO 有回复 |
| **73** | **`cmd.txt` 只能 `>>` 追加**，截断后命令被静默跳过 |
| **74** | **PowerShell 命令要加 `MSYS_NO_PATHCONV=1`**，否则 `-Id <数字>` 被当路径 |
| **75** | **`READ` 的 `data=` 最多 192 字节**（`SXNU_CONTROL_REPLY_BYTES=512` 减协议开销），`len=` 是请求长度；超过要分块 |
| **76** | **`boot-merge-r3.img` 的 `ramdisk_size=0`**，不受 Android ramdisk 32–64 MB 限制（那是另一条路的坑）|
| **77** | **`scratch-data.img` 是 `DBRDISK1` 格式**（104 KB 稀疏，声明 1 MiB），不是 Android ramdisk |
| **78** | **`devicetree.py` 校验 `owned_memory_size` 范围 256 MiB–8 GiB**，改它必须重生 payload，重生就踩 #71 |
| **79** | **改 `owned_memory_size` 只换 ADT 也不行** —— ADT 大小会变（211568→211412），位置会移到 `<8829-8932>`，紧贴 kernel 的 `<8104-8827>` |
| **80** | **`extern "C"` 不能写在函数体内**（`expected unqualified-id`），必须文件作用域 |
| **81** | **`extern "C" uint64_t a[8];` 是声明不是定义**，定义要写进 `extern "C" { uint64_t a[8]; }` 块，否则链接期 `Undefined symbols` |
| **82** | **`l772` 的「加缓存无效」结论有前提** —— 它只提到 64 MiB，没超过 90 MiB 的对象；提到 128 MiB 后行为不同（kb-101 实证）|

---

## 9. 下一步（按优先级）

| 优先 | 项 | 怎么做 |
|---|---|---|
| **1** | **kb-102 上板**，看 `insert_reject` 直方图 | NOP18 → 打包 → 刷 → `slow 20` |
| **2** | 如果 reject 是 `OVERSIZED` | 说明大小判断有别的闸门，继续查 |
| **2** | 如果 reject 是 `NoSlot`/`no_evictable` | 提高 `BackingCacheEntries`（64→128）|
| **2** | 如果 reject 全 0 但分配仍 miss | **大小不匹配**，需记录每次 allocate 的 `bytes` 看是否有随机尺寸 |
| **3** | 启动慢 | 查 `xpcproxy` 为什么 768 次重试 |
| — | 8 GiB 内存 | 需完整重生 payload（带全三文件参数）+ 单独验证 ADT 大小变化能否被 bootloader 接受 |
| — | panic→KDP 退出 | 不改 XNU 不可行（T39 已论证）|

---

## 10. 快速开始下一轮（复制粘贴）

```bash
# 1. 现状
adb devices
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames()"
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "(Get-Process python -ErrorAction SilentlyContinue | Measure-Object).Count"

# 2. 如果设备卡着：先试控制台复位，不行再让用户断电
#    控制台在跑时：printf 'reboot\n' >> /e/nabu/logs/<tag>-cmd.txt

# 3. kb-102 打包
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'python3 /mnt/e/nabu/tools/_nop18_l800.py'
# 然后按 §4.1 用 payload_swap_kernel.py 从 kb-101-cache 打包

# 4. 起控制台（§5.2）+ 刷机（§5.3–5.4）+ 等 CDC（§5.5）
# 5. 发 hello（§5.6）
# 6. 查 reject：printf 'slow 20\n' >> <tag>-cmd.txt
```

---

## 11. 文档与工具索引

### 记忆与纪律
| 文档 | 内容 |
|---|---|
| **本文** | 最新，自包含 |
| `MEMORY-MASTER-20261123-1835.md` | 上一份（kb-70 时代，725 行）|
| `DISCIPLINE-20261123-anti-pollution.md` | 防污染纪律 434 行 |
| `BASELINE-kb99-20261123.md` | kb-99 基线规格 |

### 本轮关键结果
| 文档 | 内容 |
|---|---|
| `RESULT-kb99-five-features.md` | TSENS/JOBSTAT/SLOW/CPSTAT/IRQSTAT 全部验证 |
| `RESULT-kb94-kdp-exit-closed.md` | KDP 进出闭合（8 个 `c`）|
| `RESULT-kb74-writepa-positive.md` | WRITEPA 值变化验证 |
| `RESULT-kb78-sysregw-positive.md` | SYSREGW 正向（gate 变量直写）|
| `board-report-l772-cache-result.md` | **加缓存负收益的历史实证（必读，防止重复踩）** |
| `KDP-EXIT-TWO-MODES-20261123.md` | 两种 Debug 的区分 |
| `ROOTCAUSE-20261123-boot-hang-open.md` | 启动卡死的排查记录 |

### 工具
| 工具 | 用途 |
|---|---|
| **`D:/_nabu-usb-cdc/tools/nabu_console.py`** | **唯一控制台**（48 verb + 14 主机命令 + KDP）|
| **`D:/_nabu-usb-cdc/tools/payload_swap_kernel.py`** | **原地换 kernel（保持簇链）** |
| **`E:/nabu/tools/payload_layout_check.py`** | **校验簇链是否可启动** |
| `E:/nabu/tmp/_build_inner.sh` | 构建入口（零 mount）|
| `E:/nabu/tmp/pack_round.sh` | 打包（带 NOP18 门 + 布局门）|
| `E:/nabu/tools/_nop18_l800.py` | **NOP18 补丁（必须跑）** |
| `E:/nabu/tmp/args_count.py` | 读 payload 的 args 数 |
| `E:/nabu/tools/kernel_edit_guard.py` | `snapshot` / `object` |

---

## 12. 一句话交接

> **当前跑 kb-101（`a7cb66a649979df98212`），缓存 512/128 MiB，进桌面 300 s、`nswap=399`，
> 比 kb-99 的 427 s / `nswap=5` 明显改善，但抽搐仍在。**
>
> **kb-102（`ee3ddd8bc7fee5ab497e`）已构建完，加了 `_nabu_console_reject` / `_nabu_console_evict`
> 导出，待 NOP18 + 打包 + 上板，用来查「为什么 64 MiB 对象进了缓存却仍 miss」。**
>
> **三条铁律（§2）：不打 NOP18 必卡死、不用 mcopy 换 kernel、不重生 payload。**
> **上板前五项核对（§4.2）全过才能刷。**
> **一个 boot 只能 open 一次 CDC（§5.6），判据只能是 HELLO 有回复。**
