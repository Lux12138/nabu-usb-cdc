# 踩坑清单

## SMMU

1. **不要写 `S2CR.TYPE=BYPASS`**：这块固件会把 `0x10003` 读回 `0x20003`（TYPE 变 FAULT）。
   正确做法是 S2CR 写 **bank 号**（TYPE=TRANSLATE），CB 配 `SCTLR=0 / CBAR=0x10000 / CBA2R=1`。
2. **不要用回读判定写入失败**：SM8150 的 S2CR/CBAR/SCTLR 不保证返回写入值
   （UFS 代码里回读校验是停用的）。`lr` 只作诊断，验收看事件环/主机枚举。
3. **不要放宽比较逻辑让错误消失**：`dr=17`（WriteFailed）曾靠“回读不符”触发；
   如果改成只比某些位，错误会消失但 DMA 照样不通。
4. **SID 已被固件占用**：启动时 `SMR=0x80000140 / S2CR=3` 已存在，私有翻译域必然
   `StreamExists`；不要 disable 重装域（固件的页表/属主不认识）。

## 构建

5. **归档必须晚于源码**：`llvm-nm`/mtime 闸门；库滞后会让整轮重复旧读数（真发生过两次）。
6. **改 XNU 补丁会触发 prep，清掉 `EXTERNAL_HEADERS/TrustCache`**：编译前从同版本树补回
   `TrustCache/API.h` 等，否则 `archive-11-build-mockfs-fixture` 失败。
7. **状态结构体不加字段**：内核适配器与模块逐字节共享 `qcom_usb_debug_status`；
   诊断值一律映射进已有字段（`domain_bank/asid/last_*`、`last_trace/traces/trace_a/b`）。
8. **打印格式改动要同步补丁**：`pe_kprintf.c` 的行同时存在于工作树与 `patches/xnu/0007…`，
   改完用 `git apply --reverse --check` 验证，否则下一次重建树会丢。
9. **EDK2/Mu 不许并发构建**；内核与固件分开跑。
10. `build.py` 的 `identity DMA fallback` 审计只对定义了 `QUD_DMA_PREFER_IDENTITY 1`
    的 profile 放行；Sm8550 仍应保持禁止。

## 主机侧

11. **端口刚注册时打不开**：COM3 在 PnP 出现后还要约 0.5 s 才能配置；
    捕获要“等 0.5 s + 失败 5×1 s 重试”。
12. **必须先置 DTR**：设备 `pump()` 只在 `dtr=1` 时发 IN；裸 `ReadFile` 前先
    `EscapeCommFunction(SETDTR)`，否则“零字节”是假阴性。
13. **用短超时持续读**：150 ms 超时循环到端口消失，而不是读一次就关；
    还要记录“最后一次成功读取 → 消失”的间隔（本例 31.7 s）。
14. **捕获要在 boot 前启动**：窗口只有约 32 s，2 s 轮询的热插拔检测够用，
    但启动晚了会错过枚举。
15. **不要相信“state=none”**：轮询脚本里 `tr -d '\n'` 把 `adb devices` 输出拼成一行，
    `NR>1` 永不命中——工具 bug 会被误读成板子没回来。

## 发布/隐私

16. 设备序列号、`--unique-chip-id`、token 不入库；日志里的
    `VID_1209/PID_316D/SER=QCOMXNUUSBDEBUG` 是设备固件常量，可以保留。
17. 二进制（kernel/payload/boot 镜像、`.a`）不入库，只发源码与工具。

## 复位与观测

18. 任何 reboot/切 fastboot 前先 arm misc 并读回；`boot-recovery` 是一次性的，
    ABL 读过即清。
19. **PON 不能判复位源**（三档实测）：硬件狗复位的 r3a/r3b 与历史内核 PSCI 复位都是 `0x80041`；
    `0x80081` = 冷启/人工（长按后也是它）；`0x800c1` = panic 后硬复位。
    要判"内核跑没跑过"只能看 **COM3 有没有出现**，别看 PON。另外**名义 bite ≠ 实际**：
    `mode=1` 写 240 s，实测 boot+60 s 内就复位，标定前不要按名义值规划。
20. **内核侧的 `nabu-hang-reset` 不可依赖**：三档实验（含 109 s 完全不回来的 r4）里它一次都没 fire。
    能兜底的只有 Mu 侧硬件狗，而它的编译默认档位是 0（`NABU_WDT_DEFAULT=0`），
    ESP 文件读不到就不武装 —— fail-safe，但也意味着"没报错"不等于"狗开着"。
21. **panic 文本进不了串口是代码顺序问题**：`panic_spin_forever()` 里 `nabu_panic_restart()`
    排在 `qcom_usb_debug_panic_drain()` 之前 → 复位先发生，drain 永远到不了。
    判据"串口没有 panic"≠"没有 panic"。把顺序改过来也不够：drain 在 panic 上下文里还可能因
    `phase != ACTIVE` 或拿不到锁直接返回，所以"没有 `QCOM PANIC END`"同样不能反证没有 panic。
22. `nabu_mark_text` 只写内存 ring（`NativeUfs.cpp` 里它转成 `nabu_log_puts`），**不上屏也不落盘**；
    所以"屏幕/窗口里没看到某个标记"**不能**证明对应代码没执行。
23. **userdata 窗口的正文会混着上一轮的残留**：`window_new` 只哈希首块（头），
    文本块可能整批是旧内容（实测见过两轮时间戳精确到微秒相同）。
    判"本轮到哪"必须用**只有本轮才可能出现的字符串**定位，或直接用串口。
24. 捕获脚本以 `timeout` 结束（而不是 `disconnect`）= **端口从未消失 = 全程没有复位**；
    判断活性看 tsv 的最后一条真实读取时间，别看脚本自己打的 summary 时间戳。

## 构建与打包

25. 证明"真的重编过"要用**内容差异**（归档 sha、内核符号数、新字符串是否出现），
    不要用文件 mtime —— 复制也会刷新 mtime。
26. `--unique-chip-id` 之外，**trust cache 是打包参数**：`payload.py` 的 `--trust-cache` 会把 bundle
    落成 `payloads/kernel.macho.trustcache` 并写进 ADT，`DarwinBoot.c` 有消费端
    （`ValidTrustCaches()` / ADT 里找 `TrustCache` 节点）。不带这个参数时内核会打
    `no external trust caches found (segment length is zero)` —— 这是配置缺失，不是缺功能。
