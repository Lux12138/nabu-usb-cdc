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

上板侧八条速查（详见原文档）：

1. 10 秒轮询，永不 sleep 30/48/60
2. 先起控制台，再刷机
3. WDT 咬回判据：COM3 消失 **且** adb 出现 recovery
4. 复位一律手动发，禁 `--arm-at`
5. 只用整合控制台，不开第二个脚本
6. 不要盲试，用现成控制台读状态
7. 不要自己下需求单
8. 改源，不改产物
