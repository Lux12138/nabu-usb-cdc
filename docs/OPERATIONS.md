# 操作手册（逐条命令，2026-11-23 22:35）

> 与 `MEMORY-MASTER-20261123-2235.md` 配套。**这份只有命令，没有解释。**
> 所有命令都是本轮实际跑过的。

---

## 0. 环境前置（每个新 shell 都要）

```bash
# Git Bash 里调 WSL，一律这样写（防止变量被吞 / 路径被转换）
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c '<命令>'
MSYS_NO_PATHCONV=1 wsl.exe --exec bash /mnt/e/nabu/tmp/<脚本>.sh

# PowerShell 也要加（否则 -Id 后面的数字被当路径）
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command '<命令>'
```

**永不使用**：
- `wsl bash -c "... $var ..."`（变量被吞）
- `wsl bash -lc`（login shell 会卡住）
- `grep` 125 MB 的 `kernel.macho`（用 `llvm-nm`）
- `grep` 136 MB 的 `logs/kernel.log`

---

## 1. 看现状

```bash
# 设备
adb devices
# 期望：f91fa664  recovery

# 串口
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "[System.IO.Ports.SerialPort]::GetPortNames()"
# recovery：只有 COM1
# 内核在跑：COM1 + COM3

# 残留控制台
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "(Get-Process python -ErrorAction SilentlyContinue | Measure-Object).Count"
# 必须 0

# 构建树健康
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'ls -ld /root/merge/out-v6/nabu /root/merge/out-v6/feeds; mount | grep -c out-v6'
# 两个都是 symlink；mount 数必须 0
```

---

## 2. 构建内核

```bash
# 后台跑，约 3-5 分钟
MSYS_NO_PATHCONV=1 wsl.exe --exec bash /mnt/e/nabu/tmp/_build_inner.sh <tag>
```

轮询（10 秒一轮）：

```bash
sleep 10; MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'grep -o "sha256 [0-9a-f]\{16\}" /tmp/build_<tag>.log 2>/dev/null | tail -1; echo "running: $(pgrep -f "unixnu build kernel" >/dev/null && echo YES || echo NO)"'
```

成功判据：

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'grep -cE " error:" /tmp/build_<tag>.log; grep -o "sha256 [0-9a-f]\{64\}" /tmp/build_<tag>.log | tail -1'
# 期望：0 和 sha256 ...
```

查新符号是否进了内核：

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c '/usr/bin/llvm-nm /root/merge/l738-full/out/nabu/ipados-17.7.11-21H461/nabu/kernel/kernel.macho 2>/dev/null | grep <符号名>'
```

---

## 3. NOP18（必须）

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'python3 /mnt/e/nabu/tools/_nop18_l800.py'
# 期望：PATCHED / nop18 1 / sha16 ...
```

验证：

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c 'python3 -c "
d=open(\"/root/merge/l738-full/out/nabu/ipados-17.7.11-21H461/nabu/kernel/kernel.macho\",\"rb\").read()
i=d.find(bytes.fromhex(\"6100005408214439\"))
print(d[i+8:i+12].hex())"'
# 期望 1f2003d5
```

---

## 4. 打包

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c '
TAG=<新包名>
BASE=/mnt/e/nabu/hold/<上一个能启动的包>
H=/mnt/e/nabu/hold/$TAG
K=/root/merge/l738-full/out/nabu/ipados-17.7.11-21H461/nabu/kernel/kernel.macho
rm -rf "$H"; mkdir -p "$H"
python3 -I /mnt/d/_nabu-usb-cdc/tools/payload_swap_kernel.py "$BASE/payload.img" "$K" "$H/payload.img"
cp -f "$K" "$H/kernel.macho"
cp -f "$BASE"/{boot-merge-r3.img,gpu-addon.bin,owner-trust.bundle,kernel.macho.adt} "$H"/
'
```

五项核对（**全过才能刷**）：

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c '
H=/mnt/e/nabu/hold/<TAG>
mdir -i "$H/payload.img" ::/EFI/UniXNU/Payloads/ 2>/dev/null | grep -E "macho|scratch|trust|gpuaddon|adt"
rm -f /tmp/v.macho
mcopy -o -i "$H/payload.img" ::/EFI/UniXNU/Payloads/kernel.macho /tmp/v.macho
sha256sum /tmp/v.macho "$H/kernel.macho" | cut -c1-20
python3 -c "d=open(\"/tmp/v.macho\",\"rb\").read();i=d.find(bytes.fromhex(\"6100005408214439\"));print(d[i+8:i+12].hex())"
sha256sum "$H/boot-merge-r3.img" | cut -c1-16
python3 -I /mnt/e/nabu/tools/payload_layout_check.py "$H/payload.img"
python3 -I /mnt/e/nabu/tmp/args_count.py "$H/payload.img" | head -2
'
```

期望：
- 五个文件：`trustcache` / `scratch-data.img` / `kernel.macho` / `gpuaddon` / `adt`
- 两条 sha 相同
- `1f2003d5`
- `ead4b53135d67516`
- `LAYOUT OK`
- `n=50 c=887`

---

## 5. 上板

```bash
# 5.1 起控制台（必须早于刷机）
cd /d/_nabu-usb-cdc && rm -f /e/nabu/logs/<tag>-cmd.txt && \
nohup python -u tools/nabu_console.py --tag <tag> --out E:/nabu/logs \
  --wait 400 --deadline 2400 --stall-exit 0 \
  --kernel /e/nabu/hold/<包名>/kernel.macho > E:/nabu/logs/<tag>-stdout.log 2>&1 &
sleep 6; tail -2 /e/nabu/logs/<tag>-stdout.log

# 5.2 进 bootloader
cd /e/nabu && adb shell 'dd of=/dev/block/by-name/misc bs=4096 count=1' \
  < /e/nabu/dwc3/build/misc-bootrecovery.bin 2>&1 | tail -1
adb reboot bootloader
for i in $(seq 1 18); do
  f=$(fastboot devices 2>/dev/null | awk 'NF>=2{print $1}' | head -1)
  [ -n "$f" ] && { echo "fb=$f"; break; }
  sleep 5
done

# 5.3 刷 + 启动
cd /e/nabu && \
fastboot flash cust /e/nabu/hold/<包名>/payload.img 2>&1 | tail -1 && \
fastboot flash misc /e/nabu/dwc3/build/misc-bootrecovery.bin 2>&1 | tail -1 && \
fastboot boot /e/nabu/hold/<包名>/boot-merge-r3.img 2>&1 | tail -1

# 5.4 等 CDC（10 秒一轮）
sleep 12; tail -2 /e/nabu/logs/<tag>-stdout.log
```

---

## 6. 验证与测试

```bash
# 6.1 第一件事：验证 OUT 方向
cd /e/nabu/logs && printf 'hello\n' >> <tag>-cmd.txt && sleep 10 && tail -3 <tag>-ctl.txt

# 6.2 批量命令
cd /e/nabu/logs && printf 'tsens\ncpstat\nslow 20\nmemstat\njobstat 8\n' >> <tag>-cmd.txt
sleep 30
grep -aE "TSENS|CPSTAT|SLOW|MEMSTAT|JOB" <tag>-ctl.txt | tail -20

# 6.3 看回复
grep -a "<VERB>" /e/nabu/logs/<tag>-ctl.txt | tail -3
tail -20 /e/nabu/logs/<tag>-ctl.txt
```

---

## 7. 复位

```bash
# 7.1 通过控制台（推荐，不用断电）
cd /e/nabu/logs && printf 'reboot\n' >> <tag>-cmd.txt && sleep 15
adb devices    # 期望 f91fa664 recovery

# 7.2 如果控制台已死（HELLO 无回复）
# 只能让用户手动断电
```

---

## 8. 停控制台

```bash
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "Get-Process python -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Id"
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "Stop-Process -Id <PID> -Force"
sleep 3
MSYS_NO_PATHCONV=1 powershell -NoProfile -Command "(Get-Process python -ErrorAction SilentlyContinue | Measure-Object).Count"
# 期望 0
```

---

## 9. 查符号地址（换内核后必做）

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c '/usr/bin/llvm-nm /e/nabu/hold/<包名>/kernel.macho 2>/dev/null | grep -E "_符号1|_符号2"'
```

本轮常用：

```bash
# 关键变量
_nabu_console_diag_totals      # SLOW 的 totals[8]
_nabu_console_slow_flat        # SLOW 明细 [64][12]
_nabu_console_slow_count
_nabu_console_reject           # 拒绝原因直方图 [16]
_nabu_console_evict            # 驱逐原因 [8]
_nabu_cpufreq_tsens_tenths     # 温度
_nabu_console_job_samples      # JOBSTAT [64][14]
_native_adreno_a6xx_stats      # CPSTAT [24]
_native_adreno_backing_cache_stats  # MEMSTAT
_native_adreno_pool_stats      # POOLSTAT
_nabu_alias_stats              # ALIASSTAT
_native_adreno_method_stats    # METHODS
_native_adreno_client_stats    # CLIENTS
_native_adreno_wait_outer      # WAITOUTER
_native_adreno_ib_log          # IB
_qud_sysreg_write_enabled      # SYSREGW 的 gate（写 1 打开）
```

---

## 10. 读内存/寄存器

```bash
# 内核虚拟地址
printf 'read 0x<va> <len>\n' >> <tag>-cmd.txt

# 物理地址 / MMIO（有白名单）
printf 'readpa 0x<pa> <len>\n' >> <tag>-cmd.txt

# 系统寄存器
printf 'sysreg TCR_EL1\n' >> <tag>-cmd.txt
printf 'sysregw CNTKCTL_EL1 0x0f\n' >> <tag>-cmd.txt    # 需要 gate

# 运行时开 SYSREGW gate（不用刷机）
printf 'write 0xfffffff00c0f5a70 01000000\n' >> <tag>-cmd.txt
```

---

## 11. UFS 日志读回

```bash
printf 'readufs header\n' >> <tag>-cmd.txt        # 周期槽头
printf 'readufs panic\n' >> <tag>-cmd.txt         # panic 槽头
printf 'readufs 8166@4032\n' >> <tag>-cmd.txt     # panic 文本
```

---

## 12. KDP

```bash
# 进 + 能出（预置 8 个 'c'）
printf 'kdp enter 6363636363636363\n' >> <tag>-cmd.txt
# 判据：DEBUGGER OK ... queued=8 pending=8 ovf=0
#       然后日志出现 Continuing...
```

---

## 13. 归档

```bash
# 产物归档
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c '
SRC=/e/nabu/hold/<包名>
DST=/e/nabu/hold/_baseline_archive/20261123/<归档名>
mkdir -p "$DST"; cp -f "$SRC"/* "$DST"/
cd /e/nabu/hold/_baseline_archive/20261123
sha256sum <归档名>/* >> SHA256SUMS
sort -u SHA256SUMS -o SHA256SUMS
sha256sum -c SHA256SUMS 2>&1 | grep -v ": OK$" | head -5
'

# git tag
cd /d/_nabu-usb-cdc && git tag -a baseline-<tag> -m "<说明>" && \
GIT_TERMINAL_PROMPT=0 git push origin baseline-<tag>
```

---

## 14. 常用的"不要做"

| 不要 | 为什么 |
|---|---|
| `mcopy` 换 `payload.img` 里的 kernel | 重排簇链 → 启动卡死（4 轮实证）|
| `unixnu payload` 全量重生 | 丢 trustcache/scratch-data/gpuaddon |
| 截断 `<tag>-cmd.txt` | 命令被静默跳过 |
| `sleep 30` / `sleep 48` / `sleep 60` | 用 10 秒轮询 |
| 刷 `boot_a` | 清掉 misc 标记，循环回同一内核 |
| 同时开两个串口工具 | 单句柄，抢 COM3 会 `PermissionError(13)` |
| 删 `.o` 文件 | corecrypto 的 `nm` 扫描会失败 |
| `READPA` 探测未知地址 | 卡死 worker（kb-72 实证）|
| 写 `TPIDR_EL1` | 它是 `current_thread()`，写随机值内核静默卡死 |
| `grep` 125 MB kernel / 136 MB kernel.log | 用 `llvm-nm` / `tail` |
