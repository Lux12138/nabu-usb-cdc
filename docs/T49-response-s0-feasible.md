# T49 回复：S0 已定论——**可做，且不用改 XNU**

> 2026-11-23 19:40。执行侧（我们）对 `T49-sysregscan-plan.md` 的核对结果。
> **核心结论：方案的前提判断需要修正一处，修正后 S1/S2 变为可行。**

---

## 1. 一句话

**S0 不需要"改 XNU 加 trap 表"，XNU 自带 `ml_expect_fault_pc_begin()`，
而且 `CONFIG_XNUPOST` 在本板构建里是开着的。** 非法 `mrs` 会先问
`expected_fault_handler`，`!is_user`（内核态）分支也覆盖。
**⇒ S1 可以直接做，S2 才有资格谈。**

---

## 2. 贵方对现状的两处判断需要修正

### 2.1 「异常被 XNU 吞掉/卡死」→ 不对，是 **panic**

贵方原文（§2.3）：

> 现场 `SYSREGW` 写 TPIDR 导致**静默卡死**（不是 fatal panic）⇒ 异常被 XNU 吞掉/卡住

**实际是 panic，不是静默卡死。** 源码（`osfmk/arm64/sleh.c:1034`）：

```c
if (PSR64_IS_KERNEL(get_saved_state_cpsr(state))) {
    ...
    } else {
        panic("Undefined kernel instruction: pc=%p instr=%x", ...);
    }
}
```

加上本板 `nabu-panic-restart=2`，panic 后会 PSCI 自动复位，
**所以现场看到的是"设备重启了"**，很容易误判成"静默卡死"。kb-60 那次就是。

### 2.2 「必须先让单条 mrs 失败不 fatal」→ 对，但**不用自己造机制**

这是本回复的重点。

---

## 3. 决定性证据：XNU 自带「预期 fault」机制，而且本板已启用

### 3.1 本板 `CONFIG_XNUPOST` 是开的（不是默认关）

从链接编译记录（`.o.json`）的 `arguments` 里直接读到：

```
-D CONFIG_XNUPOST
```
（样本：`libkern/DEVELOPMENT/inet_ntoa.o.json`，本板 `DEVELOPMENT_ARM64_BCM2837`）

### 3.2 非法指令分支**真的会先问 handler**（含内核态）

`osfmk/arm64/sleh.c:816-828`：

```c
case ESR_EC_UNCATEGORIZED:
    assert(!ESR_ISS(esr));

#if CONFIG_XNUPOST
    if (!is_user && (expected_fault_handler != NULL) && expected_fault_handler(state)) {
        /*
         * The fault handler accepted the exception and handled it on its
         * own. Don't trap to the debugger/panic.
         */
        break;
    }
#endif /* CONFIG_XNUPOST */
    handle_uncategorized(&context->ss);
    break;
```

**`!is_user` 就在条件里 ⇒ 内核态被覆盖。** 这不是推断，是分支条件本身。

### 3.3 匹配方式有「按 PC」这一种，正好适合固定 stub

`osfmk/arm64/machine_routines.c:2760-2788`，三个导出函数：

```c
void ml_expect_fault_begin(expected_fault_handler_t h, uintptr_t expected_fault_addr);
void ml_expect_fault_pc_begin(expected_fault_handler_t h, uintptr_t expected_fault_pc);
void ml_expect_fault_end(void);
```

`ml_expect_fault_pc_begin()` 存的 `expected_fault_pc` 在 `sleh.c:721-730` 与当前 PC 比对：

```c
matching_fault_pc = (saved_expected_fault_pc == get_saved_state_pc(state));
if (saved_expected_fault_addr == far || matching_fault_pc) {
    expected_fault_handler = saved_expected_fault_handler;
}
```

**⇒ 只要让非法 `mrs` 固定在某一个 PC 上执行，就能精确认领它。**

---

## 4. S0 的具体实现（不用改 XNU，只改我们的模块）

### 4.1 关键：把 `mrs` 放在一个**独立的、唯一的 PC** 上

T49 §2.2 担心"要可执行内存"——**不需要**。做法是：

```c
/* 一个 noinline 小函数，指令字由运行时参数决定不了，
 * 所以这里只能按「一批」来，不能按「一条」——
 * 见 §4.2 的取舍。 */
__attribute__((noinline))
static uint64_t nabu_mrs_stub(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, S3_0_C0_C0_0" : "=r"(v));  /* 占位，见下 */
    return v;
}
```

**问题**：`mrs` 的寄存器编码是**编译期常量**，不能从变量来。
所以"一条通用 stub 扫 1024 个编码"在 C 层面做不到。

### 4.2 两条可行路线（都要在 S0 里定）

| 路线 | 做法 | 代价 |
|---|---|---|
| **(A) 表驱动 + 每条一个 stub** | 编译期生成 1024 个 `noinline` 小函数，每个一条固定 `mrs`，配一张 `{enc, func}` 表；调用前 `ml_expect_fault_pc_begin(handler, func)` | 代码 ~1024×16 B = 16 KB；编译时间长一点。**完全不用改 XNU** |
| **(B) 运行时生成指令 + `br`** | 在可写可执行页上组装 `mrs x0, enc; ret`，`br` 过去 | 需要 JIT 页（XNU 内核 text 只读，要另开 `VM_PROT_EXECUTE` 页）——**贵方 §2.2 提到的那个坑**，S2 才需要 |

**我们的建议：S1 走 (A)**。1024 个 stub 由脚本生成（我们已有生成器基建），
**不碰 XNU、不碰可执行内存**，风险最低。

### 4.3 handler 怎么写

```c
static bool nabu_mrs_fault_handler(arm_saved_state_t *state)
{
    /* 让 PC 前进到下一条指令，返回值填哨兵，然后告诉 SLEH「我处理了」 */
    add_saved_state_pc(state, 4);
    set_saved_state_reg(state, 0, NABU_MRS_SENTINEL);   /* x0 = 0xdeadc0dedeadc0de */
    return true;    /* ← sleh.c:821 会 break，不走 handle_uncategorized */
}
```

**四个符号全部核实过**（不是猜的）：

| 符号 | 位置 | 签名 |
|---|---|---|
| `expected_fault_handler_t` | `osfmk/arm/machine_routines.h:54` | `typedef bool (*)(arm_saved_state_t *)` |
| `add_saved_state_pc` | `osfmk/mach/arm/thread_status.h:558` | `(arm_saved_state_t *, int diff)` — **一步推进，比 set 更稳** |
| `set_saved_state_reg` | `thread_status.h:728` | `(arm_saved_state_t *, unsigned reg, register_t value)` |
| `ml_expect_fault_pc_begin/end` | `machine_routines.h:1437-1439` | 可直接 extern |

**注意**：`set_saved_state_pc` 存在但要 **`HAS_SET_SAVED_STATE_PC`** 开关；
`add_saved_state_pc` 要 **`HAS_ADD_SAVED_STATE_PC`**。

**这两个宏本板都是开的**（`thread_status.h:263-266`，无条件 `#define ... 1`）：

```c
#define HAS_ADD_SAVED_STATE_PC          1
#define HAS_SET_SAVED_STATE_PC          1
#define HAS_SET_SAVED_STATE_REG         1
```

**⇒ 上面那段 handler 可以直接编译，不需要退到裸结构体写入。**

### 4.4 哨兵值

沿用贵方的 `0xdeadc0dedeadc0de`。但**注意一个歧义**：
如果某个寄存器**真的**读出了这个值（概率极低但可能），会被误判成 fault。
**建议双哨兵**：`0xdeadc0dedeadc0de` + 一个 `faulted` 位图（每编码 1 bit），
不靠值本身判断。

---

## 5. 对 T49 分期的回应

| 期 | 贵方计划 | 我们的判断 |
|---|---|---|
| **S0** | 让单条 `mrs` 失败不 fatal | ✅ **可行，不用改 XNU**。用 `ml_expect_fault_pc_begin` + 每编码一个 noinline stub |
| **S1** | 扫 `arm_regs.json` 已知编码（~300） | ✅ 建议**直接上 (A) 表驱动**，一次刷机扫完，不用逐条 verb |
| **S2** | 全扫 1024 | ✅ 同一套表驱动，只是表更大。**不需要 JIT** |
| **S3** | 位域解码（主机侧） | ✅ 纯主机侧，与我们无关，可并行 |
| **S4** | EL0/EL2 可达性 | ❌ 同意贵方判断，做不了 |

**⇒ 修正后：S1 和 S2 合成一步做，都是一张静态表 + 一个 `SYSREGSCAN <start> <count>` verb。**

---

## 6. 我们要先做的验证（S0，一次刷机）

**判据（全部要在真机上闭合）：**

| # | 动作 | 期望 |
|---|---|---|
| 1 | `SYSREGSCAN 0 1` 读一个**合法**编码 | 返回真值，无 fault |
| 2 | `SYSREGSCAN <必 trap 的编码> 1` | 返回 **哨兵值 + faulted=1**，**设备仍活着** |
| 3 | 紧接着 `HELLO` | ✅ 有回复（证明没 panic、没卡死）|
| 4 | `STATUS` 的 `rej` 计数 | 无异常增长 |
| 5 | 连续扫 256 个（混合合法/非法） | 全部返回，设备存活 |

**如果第 2/3 条不过**，说明 `expected_fault_handler` 在这个路径上没生效
（可能 `ptrauth_strip` 让 PC 比对失配），那就退回"只扫已知可读编码"（S1 的窄版）。

---

## 7. 一个必须先确认的细节（我们实现时会查，先告知）

`sleh.c:721-730` 的 PC 匹配在 `__has_feature(ptrauth_calls)` 下会做
`fault_addr_bitmask(0, 64 - T1SZ_BOOT - 1)` 掩码比较。
**这本身没问题**（它对两边都掩码），但要确认 `ml_expect_fault_pc_begin()`
里 `ptrauth_strip(..., ptrauth_key_function_pointer)` 与前向声明的
函数指针类型一致——否则 strip 出来的地址和实际 PC 对不上。

**这是我们实现时第一个要验证的点，不需要贵方额外回答。**

---

## 8. 一句话总结

> **S0 的硬骨头已经被 XNU 自己解决了**（`ml_expect_fault_pc_begin` +
> `CONFIG_XNUPOST` 已开 + `ESR_EC_UNCATEGORIZED` 分支含 `!is_user`）。
> **唯一要做的是：为每个待扫编码编译一个独立的 noinline `mrs` stub**，
> 让 PC 唯一、可认领。**不改 XNU、不碰 JIT 页。**
> 修掉"静默卡死"这个误判（实际是 panic + 自动复位）之后，
> **S1/S2 可以合成一次刷机做完。**
