# T54-patch · 在 Linux 上构建带 `kdp-remote` 的 lldb（release/20.x）

- 日期：2026-11-24
- 性质：**源码与文档交付**。本次运行未下载 LLVM、未编译、未改任何仓库、未上板、未碰 `0x3c0fc000`。
- 上游基线：`llvm/llvm-project` `release/20.x`（2026-11-24 直读该分支 tip；行号口径同 T54）。
- 输入结论：`../T54-linux-lldb-kdp-port.md`（门控位置、依赖面、UB 风险、推荐基线均为其【实证】结论）。
- 设备侧前提（不在本交付内）：`../kdp_net_bridge/` 已提供 `--cdc-framing serial --udp-mode kdp` 的标准 UDP 裸 KDP，端口 `41139`。
- **验证状态（2026-11-24 实测）**：两个补丁均已在 `release/20.x` 真实单文件副本上通过 `git apply --check` 与 GNU `patch -p1 --dry-run`（msys 与 WSL patch 2.7.6 双实现）；`processkdp-ub-fix.patch` 的 hunk 已改为规范 `diff -u` 形式（旧版被 GNU patch 拒绝）。命令与输出见 `VERIFY.md`。

---

## 1. 这版做了什么

| # | 文件 | 内容 | 证据级 |
|---|---|---|---|
| 1 | `kdp-remote-linux.patch` | 改 **1 个文件、净增 4 行**：把 `add_subdirectory(MacOSX-Kernel)` 移出 Darwin 分支，改为「Darwin 或 Linux/Android 才构建」。子文件 `MacOSX-Kernel/CMakeLists.txt` 经核对**无需改动**（无门控；LINK_LIBS 已含 `lldbPluginDynamicLoaderDarwinKernel` / `lldbPluginDynamicLoaderStatic` / `lldbPluginProcessUtility`）。 | 【实证】原文直取逐行一致 |
| 2 | `processkdp-ub-fix.patch` | (a) **UB 修复**：`ProcessKDP.cpp` 中 `static_cast<const TCPSocket&>(*conn_up->GetReadObject())` + `GetLocalPortNumber()` 改为「`static_cast<const Socket&>` + `getsockname(GetNativeSocket())`」，TCP/UDP 通用；(b) 三处编译小修：`<unistd.h>`(usleep)、`<sys/socket.h>`(getsockname)、`<arpa/inet.h>`(htons)。 | (a)【实证】问题点+替代实现均已直读源码；(b)【推断】是否需要取决于传递包含，按报错增删 |
| 3 | `build-lldb-linux.sh` | tarball 取源码 → 应用补丁 → CMake/Ninja（仅 lldb，内存保护参数）→ 自检。 | 脚本，未执行 |
| 4 | 本 README | 步骤 / 连接 / 验收 / 风险 / 回滚 | — |

为什么值得做：移植成功后用 Apple 官方 `CommunicationKDP` 协议栈 + 完整 lldb 前端连我们的 XNU，而不是用自写 Python 客户端（`../kdp_client/`，仅部分 verb、无符号化）。

---

## 2. 逐步操作（Linux / WSL2 Ubuntu）

```bash
# 0) 依赖（Ubuntu 示例；脚本不会自动 apt）
sudo apt install -y cmake ninja-build clang lld python3 \
     libedit-dev libncurses-dev zlib1g-dev liblzma-dev swig

# 1) 放进补丁与脚本（本目录四个文件），然后
cd <本目录>
chmod +x build-lldb-linux.sh
./build-lldb-linux.sh            # 下载+补丁+配置+构建一条龙
```

手动分步（等价，便于排查）：

```bash
curl -fSL -o llvm.tar.xz \
  https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.0/llvm-project-20.1.0.tar.xz
tar -xf llvm.tar.xz && cd llvm-project
patch -p1 < ../kdp-remote-linux.patch
patch -p1 < ../processkdp-ub-fix.patch
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS=lldb -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_PARALLEL_LINK_JOBS=1 \
  -DLLDB_ENABLE_PYTHON=OFF -DLLDB_ENABLE_CURSES=OFF -DLLDB_ENABLE_LIBEDIT=OFF
cmake --build build --target lldb -j$(nproc)
```

长构建建议放后台并按 **10 秒轮询**日志（纪律要求，禁 sleep 死等）：

```bash
nohup ./build-lldb-linux.sh > build.log 2>&1 &
tail -f build.log        # 或每 10 秒看一次 ninja 进度行与 build 目录大小
```

资源预估【推断】：全量约 0.5~2 h、磁盘 30~60 GB；内存保护见脚本顶部（JOBS≈RAM/2、链接串行、Python OFF）。若首次构建因链接 OOM 失败，降 `JOBS` 重试或去掉了 `-DLLDB_ENABLE_PYTHON=OFF` 之外再减 target。

---

## 3. 连接我们的设备（命令已核实）

网桥侧（终端 1，Windows）：

```bat
python E:\nabu\state\bridge\answers\kdp_net_bridge\kdp_net_bridge.py --cdc-framing serial --udp-mode kdp ...
```

lldb 侧（终端 2，Linux）：

```bash
./build/bin/lldb -o "settings set plugin.process.kdp-remote.packet-timeout 5" \
                 -o "process connect --plugin kdp-remote udp://127.0.0.1:41139"
```

命令口径说明（避免踩坑）：合法形式是 `process connect --plugin kdp-remote udp://<host>:<port>`，`kdp-remote` 是**插件名**，URL scheme 是 `udp://`；`kdp-remote://…`、`kdp-connect …`、`plugin select …` 均非法定形式【实证，T29 §1.2 直读 Apple 文档】。Apple `kdp-remote <host>` 简写内部即拼成 `udp://<host>:41139`（默认端口一致，我们正好用 41139）。

连上后可用：`showallkmods`、内存/寄存器读写、软件断点、`step`（走 ARM debug 寄存器）、`image lookup` 等；符号化需配合 `ipsw` 抽出的 kernelcache + KASLR slide。

---

## 4. 验收判据

1. **构建产物**（脚本末尾自检）：
   ```bash
   strings build/lib/liblldb.so* | grep -i -m5 -e kdp-remote -e darwin-kernel
   ```
   预期：能看到 `kdp-remote`（Ubuntu 发行版 lldb-18 里**看不到**，这是对照差异）；`darwin-kernel` 出现属正常（DynamicLoaderDarwinKernel 本来就链入）。
2. **连接成功**：`process connect ...` 返回后打印 `Version: Darwin Kernel Version ...`（KDP_KERNELVERSION 成功），进程状态为 stopped；`showallkmods` 能列出 kmod。
3. **`reply_port != 0`**：代码里 `reply_port == 0` 会直接报 `invalid reply port from UDP connection` 并断开。所以「连接成功且未报该错误」即隐含 `reply_port != 0`；若要显式确认，开 `log enable lldb process` 观察 REATTACH/CONNECT 交换，或临时在 `DoConnectRemote` 打断点看 `reply_port`。**这是 UB 修复的第一验收点**。
4. **对照实验（可选）**：同一二进制在不打 `processkdp-ub-fix.patch` 时若也能连通，说明该环境下原转型恰好可用；仍建议保留修复（UB 不保证跨编译器/优化级别稳定）。

---

## 5. 已知风险

1. **【待实测】UB 修复本身**：`GetReadObject()` 返回 `IOObjectSP`，对象必为 `Socket`（`ConnectUDP` 建的是 `UDPSocket`），故 `static_cast<const Socket&>` 良定义；`getsockname` 路径与 `TCPSocket::GetLocalPortNumber()` 的实现逐行等价（直读 `TCPSocket.cpp` 核对）。但真机首连仍需按 §4-3 验证。
2. **【推断】编译小修可能多余**：`usleep`/`htons` 在部分工具链经传递包含已可见；若编译无警告可删对应 hunk。反之若报隐式声明，这两行就是必须的。
3. **补丁可应用性（已实测）**：两补丁在 `release/20.x` 真实单文件副本上，`git apply --check`、msys GNU patch 与 WSL GNU patch 2.7.6 的 `patch -p1 --dry-run` 均通过（含逐 hunk 结果，见 `VERIFY.md`）。注意：`processkdp-ub-fix.patch` 的旧版手写上下文（CommunicationKDP 为 4 行前导 + 1 行尾随）会被 GNU patch 判 `Hunk #1 FAILED`（git apply 却接受），现已改为规范 `diff -u` 形式；上游若推进导致漂移，以「文件 + 关键字符串」定位手工套用。构建漂移本身仍未实测。
4. **Python 关闭的代价**：`-DLLDB_ENABLE_PYTHON=OFF` 省内存，但 `lldbmacros`/部分 AppleScript 插件不可用；需要时去掉该行增量重编。
5. **协议/设备侧上限**（非移植问题，T54 §4.2-5）：`WRITEPHYSMEM64` 空实现、`step` 无 KDP verb、异步 `KDP_EXCEPTION` 经 UDP 网桥的时序需实测；这些决定体验上限。
6. **其它平台未验证**：本补丁把构建限定 Darwin/Linux/Android，Windows 等不受影响也未被本交付验证。

---

## 6. 回滚

```bash
cd llvm-project
patch -R -p1 < ../processkdp-ub-fix.patch
patch -R -p1 < ../kdp-remote-linux.patch
# 或整树丢弃：rm -rf <workdir>/llvm-project <workdir>/build
```

无运行时副作用：未上板、未改设备、未动源码仓库。

---

## 7. 来源（2026-11-24 实取）

- `https://raw.githubusercontent.com/llvm/llvm-project/release/20.x/lldb/source/Plugins/Process/CMakeLists.txt`
- `https://raw.githubusercontent.com/llvm/llvm-project/release/20.x/lldb/source/Plugins/Process/MacOSX-Kernel/CMakeLists.txt`
- `https://raw.githubusercontent.com/llvm/llvm-project/release/20.x/lldb/source/Plugins/Process/MacOSX-Kernel/{ProcessKDP.cpp,CommunicationKDP.cpp}`
- `https://raw.githubusercontent.com/llvm/llvm-project/release/20.x/lldb/include/lldb/Host/Socket.h`
- `https://raw.githubusercontent.com/llvm/llvm-project/release/20.x/lldb/include/lldb/Host/SocketAddress.h`
- `https://raw.githubusercontent.com/llvm/llvm-project/release/20.x/lldb/include/lldb/Host/common/{TCPSocket.h,UDPSocket.h}`
- `https://raw.githubusercontent.com/llvm/llvm-project/release/20.x/lldb/source/Host/common/TCPSocket.cpp`
- `https://raw.githubusercontent.com/llvm/llvm-project/release/20.x/lldb/include/lldb/Host/posix/ConnectionFileDescriptorPosix.h`
- `https://grep.app/api/search?q=static_cast%3Cconst+TCPSocket`（main tip 行号 231-233 交叉核对）
- 本地只读：`../T54-linux-lldb-kdp-port.md`、`../T29-lldb-kdp-remote.md`（§1.2 连接命令口径）
