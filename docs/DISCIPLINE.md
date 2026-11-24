# 【最高优先级】防污染纪律 —— 内核/驱动改动前置检查

> 2026-11-23 立。**每一条都由真机事故换来。违反任何一条 = 白烧一轮构建 + 一次上板。**
> 本文是 `MEMORY-MASTER-20261123-0915.md` §1.1（上板纪律）的**构建侧补充**，
> 两份合并构成完整纪律。

---

## 0. 一句话

**改之前先确认「这条链上哪一环才是真正生效的」。**
我连续三次改错文件（`pe_init.c`、`serial_general.c`、生成器副本），
每次都是「改完编译、编译通过、上板发现改动不在」。

---

## 1. 判据一：这个文件是**源**还是**产物**？

### 1.1 检查命令（改任何内核文件之前，必跑）

```bash
X=/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree
grep -l "$(basename <要改的文件>)" $X/scripts/*.py
```

**有任何命中 ⇒ 它是产物，改它无效。**

### 1.2 已知的「产物」清单（由 `scripts/nabu-panic-log.py` 生成）

| 产物 | 生成器 | 说明 |
|---|---|---|
| `osfmk/console/serial_general.c` | `nabu-panic-log.py` | 整个 log 块（`LOG_BLOCK`）被替换 |
| `osfmk/kern/debug.c` | `nabu-panic-log.py` | panic restart + 字符串捕获被替换 |
| `pexpert/arm/pe_init.c` | `nabu-panic-log.py` | marker + 保留上次 panic |
| `osfmk/arm/sleh.c` | `nabu-panic-log.py` | iabort dump |

**⇒ 这四个文件一律改生成器，不改文件本身。**

### 1.3 生成器的版本机制（三重，必须同步）

`scripts/nabu-panic-log.py` 里有 **三处独立**的版本：

```python
LOG_VERSION = 40                              # ① 判断用
LOG_BLOCK = r'''
/* nabu-log-version: 40 */                     # ② 写进产物的 marker
'''
marker = '/* nabu-log-version: %d */' % LOG_VERSION   # ③ 查找用
```

**三处必须一致。** 不一致的后果：
- ①③ 一致但 ② 不同 ⇒ 每次构建都重复 splice（内容幂等但 mtime 乱跳）
- ② 落后 ⇒ `marker in src` 判断失败 ⇒ 重复 splice
- **改了 `LOG_BLOCK` 内容但不 bump 版本 ⇒ 生成器 skip，改动永不生效**

**⇒ 改 `LOG_BLOCK` 必须同时把 ①②③ 一起 +1。**

---

## 2. 判据二：这个路径是**唯一**的吗？

### 2.1 同一个文件可能有 4 份

已验证的 inode 对照（2026-11-23）：

| 路径 | inode | 被构建读？ |
|---|---|---|
| `/root/merge/out-v6/nabu/.../tree/scripts/nabu-panic-log.py` | **237606** | ✅ |
| `/root/merge/l738-full/out/nabu/.../tree/scripts/...` | **237606** | ✅ 同一文件（bind mount）|
| `/root/merge/l738-device/scripts/...` | 4404292 | ❌ |
| `/root/merge/UniXNU-nabu/scripts/...` | 4245996 | ❌ |

### 2.2 检查命令

```bash
stat -c '%i  %n' <路径1> <路径2> ...
```

**inode 相同 = 同一文件；不同 = 独立副本，只有一份被读。**

### 2.2b 【已确认·第二次修正】生成器真源是 **ctx.device**

**第一次的结论（「改 `/root/fresh` 那份」）也是错的。** 真正被执行的只有一处。

`scripts/unixnu/tree.py` 的 `assemble()`：

```python
bases = [ctx.root]                 # /root/fresh/UniXNU-latest   —— 没有 scripts/nabu-panic-log.py
if ctx.soc:  bases.append(ctx.soc.path)
bases.append(ctx.device)           # /root/merge/l738-device     —— ← 最后一个，覆盖前面所有
for base in bases:
    _copy_scripts(base / 'scripts', tree / 'scripts')
```

`_copy_scripts` 的跳过规则（这是陷阱所在）：

```python
if target.exists() and target.stat().st_size == path.stat().st_size \
        and target.stat().st_mtime >= path.stat().st_mtime:
    continue          # ← 大小相同且目标不旧，就永远不刷新
```

`out-v6` 里那份是 **11-07 的 35016 B**，和 device 源**大小完全相同、mtime 也相同**
⇒ 每次构建都判 `SKIP`，**永远不刷新**。

| 路径 | inode | 11-23 时的状态 | 角色 |
|---|---|---|---|
| **`/root/merge/l738-device/scripts/nabu-panic-log.py`** | **4404292** | LOG_VERSION 38（改前）| ✅ **真源（唯一要改的）** |
| `/root/merge/UniXNU-nabu/scripts/nabu-panic-log.py` | 4245996 | 同内容副本 | 同步副本，习惯上也一起改 |
| `/root/merge/out-v6/.../tree/scripts/nabu-panic-log.py` | 237606 | **11-07 旧版，永不刷新** | 实际被执行的（`build-xnu-4k-baseline.sh:10`）|
| `/root/fresh/UniXNU-latest/out/.../tree/scripts/` | 4202039 | 我手动改过的那份 | ❌ **根本不在 bases 里，构建完全看不见** |

**⇒ 只改 `out-v6` 那份也没用**：`unixnu tree`/`assemble` 一旦重跑就会覆盖它；
而且这次它根本没被刷新，所以「改了没生效」的表象是 mtime 停在 11-07。

### 2.2b.1 正确的改法（三步，全过再跑整轮构建）

```bash
D=/root/merge/l738-device/scripts/nabu-panic-log.py     # ← 真源，改这个
X=/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree
B=$X/scripts/nabu-panic-log.py
L=/root/merge/l738-full/out/nabu/ipados-17.7.11-21H461/nabu/tree/scripts/nabu-panic-log.py

# 第 1 步：改真源 D，然后绕过「大小+mtime」跳过规则，强制推到两个 tree 位置
cp -f "$D" "$B"
cp -f "$D" "$L"       # l738-full 是 out-v6 的 bind mount 源，两边是不同 inode
cmp -s "$D" "$B" && cmp -s "$D" "$L" || echo 'SYNC FAILED'

# 第 2 步：立刻跑生成器，验证产物（不要先跑整轮构建）
cd "$X"
python3 scripts/nabu-panic-log.py .cache/xnu-work/osfmk/console/serial_general.c \
                                   .cache/xnu-work/osfmk/kern/debug.c
grep -o 'nabu-log-version: [0-9]*' .cache/xnu-work/osfmk/console/serial_general.c
grep -c 预期符号 .cache/xnu-work/osfmk/console/serial_general.c

# 第 3 步：单编该单元，确认符号在对象里
#   用 .o.json 里的命令行（见 §5），然后：
#   llvm-nm 该对象 | grep 预期符号
```

**三步全过才跑整轮构建。** 否则整轮构建要 3 分钟才发现符号没进去。

### 2.2c 生成器里的版本号是**三处**

```python
LOG_VERSION = 39                              # ① 判断 / 引用
LOG_BLOCK = r'''
/* nabu-log-version: 31 */                     # ② 硬编码在产物里 —— 曾长期不同步！
'''
marker = '/* nabu-log-version: %d */' % LOG_VERSION   # ③ 查找
```

**② 一直停在 31，而 ① 是 38** —— 这就是为什么生成器每次都判定
「marker 不在，需要 splice」，反复重写产物。

**⇒ 三处必须同步 +1，缺一个就会出问题。**

---

### 2.2d 【驱动】真源只有 `l738-device/drivers/<name>`，挂载靠 symlink

`tree/drivers/` 在构建时由 `tree.py:assemble()` 用 `_clear_links` + `_link` 重建，
里面全是**符号链接**，指向不同地方：

| tree 里的名字 | 指向 | 说明 |
|---|---|---|
| `ShengUfs` | `/root/merge/l738-device/drivers/NativeUfs` | **驱动真源**（名字不一致！）|
| `NabuAdreno` | `/root/merge/UniXNU-nabu/drivers/NabuAdreno` | 注意是 UniXNU-nabu，不是 l738-device |
| `ArmDTPlatform` / `DarwinBoot` | `/root/fresh/UniXNU-latest/drivers/...` | 在上游仓库里 |
| `GopFramebuffer` / `ArmCpu` / `AppleKeyStoreState` | `/root/merge/l738-device/drivers/...` | |

**判据（不靠猜，靠对象的 DWARF）：**

```bash
O=<tree>/build/sheng-ufs/NativeUfs.o
/usr/bin/llvm-dwarfdump --debug-info $O | grep -E 'DW_AT_(name|comp_dir)' | head -2
# → DW_AT_name ("/root/merge/out-v6/.../tree/drivers/ShengUfs/NativeUfs.cpp")
```

**这条路径经过 `tree/drivers/ShengUfs` 这个符号链接解析后才落到真源。**
直接 `ls tree/drivers/ShengUfs` 看到的是链接本身，要 `ls -la` 才看到目标。

**⚠️ `l738-device/drivers/NativeUfs/` 里有几十个备份**（`*.pre-l487`、`*.l232.bak`、
`pre-l475/NativeUfs.cpp` 等）。**`grep -rn` 会全部命中，让人以为有多个源。**
真正的源是**不带后缀的那一个**：`NativeUfs.cpp`。

### 2.2e 【驱动第二例】GPU 驱动的真源是 `UniXNU-nabu`，不是 `l738-device`

上一节说驱动真源是 `l738-device/drivers/`。**这对 `NativeUfs` 成立，对 `NabuAdreno` 不成立。**

`scripts/build-nabu-adreno-step1.py` 的源解析：

```python
SRC = ROOT / 'drivers' / 'NabuAdreno'          # tree/drivers/NabuAdreno
if not SRC.exists():
    SRC = Path('/root/merge/UniXNU-nabu/drivers/NabuAdreno')   # ← 实际走这条
```

而 `tree/drivers/` 里**没有 `NabuAdreno` 这个符号链接**（`tree.py` 只链 `components.toml` 里
列出的组件，GPU 驱动走 `build-nabu-adreno-step1.py` 的独立路径）。所以：

| 驱动 | 真源 |
|---|---|
| `NativeUfs`（`component:ShengUfs`）| `/root/merge/l738-device/drivers/NativeUfs` |
| **`NabuAdreno`（`build-nabu-adreno-step1`）** | **`/root/merge/UniXNU-nabu/drivers/NabuAdreno`** |
| 其它 `component:*` | 看 `tree/drivers/<name>` 的符号链接目标 |

**⚠️ `l738-device/drivers/NabuAdreno` 和 `UniXNU-nabu/drivers/NabuAdreno` 内容曾经完全相同，
所以改错那份看起来"改成功了"，但构建完全看不到。**

**判据（改 GPU 驱动前必跑）：**

```bash
# 看脚本里的 SRC 解析
grep -n "SRC = \|Path('/root/merge" <tree>/scripts/build-nabu-adreno-step1.py | head
# 看对象被写到哪
find <tree>/build -name "NativeAdrenoService.o" -printf "%TY-%Tm-%Td %TH:%TM %p
"
# 两者一致才是真源
```

**⇒ 改完必须 `diff -q` 确认两份一致，或者直接改 `UniXNU-nabu` 那份。**

### 2.3 XNU 源的三份 `.cache/xnu-work`

| 路径 | 是编译输入？ |
|---|---|
| `/root/merge/l738-device/.cache/xnu-work/` | ❌ |
| `/root/merge/UniXNU-nabu/.cache/xnu-work/` | ❌ |
| **`/root/merge/out-v6/.../tree/.cache/xnu-work/`** | ✅ **唯一** |

### 2.4 权威判据（不靠记忆，靠构建自己说）

```bash
# 每个编译单元的记录里有 "file" 字段，那是它真正读的源
python3 -I - <<'PY'
import json
rec = "<build>/osfmk/DEVELOPMENT/debug.o.json"
obj, _ = json.JSONDecoder().raw_decode(open(rec).read())
print(obj["file"])        # ← 这就是真源
PY
```

**⇒ 改之前先读这个字段，改的必须是它指向的文件。**

---

## 3. 判据三：挂载是否真的生效？

`/tmp/_build_inner.sh` 用 `mount --bind` 把 `l738-full/out` 映射到 `out-v6`。
**WSL 重启后挂载会丢失**，此时两边是独立目录。

### 3.1 检查

```bash
mount | grep -c out-v6           # 应为 3（tree / feeds / state.json）
stat -c '%i' /root/merge/out-v6/.../tree/.cache/xnu-work/osfmk/kern/debug.c \
             /root/merge/l738-full/out/.../tree/.cache/xnu-work/osfmk/kern/debug.c
# inode 必须相同
```

### 3.2 挂载丢失的后果

**我改 `out-v6` 那份（当时是独立目录），构建读的也是 `out-v6`** ⇒ 看起来生效了，
**但 `l738-full`（真正的持久树）没变** ⇒ 下次挂载恢复后改动消失。

**⇒ 改完必须两边都确认（inode 相同时一次就够）。**

---

## 4. 判据四：对象真的重编了吗？

### 4.1 这是最隐蔽的一类污染

实测事故：

| 文件 | `.o` 时间 | 源时间 | 结果 |
|---|---|---|---|
| `debug.o` | **11-16**（一周前）| 11-23 | 改动从未编进去，但构建「成功」 |
| `pe_init.o` | 19:55（昨天）| 12:50 | 同上 |

### 4.2 检查工具

```bash
python E:/nabu/tools/kernel_edit_guard.py object <unit>.o.json
# 输出 PASS 才算真的重编了
```

### 4.3 绝对不能删 `.o`

**删了会破坏构建**：`archive-01-build-corecrypto-14` 在 XNU 编译**之前**
用 `nm` 扫所有已有 `.o` 的导出符号，缺一个就整轮失败：

```
error: nm: can't open file: .../debug.o (No such file or directory)
```

**⇒ 强制重编用 `touch <源文件>`，不删对象。**

### 4.4 符号级验证（最终判据）

```bash
# 改动是否真进了对象
llvm-objdump -dr <unit>.o | grep -c <预期符号>
llvm-strings <unit>.o | grep -c <预期字符串>

# 改动是否真进了内核
llvm-objdump -d --disassemble-symbols=_<函数> <kernel.macho> | grep -c <预期调用>
```

**注意两个「查不到但不是失败」的情况：**

| 情况 | 原因 | 正确查法 |
|---|---|---|
| `static` 函数查不到符号 | 被内联 | 查它的**字符串字面量**或它调用的外部符号 |
| `0x9b000000` 查不到 | 编译成 `mov w0, #-0x65000000`（负数立即数）| 换算补码，或查 `movz/movk` 序列 |

---

## 5. 完整的前置检查清单

**改任何内核/驱动文件之前，按顺序跑完 5 步：**

```bash
# ① 它是源还是产物？
X=/root/merge/out-v6/nabu/ipados-17.7.11-21H461/nabu/tree
grep -l "$(basename <file>)" $X/scripts/*.py    # 命中就要改生成器

# ② 哪份是被读的？（读构建记录的 "file" 字段）
python3 -I -c "import json;o,_=json.JSONDecoder().raw_decode(open('<build>/<unit>.o.json').read());print(o['file'])"

# ③ 挂载生效了吗？（两份 inode 必须相同）
stat -c '%i %n' <out-v6路径> <l738-full路径>

# ④ 改之前快照
python E:/nabu/tools/kernel_edit_guard.py snapshot <tag> <file>

# ⑤ 改完编译，验证对象
python E:/nabu/tools/kernel_edit_guard.py object <unit>.o.json
```

**五步全过才可以上板。**

---

## 6. 本轮三次改错的具体记录（供复盘）

| 次 | 我改的 | 应该改的 | 白烧 |
|---|---|---|---|
| 1 | `/root/merge/l738-device/.cache/xnu-work/kern/debug.c` | `out-v6/.../tree/.cache/xnu-work/kern/debug.c` | 1 编译 + 1 上板 |
| 2 | `pe_init.c`（产物）| `scripts/nabu-panic-log.py`（生成器）| 2 编译 |
| 3 | `serial_general.c`（产物）+ 错误的生成器副本 | inode `237606` 那份生成器 | 2 编译 |

**共同点：都没有先确认「哪一环生效」。**

---

## 7. 一句话纪律

> **改之前先读构建记录里的 `"file"` 字段；它是产物就去改生成器；
> 改完核对对象比源码新、符号真在对象里。五步全过才上板。**

---

## 8. 与上板纪律的关系

本文（构建侧）+ `MEMORY-MASTER-20261123-0915.md` §1.1（上板侧）= 完整纪律。

上板侧九条速查（详见原文档）：

1. 10 秒轮询，永不 sleep 30/48/60
2. 先起控制台，再刷机
3. WDT 咬回判据：COM3 消失 **且** adb 出现 recovery
4. 复位一律手动发，禁 `--arm-at`
5. 只用整合控制台，不开第二个脚本
6. 不要盲试，用现成控制台读状态
7. 不要自己下需求单
8. 改源，不改产物
9. **本轮采集数据达到预期后，立刻发复位命令**（2026-11-24 用户拍板）

### 8.1 第 9 条的展开

**每轮上板前先写清楚「这一轮要拿到的数据是什么」。一旦控制台已经拿到，
就发 `reboot`，不要继续挂着。**

理由：一个 boot 周期只有一次 CDC open 机会。数据拿到之后继续挂着的唯一后果
是端口被占着，下一轮想刷机时要么杀不掉阻塞在串口的进程、要么撞
`PermissionError(13)`，只能断电。设备留在系统态不会产生新信息。

做法：

```bash
printf 'reboot\n' >> /e/nabu/logs/<tag>-cmd.txt
# kb-129 起的正确现象：TX REBOOT 之后【没有】RX 回复，3 秒内回到 recovery
```

**判据仍按第 3 条**：COM3 消失 **且** `adb devices` 出现 recovery，
`adb devices` 为空不等于设备死了。

这条与第 4 条不冲突：第 4 条禁的是**自动**复位兜底（`--arm-at` 那种会在该看
日志的时候把设备抢走）；第 9 条说的是**数据已经到手之后**由人主动发的那一次。

### 8.2 复位命令已经修好（kb-129，2026-11-24 05:44 真机验证）

**REBOOT 现在真的能复位设备。判据：发命令后 3 秒内 adb 出现 recovery，随后 COM3 消失。**

**关键现象：`TX REBOOT` 之后不会收到 `REBOOT OK`。** 这是正确的——复位走
PSCI `SYSTEM_RESET`，`noreturn`，回复帧来不及发出。**看到 TX 无 RX 就是成功的信号。**

**为什么以前不行：** 正确实现一直在 `patches/0008-nabu-reboot-worker-and-wake.patch`
里，但它从来没进构建序列 —— `series` 里没列它（那里有个同名的
`0008-nabu-wdt-init.patch`，是另一个东西），而且它调用了模块里从未实现的
`qcom_usb_debug_set_wake_hook`，加进去编译不过。

**修法（已固化）：** 复制成 `0018-nabu-reboot-worker-and-wake.patch`，删掉
wake-hook 那 4 行，加进 `patches/xnu/series` 第 9 行（`0007` 之后）。

**完整记录见** `E:/nabu/state/SOLVED-reboot-command-kb129.md`，含：
- 三条失败路径的定位过程
- 反汇编验证方法（`qcom_usb_reboot_worker` 应 `bl _qcom_usb_debug_platform_reset`）
- 内核变大 4,096 B 后必须重生 payload（`<90-61499>` → `<90-61501>`）
- 复位路径的完整调用链（worker 循环 → take → arm → PSCI）

---

## 9. 2026-11-23 18:25 追加：本轮新增的四条红线

### 9.1 `wsl bash -lc` 会卡住

现在 `-lc`（login shell）加载 profile 会挂。**一律用**：

```bash
MSYS_NO_PATHCONV=1 wsl.exe --exec bash /mnt/e/nabu/tmp/x.sh      # 脚本
MSYS_NO_PATHCONV=1 wsl.exe --exec bash -c '...'                  # 单行（无变量展开风险时）
```

### 9.2 每次重链接，BSS/data 符号地址都会变

实测：`_kdebug_enable` 从 `0xc185a18`（kb-64）→ `0xc189a18`（kb-65），
因为加了代码后 `.data`/`.bss` 布局变化。

**⇒ 换内核包之后，任何"按地址读变量"的操作都必须先用
`llvm-nm <新kernel.macho>` 重新查地址。** 用旧地址会读到别的变量，得出错误结论
（我曾因此误判 `trace_wrap` 失效）。

代码里的 `&符号` 引用不受影响（链接器解析）；**只有主机侧手工拼地址的
`READ`/`WRITEPA` 才有这个问题**。

### 9.3 `llvm-strings | grep -x "str"` 查不到带前导空格的字符串

控制台输出里很多字符串是 `" state=..."` 带前导空格，`grep -x` 要求整行精确匹配
⇒ **查不到**，会误判"没编译进去"。

```bash
# 错
llvm-strings K | grep -cx "hfi_q"        # 0
# 对
llvm-strings K | grep -c "hfi_q"         # 1
```

### 9.4 单句柄 CDC：换工具就是破坏性操作

**一个 boot 周期只有一次 open 机会。** 我连续用 sxnu_ctl → PowerShell → 手工 python
→ bridge 轮流开关，第 4 次就 `PermissionError(13)` 锁死，只能断电。

**⇒ 一个 tag、一个工具、跑到底。要换就复位。**

**正确姿势**：`route_a_bridge.py` 复用 `nabu_console.Session`（reader 线程分流日志），
不要用 `sxnu_ctl`（同步读会被 1MB 日志洪流淹没）。

---

## 10. 一句话（更新）

> **改之前先读构建记录的 `"file"` 字段；它是产物就去改生成器；
> 改完核对对象比源码新、符号真在对象里。五步全过才上板。**
> **上板只用一个工具；换工具前先复位。**
> **换内核包后按地址读任何变量之前，先 `llvm-nm` 重查地址。**

---

## 11. 排查一律先用控制台，不许插探针（2026-11-24 用户拍板，优先级同 0）

**控制台就是为了不插探针才做出来的。** 它支持 48 个设备端 verb + 14 条主机侧命令，
能读内核虚拟地址、能读物理地址/MMIO、能写、能发复位——**每一轮遇到疑问时的第一个动作
必须是「用哪个 verb 能读到这个值」，而不是「我该在哪加一行 IOLog」。**

### 11.1 为什么这条要写进纪律

2026-11-24 一天里，我至少四次在遇到未知值时先去改驱动加打印，每次都被用户拦住。
其中最典型的一次：`presenter.bind()` 返回 `kIOReturnIOError`，我准备在
`NabuDpuNativePresenter.cpp` 里加一行打印 `snapshotActive` 的七个字段；
**而正确的做法是 `READ` 那两组已经导出的数组**
（`native_ufs_dpu_check[]` / `native_ufs_dpu_smmu[]`），一步就拿到了
TCR、FSR、表物理地址。

**探针的代价是真实存在的：**
- 一轮构建 4 分钟 + 一轮上板 3 分钟，而一次 `READ` 是 8 秒
- 探针会改变时序（IOLog 在关键路径上会拖慢甚至改变行为）
- 探针留在源码里会污染后续的「证据链」——分不清是原逻辑还是调试代码的结果
- 探针加在 `fail()` 路径上尤其危险：它可能永远不执行，白费一轮

### 11.2 决策顺序（照着问自己）

| 想知道的 | 先用 |
|---|---|
| 某个全局变量的值 | `sym <名字>` 拿地址 → `READ <addr> <len>` |
| 某个结构体字段 | `sym` 拿基址 → 按偏移 `READ`；偏移从 `llvm-nm` + 反汇编确认，不猜 |
| 导出的计数/状态数组 | 直接 `READ`（`native_ufs_dpu_check` / `native_ufs_dpu_smmu` / `native_legacy_*`）|
| MMIO 寄存器（白名单内）| `READPA`。**一次不超过 ~40 字节**，超长会卡死 worker（kb-72 实证）|
| MMIO 寄存器（白名单外，如 SMMU）| 读不到。**改读「驱动自己缓存的那份」**（如 `native_ufs_dpu_smmu[]`），或从旁边能读的证据推断 |
| 内核走到哪一步 | 日志里的具名行（`NABU DPU ...`、`owner stage=`、`nabu-display: frames ...`）|
| 某个函数被调用过没有 | 看它自己的日志行；没有日志就用 `sym`+`READ` 读它的计数器（如果导出）|
| 真的读不到（值只在栈上、或设备已卡死）| **才可以考虑改代码**——且必须在代码注释里写明为什么控制台做不到 |

#### 11.2.1 【2026-11-24 事故】READPA 猜物理地址把内核打死

**`READPA 0xd15a6000 64` 直接触发 `panic(cpu 0): Unaligned kernel data abort`。**

```
pa-rd: 0x00000000d15a6000 l=100
panic(cpu 0): Unaligned kernel data abort at pc 0xfffffff007ae382c
	  x12: 0x00000000d15a6003   esr: 0x96000021   far: 0xfffffff000a1a044
```

`esr=0x96000021` 的 DFSC `0x21` = **alignment fault**。`x12` 是「物理地址 + 3」，
说明 worker 内部按字节推进后做了未对齐的宽访问。当时的意图是读 SMMU 页表，
而这个物理地址**不是 READPA 设计要读的 MMIO 区间**——同一次会话里
`READPA 0x15082000` 明确回了 `state=denied name=outside`，白名单确实存在，
但页表地址落在了**允许却未验证**的范围内，于是命令被放行并打死了内核。

**三条硬规则（缺一即不许发）：**

1. **READPA 的地址必须来自白名单。** 「我猜这个物理地址有我要的值」不是理由。
   页表、SMMU context、struct 这类结构体**不通过 READPA 读**。
2. **长度 ≤ 40 字节**（§11.2 原有规定，这次用了 64）。
3. **读不到就先想「驱动是不是已经导出了」。** 本次真正想要的 SMMU 状态，
   `native_ufs_dpu_check[]` / `native_ufs_dpu_smmu[]` 里本来就有，既安全又是同一份数据。

**代价**：设备 panic 进 KDP 等待，CDC 句柄被占死无法软件复位，只能断电；
屏幕冻结在锁屏，看起来像「系统卡死」，实际是这条命令。一轮诊断因此报废。

### 11.3 已导出的诊断数组（优先用这些）

| 符号 | 内容 |
|---|---|
| `native_ufs_dpu_smmu[24]` | `[0]`summary `[2]`idr0 `[3]`idr1 `[4]`slot\|bank `[5]`smr `[6]`s2cr `[7]`cbar `[9]`sctlr `[22]`fsr |
| `native_ufs_dpu_check[10]` | `[0]`TTBR0 读回 `[1]`TCR 读回 `[2]`FSR `[3]`fault 时的 FSR `[7]`us |
| `native_ufs_dpu_state` | `0x80000002` = UFS 已装 bank 2 |
| `native_legacy_transfer[]` / `native_legacy_frame[]` | swap/帧计数 |
| `native_legacy_validation[]` | `[2]` = nrej |

**⚠️ 缓存的数组是快照，不是实时值。** `native_ufs_dpu_smmu[9]` 是 UFS
**启用 bank 之前**读的 SCTLR（`0xe0`），实时值是 `dpuEnable` 写的 `0x61`。
**用之前先问「这是什么时候写的」。**

### 11.4 加代码的唯一合法理由

只有这三种，缺一不可：
1. **功能本身**（不是诊断）——例如让 adopt 的 context 通过检查
2. 控制台**结构上**读不到（值只在函数栈、或需要在内核态做决策）
3. 用户明确要求

**「我想知道 X 是不是 Y」不是理由。** 那一定能用 `READ` 或日志回答。
