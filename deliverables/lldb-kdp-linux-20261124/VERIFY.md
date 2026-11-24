# T54-patch 验证记录（VERIFY）

- 日期：2026-11-24
- 目标：把 `kdp-remote-linux.patch`、`processkdp-ub-fix.patch` 从「lint 通过」升级为「对真实上游源码 dry-run 通过」。
- 基线：`llvm/llvm-project` `release/20.x`，tip = `87f0227cb60147a26a1eeb4fb06e3b505e9c7261`
  （`git ls-remote https://github.com/llvm/llvm-project refs/heads/release/20.x`，2026-11-24）。
- 纪律：未下载 LLVM 源码树、未 clone 仓库、未构建、未上板、未碰 `0x3c0fc000`；仅按 URL 逐文件抓取到临时目录 `%TEMP%\nabu-t54-verify`，验证后已清理。

## 1. 抓取清单（raw.githubusercontent.com/llvm/llvm-project/release/20.x/<path>）

| 文件 | 字节数 | ETag（响应头原值） |
|---|---|---|
| `lldb/source/Plugins/Process/CMakeLists.txt` | 728 | `cd5d101616e8a240d895ba2c09fb393879e027767a07906a525ee3ecbc2366d2` |
| `lldb/source/Plugins/Process/MacOSX-Kernel/CMakeLists.txt` | 939 | `915ef12d33fea0b464bc41d24a107e993a52d815123788365834b010a34feb2f` |
| `lldb/source/Plugins/Process/MacOSX-Kernel/ProcessKDP.cpp` | 34898 | `01be1da68be7b733f63f42f5a92d1841ba8d1a07e8ca72bfdb6b008487f96f50` |
| `lldb/source/Plugins/Process/MacOSX-Kernel/CommunicationKDP.cpp` | 49423 | `a10330991fd9dd75980888209de85484b1713ed318b19d68f9b86197b14b4c27` |
| `lldb/include/lldb/Host/Socket.h` | 5597 | `a2db828325fa2999763991dd1fc82213b8367128b9fc7a8ce7df52740eb29391` |
| `lldb/include/lldb/Host/SocketAddress.h` | 5905 | `b3b1d1100ef8f1f00f6b4cbd3dd134deacee51506501daf7dcf7c1bf685aca6d` |
| `lldb/include/lldb/Host/common/TCPSocket.h` | 1948 | `405209ab3a697cf2468771da532a9e1ac54594bf36fa3b11045901e273745616` |

全部为 LF（CRLF=0），UTF-8 无 BOM。临时目录内按原相对路径摆放成迷你源码树，仅含补丁涉及的文件。

## 2. 验证环境与命令

| 工具 | 版本 / 位置 |
|---|---|
| git | 2.56.0.windows.2（PortableGit）；另用 WSL Ubuntu-24.04 git 2.43.0 交叉 |
| GNU patch #1 | 2.8，`C:\Users\Administrator\.dsh-tools\PortableGit\usr\bin\patch.exe` |
| GNU patch #2 | 2.7.6，WSL Ubuntu 24.04 `/usr/bin/patch` |

命令（在迷你树根目录执行，补丁以**文件参数**给出）：

```bash
git apply --check <patch>
patch -p1 --dry-run -i <patch>          # msys patch 2.8
patch -p1 --dry-run -i <patch>          # WSL patch 2.7.6
```

主机环境注意：本机 git 由包装器注入 `GIT_CONFIG_COUNT`/`GIT_CONFIG_VALUE_*` 但缺 `GIT_CONFIG_KEY_*`，直接调用会报 `missing config key GIT_CONFIG_KEY_0`；清理这几个环境变量后 git 正常。另：**不要**用 PowerShell 管道把补丁喂给 patch（`Get-Content -Raw | patch`），换行转换会造成伪失败/伪 fuzz；以文件参数传入才可靠。

## 3. 最终结果（当前交付版本）

```
[1] git apply --check
    kdp-remote-linux.patch       rc=0
    processkdp-ub-fix.patch      rc=0

[2] msys GNU patch 2.8 -p1 --dry-run
    kdp-remote-linux.patch       rc=0  checking file lldb/source/Plugins/Process/CMakeLists.txt
    processkdp-ub-fix.patch      rc=0  checking file .../MacOSX-Kernel/ProcessKDP.cpp
                                        checking file .../MacOSX-Kernel/CommunicationKDP.cpp

[3] WSL GNU patch 2.7.6 -p1 --dry-run
    kdp-remote-linux.patch       rc=0
    processkdp-ub-fix.patch      rc=0（两个文件均只打印 "checking file"，无 fuzz/offset/FAILED）

[4] WSL GNU patch 真实应用
    rc1=0, rc2=0；应用后大小（LF 保持）：
      Process/CMakeLists.txt         728 -> 777
      MacOSX-Kernel/ProcessKDP.cpp   34898 -> 35574
      MacOSX-Kernel/CommunicationKDP.cpp 49423 -> 49498
```

逐 hunk（均为规范 unified diff，3 行前导 + 3 行尾随上下文）：

| 补丁 | hunk | 结果 |
|---|---|---|
| `kdp-remote-linux.patch` | `@@ -12,8 +12,9 @@`（Process/CMakeLists.txt） | pass（两工具干净，无 offset/fuzz） |
| `processkdp-ub-fix.patch` | `@@ -9,6 +9,11 @@`（ProcessKDP.cpp include 小修） | pass |
| `processkdp-ub-fix.patch` | `@@ -18,6 +23,8 @@`（Socket.h/SocketAddress.h include） | pass |
| `processkdp-ub-fix.patch` | `@@ -230,9 +237,16 @@`（UB 修复主体） | pass |
| `processkdp-ub-fix.patch` | `@@ -12,6 +12,10 @@`（CommunicationKDP.cpp arpa/inet.h） | pass |

## 4. 修正说明（旧版 → 现版）

旧版 `processkdp-ub-fix.patch` 的手写上下文在 GNU patch 下失败，且 **git apply 与 GNU patch 结论不一致**：

```
旧版（git apply --check）：rc=0 通过
旧版（msys patch 2.8 / WSL patch 2.7.6 dry-run）：
  ProcessKDP.cpp       Hunk #1 succeeded at 9 with fuzz 2.
                       Hunk #2 succeeded at 24 (offset 3 lines).
                       Hunk #3 succeeded at 239 (offset 2 lines).
  CommunicationKDP.cpp Hunk #1 FAILED at 10.   1 out of 1 hunk FAILED
```

最小复现确认：把该 hunk 单独抽出、甚至放到合成文件上，GNU patch 仍拒绝；触发因素是**前导上下文 4 行 + 尾随上下文 1 行**的非对称布局（同内容改成 3+3 或增加尾随上下文即通过）。`git apply` 能接受这种布局，所以只看 git apply 会漏判。

修法：不改任何改动语义，把 hunk 重新生成为 GNU `diff -u` 规范形式——具体做法是用旧补丁在 release/20.x 原文上实际得到「改动后文件」，再对该文件对取 `diff -u`，反向作为新补丁体。校验：

- 新补丁应用结果与旧补丁应用结果**逐字节一致**（ProcessKDP.cpp、CommunicationKDP.cpp 均 True）；
- 新补丁在两个实现 × 两种 dry-run 下全部干净通过；
- `kdp-remote-linux.patch` 原本就两工具干净通过，**hunk 体未改动**（文件头注释在复核后更新为实测结论，注释行不影响解析与应用）。

## 5. 静态一致性检查（门控）

应用后 `lldb/source/Plugins/Process/CMakeLists.txt` 第 1-24 行（实测全文）：

```cmake
 1 if (CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
 2   add_subdirectory(Linux)
 3   add_subdirectory(POSIX)
 4 elseif (CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
 ...
12 elseif (CMAKE_SYSTEM_NAME MATCHES "Windows")
13   add_subdirectory(Windows/Common)
14 endif()
15 if (CMAKE_SYSTEM_NAME MATCHES "Darwin" OR CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
16   add_subdirectory(MacOSX-Kernel)
17 endif()
18 add_subdirectory(scripted)
 ...
```

- `add_subdirectory(MacOSX-Kernel)` 在父文件中出现次数 = **1**（仅第 16 行），不存在重复添加。
- 子文件 `lldb/source/Plugins/Process/MacOSX-Kernel/CMakeLists.txt` 全文 34 行，`CMAKE_SYSTEM_NAME` 出现次数 = **0**，即 **release/20.x 的子文件没有任何平台门控**（「Darwin」字样只出现在 target/库名里），不会把父级的放行重新关掉。
- 结论（两处最终生效条件）：
  - 父级放行条件 = `CMAKE_SYSTEM_NAME MATCHES "Darwin" OR CMAKE_SYSTEM_NAME MATCHES "Linux|Android"`（第 15 行）；
  - 子级无任何条件。
  - 因此 Linux/Android 与 Darwin 都会构建 `lldbPluginProcessMacOSXKernel`；Windows/FreeBSD/NetBSD/OpenBSD 两个条件都不满足，维持原行为（不构建）。

## 6. UB 修复涉及的 API 事实（release/20.x 直读）

- `Socket : public IOObject`（Socket.h:68）；`NativeSocket GetNativeSocket() const { return m_socket; }`（Socket.h:138，public、非虚）。
- `GetLocalPortNumber()` 只在 `TCPSocket` 声明（TCPSocket.h:27），`Socket` 基类无此接口 → 原 `static_cast<const TCPSocket&>` + UDP 对象确为向下转型误用。
- `SocketAddress`：`static socklen_t GetMaxLength()`（:71）、`uint16_t GetPort() const`（:83）、`operator const struct sockaddr *() const`（:156）→ `::getsockname(fd, sock_addr, &len)` 可编译且与 `TCPSocket::GetLocalPortNumber()` 的实现（TCPSocket.cpp）逐行等价。

## 7. 仍未验证项

1. **运行期行为**：UDP 连接下 `reply_port != 0` 需真机首连验证（README §4-3），本机无法验证。
2. **编译/链接**：未构建 LLVM（任务禁止），`<unistd.h>`/`<sys/socket.h>`/`<arpa/inet.h>` 之外是否还有 Linux 下的编译错误（如其它 Apple 专有 API）未验证。
3. **构建漂移**：以上基于 2026-11-24 的 tip `87f0227c…`；上游推进后需重跑本文件内的 dry-run。
4. **Windows/其它平台影响**：设计上不变，但未在这些平台验证。
