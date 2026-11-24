# 在 Linux 上构建带 Apple `kdp-remote` 的 lldb · 完整复现手册

- 日期：2026-11-24
- 目的:**交给主线端照着跑一遍**。已在本机 WSL Ubuntu-24.04 实测通过,产出可用的 lldb 20.1.8。
- 适用:Ubuntu 22.04/24.04、WSL2、或任意能联网的 Linux 主机(含虚拟机)。
- 参考:`T54-linux-lldb-kdp-port.md`(可行性论证)、`T54-patch\VERIFY.md`(补丁 dry-run 证据)、`T61-lldb-linux-deploy.md`(部署与验收)。

---

## 1. 为什么可以这么做(30 秒版)

上游把 KDP 插件放在 `lldb/source/Plugins/Process/MacOSX-Kernel/`,而父文件 `lldb/source/Plugins/Process/CMakeLists.txt` 只在 **Darwin** 分支里 `add_subdirectory(MacOSX-Kernel)`。插件本身**没有任何平台门控**,它依赖的模块(`DynamicLoaderDarwinKernel`/`RegisterDarwin`/`ProcessUtility`)在 Linux 都是活代码。所以只要把那一处门控放宽到 `Linux|Android`,插件就会在 Linux 上编译并注册。

另有一处 Apple 遗留的 UB:`ProcessKDP.cpp` 把 UDP 连接 `static_cast<const TCPSocket&>` 后调 `GetLocalPortNumber()`(该方法只在 TCP socket 上有意义),macOS 靠内存布局恰好能用,Linux 上是 UB。第二个补丁把它改成通用写法。

---

## 2. 前置条件

```bash
sudo apt update && sudo apt install -y cmake ninja-build clang lld git curl python3 zlib1g-dev
```

资源预估【实测】:12 核 / 19 GB 内存下,**Release 全量约 25~35 分钟**,`build/` 目录峰值约 **600 MB**(不含源码),磁盘预留 20 GB 足够。链接 `liblldb.so` 峰值 4~8 GB 内存,建议 `LLVM_PARALLEL_LINK_JOBS=1`。

---

## 3. 取源码(稀疏克隆,避免拉整个仓库)

```bash
mkdir -p ~/nabu-lldb && cd ~/nabu-lldb
git clone --depth 1 --branch release/20.x --single-branch \
          --filter=blob:none --sparse https://github.com/llvm/llvm-project.git
cd llvm-project
git sparse-checkout set llvm lldb clang cmake     # 四个都要,缺一个都会在配置期报错
git rev-parse --short HEAD                        # 本次验证基线:87f0227cb
```

> **坑 1**:只 checkout `llvm lldb` 会在配置期报找不到 `cmake/modules/LLVM-Config.cmake`;漏了 `clang` 会报 `clang-tablegen-targets` 缺失(**LLDB 上游强制依赖 clang 工程**)。

---

## 4. 打补丁

两个补丁文件来自 `E:\nabu\state\bridge\answers\T54-patch\`:

```bash
cp kdp-remote-linux.patch processkdp-ub-fix.patch ~/nabu-lldb/
cd ~/nabu-lldb/llvm-project
git apply --check ../kdp-remote-linux.patch    && git apply ../kdp-remote-linux.patch
git apply --check ../processkdp-ub-fix.patch  && git apply ../processkdp-ub-fix.patch
```

sha256(用于核对拿到的是否同一份):
```
fd7fd6714b5e4e92906a6149d8ef28f2fa7758027590ab87b34ab0176ab352a5  kdp-remote-linux.patch
625b1cf7c49a50c16bf32d782f81575d95d14288ccf2ab703ef538d9e3dc6e95  processkdp-ub-fix.patch
```

**如果拿不到我们的补丁文件,也可以手工改**(只改父文件这一处,语义等价):

```diff
--- a/lldb/source/Plugins/Process/CMakeLists.txt
+++ b/lldb/source/Plugins/Process/CMakeLists.txt
@@ -12,8 +12,9 @@
 elseif (CMAKE_SYSTEM_NAME MATCHES "Windows")
   add_subdirectory(Windows/Common)
-elseif (CMAKE_SYSTEM_NAME MATCHES "Darwin")
-  add_subdirectory(MacOSX-Kernel)
 endif()
+if (CMAKE_SYSTEM_NAME MATCHES "Darwin" OR CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
+  add_subdirectory(MacOSX-Kernel)
+endif()
 add_subdirectory(scripted)
```

子文件 `MacOSX-Kernel/CMakeLists.txt` **不用动**【实证:全文无 `CMAKE_SYSTEM_NAME`,LINK_LIBS 已含所需插件】。

---

## 5. 配置 + 构建

```bash
cmake -G Ninja -S llvm -B ~/nabu-lldb/build \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lldb" \
  -DLLVM_TARGETS_TO_BUILD="AArch64;X86" \
  -DLLDB_ENABLE_PYTHON=OFF \
  -DLLDB_ENABLE_CURSES=OFF \
  -DLLDB_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_PARALLEL_COMPILE_JOBS=8 \
  -DLLVM_PARALLEL_LINK_JOBS=1 \
  -DLLVM_USE_LINKER=lld

ninja -C ~/nabu-lldb/build lldb
```

> **坑 2**:不加 `LLVM_INCLUDE_BENCHMARKS=OFF` 会在 `llvm/CMakeLists.txt` 报 `benchmark_main` 目标不存在(稀疏检出不含 third-party)。
> **坑 3**:不加 `LLDB_INCLUDE_TESTS=OFF`,`lldb/test/api/CMakeLists.txt:15` 会要求额外的测试编译器。
> **坑 4**:不加 `LLVM_ENABLE_PROJECTS="clang;lldb"` 而只写 `lldb`,会报 `clang-tablegen-targets`(见坑 1)。

---

## 6. 验证(三条,依次做)

```bash
cd ~/nabu-lldb/build/bin
./lldb --version
#   期望:lldb version 20.1.8 ... revision 87f0227cb601...

strings ../lib/liblldb.so | grep -i kdp-remote
#   期望出现:kdp-remote / kdp-remote <hostname>[:<portnum>] / <lldb.process.kdp-remote.async>

timeout 60 ./lldb --batch -o "log enable lldb process" \
      -o "process connect --plugin kdp-remote udp://127.0.0.1:41139" -o quit
#   期望:error: KDP_REATTACH failed
#         lldb  (plugin = kdp-remote, state = detached)
#   含义:插件已实例化并已按协议发出 REATTACH;本地没有服务端所以失败——这是对的。
```

第三条**不要**因为看到 `failed` 就以为失败;反之如果报 `invalid plugin name` 之类,才是插件没注册成功。

---

## 7. 连真机(部署)

拓扑:**COM 口留在 Windows**,Linux 只连 UDP。

```powershell
# Windows:起网桥(设备侧 debug=0x14e 且不带 0x200;nabu-cdc-rx=1;已进入 KDP)
cd E:\nabu\state\bridge\answers\kdp_net_bridge
python kdp_net_bridge.py --port COM3 --cdc-framing kdpf --udp-mode kdp --verbose
```

> **`--cdc-framing` 必须是 `kdpf`,不是 `serial`。**
>
> 板上 `nabu-kdp-net=1` 生效后,`QcomKdpNet.c` 会用
> `kdp_register_send_receive()` 接管传输层,发的是 **KDPF v1 裸帧**
> (`"KDPF" | ver=1 | flags=0 | len(u16) | crc32(u32) | payload`),
> **没有 0xFA 封装、没有转义**。
>
> 用 `serial` 的后果(2026-11-24 21:2x 实测):
> - TX:网桥把 KDPF 帧包进 `0xFA…CRC…0xFB`,设备侧 `net_feed_byte`
>   找不到 `"KDPF"` magic → **静默丢弃**
> - RX:设备发的 `KDPF…` 没有 `0xFA`,deframer 永远等不到帧头 → **零回复**
>
> 现象是「网桥日志显示写了 N 字节,设备毫无反应」,极难自查。
> 判据:网桥 verbose 日志里必须出现 `wrote … (kdpf frame)`。
>
> `serial` 只在**没有** `nabu-kdp-net=1`(走旧 XNU serial KDP 路径)时才对。
> 本板 boot-args 带 `nabu-kdp-net=1`,所以用 `kdpf`。

```bash
# Linux:取 Windows 主机 IP
HOSTIP=$(ip route | awk '/default/{print $3; exit}')
cd ~/nabu-lldb/build/bin
./lldb -o "process connect --plugin kdp-remote udp://${HOSTIP}:41139" -o "quit"
```

> **坑 5:WSL 里没有 `ip` 命令,`/etc/resolv.conf` 的 nameserver 也不是宿主地址。**
>
> 精简 WSL 镜像常缺 `iproute2`,且 `resolv.conf` 里的
> `nameserver 10.255.255.254` 是 **WSL 的 DNS 代理,UDP 打不通**。
> 必须从 `/proc/net/route` 取 default gateway,该字段是**小端十六进制**:
>
> ```bash
> awk '$2=="00000000"{print $3; exit}' /proc/net/route
> # 01B015AC  ->  按字节倒读  ->  172.21.176.1
> ```
>
> 实测(2026-11-24 23:48):`172.21.176.1` 连通成功,
> `10.255.255.254` 的包网桥根本收不到。
>
> **`awk` 也常缺 `strtonum`,按字节手工倒读最稳。**

- **必须是 `--udp-mode kdp`**(裸 KDP 包形状);用 `eth` 模式 lldb 认不了。
- **加 `--poll 0.01`**(默认 0.1 的串口读超时会占掉每次往返 90 ms)。
  实测:KDP 吞吐 8×16 KiB 从 **14043 ms 降到 2544 ms(5.5 倍)**。
  完整命令:
  ```
  python kdp_net_bridge.py --port COM3 --cdc-framing kdpf --udp-mode kdp --poll 0.01 --verbose
  ```
- Windows 防火墙需放行 UDP 41139。
- 符号:把**与板上实际版本一致**的内核镜像作为目标文件喂给 lldb,否则只有原始内存/寄存器能力。
  - 当前在跑 `t134`,内核 sha16 `e68f924a4caae610`,路径 `E:\nabu\nabu-cdc-20261124\artifacts\kernel\kernel-t134-e68f924a.macho`
  - 回退版 `p0-1`,sha16 `a41eea5e7fb8ea9f`,路径 `E:\nabu\hold\p0-1\kernel.macho`
  - **给错版本的符号,断点和变量地址会全部偏移。** 换内核包后必须重新对一遍 sha。
- 若出现 `invalid reply port` 类错误,说明踩到 §1 提到的 UB 点,请确认用的是修正版 `processkdp-ub-fix.patch` 重编。

---

## 8. 排错速查

| 现象 | 原因 / 处理 |
|---|---|
| 找不到 `cmake/modules/LLVM-Config.cmake` | 稀疏检出缺 `cmake`,补 `git sparse-checkout set llvm lldb clang cmake` |
| `clang-tablegen-targets` 缺失 | `LLVM_ENABLE_PROJECTS` 必须含 `clang` |
| `benchmark_main` 不存在 | 加 `LLVM_INCLUDE_BENCHMARKS=OFF` |
| `lldb/test/api` 要求测试编译器 | 加 `LLDB_INCLUDE_TESTS=OFF` |
| GNU patch 应用补丁报 fuzz/FAILED | 用 `git apply`;或用我们修正过的补丁文件 |
| `KDP_REATTACH failed` 但插件行正确 | **正常**,代表尚无服务端 |
| `invalid plugin name` | 补丁未生效(门控仍关着),回查第 4 节 |
| **网桥写了 N 字节,设备零回应** | **`--cdc-framing` 用了 `serial`。本板带 `nabu-kdp-net=1`,必须是 `kdpf`** |
| **KDP 通了但很慢(约 10 KB/s)** | **缺 `--poll 0.01`。默认 0.1 的读超时吃掉 90 ms/往返** |
| 网桥报 `cannot open COM3` | 控制台还占着口。Windows 串口独占,先 `Stop-Process python` |

---

## 9. 回滚

删除 `~/nabu-lldb` 即可;对设备侧零影响。主机侧停掉网桥即恢复原状。

---

## 10. 本次复现记录(基线)

- 系统:WSL Ubuntu-24.04,12 核 / 19 GB RAM
- 源码:`release/20.x` tip **`87f0227cb60147a26a1eeb4fb06e3b505e9c7261`**
- 结果:`3825/3825` 目标完成,`lldb 20.1.8`,耗时约 25 分钟
- 构建脚本留档:`D:\CatPaw\Projects\e66ae29db0093a49d041383178b34fc2\nabu\_lldb_build.sh`

---

## 11. ★ 真机联调结果(2026-11-24 23:48,已通)

上面 §7 的拓扑**已实测跑通**,下面是真实输出。

### 11.1 连接命令

```bash
GW=172.21.176.1          # WSL 宿主,见 §7 坑 5
cd ~/nabu-lldb/build/bin
./lldb -o "target create /mnt/e/nabu/nabu-cdc-20261124/artifacts/kernel/kernel-t134-e68f924a.macho" \
       -o "process connect --plugin kdp-remote udp://${GW}:41139" \
       -o "bt 5" -o quit
```

### 11.2 符号化内核栈(完整正确)

```
Process 1 stopped
* thread #1, stop reason = signal SIGSTOP
    frame #0: 0xfffffff0072dde40 kernel-t134-e68f924a.macho
      `DebuggerTrapWithState(db_op=DBOP_DEBUGGER, db_message="qcom_usb_debug",
         db_panic_str=0x0, db_panic_args=0x0, db_panic_options=0,
         db_panic_data_ptr=0x0, db_proceed_on_sync_failure=1,
         db_panic_caller=0) at debug.c:732:2
    frame #1: 0xfffffff0072de1bc `DebuggerWithContext(reason=0, ctx=0x0,
         message="qcom_usb_debug", debugger_options_mask=0,
         debugger_caller=18446744005118801936) at debug.c:912:3
    frame #2: 0xfffffff0072ddf2c `Debugger(message=<unavailable>) at debug.c:858:2
    frame #3: 0xfffffff007ac3810 `control_handle_frame(frame=..., status=...,
         generation=...) at QcomXnuUsbDebug.c:2420:9
```

`QcomXnuUsbDebug.c:2420` 正是控制台 `DEBUGGER` verb 里调用
`Debugger("qcom_usb_debug")` 的那行 —— **栈和源码完全对上。**

### 11.3 网桥侧链路证据

```
[DEBUG] UDP->CDC KDP 8 B -> Ethernet frame 50 B
[DEBUG] wrote 62 B to CDC (kdpf frame)
[DEBUG] CDC->UDP KDP 8 B (hdr=816f080000000000)
[DEBUG] sent 8 B to 172.21.185.129:49185
```

lldb 发 8 B 裸 KDP → 网桥补 Ethernet/IP/UDP 头成 50 B → KDPF 封装成 62 B
→ 设备 → 回 8 B → 送回 lldb。**整条链闭合。**

### 11.4 已具备的能力

读内存、设断点、单步、完整符号化栈、寄存器、`frame variable`、
`image lookup -a`。**这是完整的原生内核调试,不再需要插探针。**
