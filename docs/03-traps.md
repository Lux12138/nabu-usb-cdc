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

## 复位

18. 任何 reboot/切 fastboot 前先 arm misc 并读回；`boot-recovery` 是一次性的，
    ABL 读过即清。
19. PON 判读：`0x80041` = 内核侧 PSCI/复位源；`0x800c1` = 长按硬复位（视为异常）。
