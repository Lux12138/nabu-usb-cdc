#!/usr/bin/env bash
# ============================================================================
# nabu · T54 交付 3/4：在 Linux 上构建带 kdp-remote 的 lldb（release/20.x）
#
# 用途：源码 tarball + 两个补丁 -> CMake/Ninja -> lldb（含 kdp-remote 插件）
# 基线：llvm/llvm-project release/20.x（LLVM 20.1.0 tarball）
#
# 预估资源（【推断】，按 Release + ninja、桌面级主机经验值；T54 §4.1 同量级）
#   CPU ：全量构建 0.5 ~ 2 小时（16 核以上取下限附近）
#   内存：编译 job 约 1.5~2 GB/个；链接 liblldb.so 峰值约 4~8 GB
#   磁盘：源码 + 构建产物合计约 30~60 GB（Release、不带测试）
#
# 内存保护建议（脚本已内置，按需调整）
#   1) 限制并行 job：JOBS 默认 min(nproc, 内存GB/2)，且上限 16。
#   2) 链接串行：-DLLVM_PARALLEL_LINK_JOBS=1（liblldb.so 单链接最吃内存）。
#   3) 关掉 Python：-DLLDB_ENABLE_PYTHON=OFF（省 swig/python 依赖与内存；
#      代价见 README「已知风险」——lldbmacros / AppleScript 类插件不可用）。
#      需要 Python 时去掉该行重编即可（增量）。
#   4) 只编 X86(+AArch64) target、关测试，缩小体积。
#   5) 长构建放后台并按 10 秒轮询日志，不要 sleep 死等（见 README）。
#
# 本脚本不在 2026-11-24 的交付运行中执行；仅作为可复现交付物。
# ============================================================================
set -euo pipefail

# ---------------- 可调参数 ----------------
LLVM_VERSION="20.1.0"                 # 对应 release/20.x 的首个正式版；如有 20.1.x 更新版可改
LLVM_TAG="llvmorg-${LLVM_VERSION}"
TARBALL="llvm-project-${LLVM_VERSION}.tar.xz"
DOWNLOAD_URL="https://github.com/llvm/llvm-project/releases/download/${LLVM_TAG}/${TARBALL}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="${ROOT_DIR}/lldb-build"      # 源码与构建都在这里（约 30~60 GB）
SRC_DIR="${WORK_DIR}/llvm-project"
BUILD_DIR="${WORK_DIR}/build"

RUN_DOWNLOAD=1                        # 0=已有源码，跳过下载
JOBS="${JOBS:-0}"                     # 0=自动按内存估算

PATCH_MAIN="${ROOT_DIR}/kdp-remote-linux.patch"
PATCH_UB="${ROOT_DIR}/processkdp-ub-fix.patch"

log() { printf '[%s] %s\n' "$(date '+%H:%M:%S')" "$*"; }

# ---------------- 依赖检查 ----------------
need() { command -v "$1" >/dev/null 2>&1 || { echo "缺少依赖: $1"; exit 1; }; }
need cmake; need ninja; need python3
# 编译器：优先 clang（省内存），没有则 gcc
if command -v clang >/dev/null 2>&1; then
  CC_BIN="$(command -v clang)"; CXX_BIN="$(command -v clang++)"
else
  CC_BIN="$(command -v gcc)"; CXX_BIN="$(command -v g++)"
fi
log "编译器: ${CC_BIN} / ${CXX_BIN}"

# Ubuntu/Debian 缺库时的参考安装命令（按需手工执行，脚本不自动 apt）：
#   sudo apt install -y cmake ninja-build clang lld python3 \
#        libedit-dev libncurses-dev zlib1g-dev liblzma-dev swig

# ---------------- JOB 数（内存保护）----------------
RAM_GB=$(free -g | awk '/^Mem:/{print $2}')          # 不足 1GB 记 0
if [ "${JOBS}" = "0" ]; then
  AUTO=$(( RAM_GB / 2 )); [ "${AUTO}" -lt 1 ] && AUTO=1
  NPROC=$(nproc);      [ "${NPROC}" -lt "${AUTO}" ] && AUTO=${NPROC}
  [ "${AUTO}" -gt 16 ] && AUTO=16
  JOBS=${AUTO}
fi
log "并行 job = ${JOBS}（RAM=${RAM_GB}GB, nproc=$(nproc)）"

mkdir -p "${WORK_DIR}"

# ---------------- 1) 准备源码 ----------------
if [ ! -d "${SRC_DIR}" ]; then
  if [ "${RUN_DOWNLOAD}" = "1" ]; then
    log "下载 tarball（官方 releases，约 150~200 MB）: ${DOWNLOAD_URL}"
    curl -fSL --retry 3 -o "${WORK_DIR}/${TARBALL}" "${DOWNLOAD_URL}"
    # 校验提示：到 https://github.com/llvm/llvm-project/releases/tag/${LLVM_TAG}
    #          对照 sha256（脚本不写死 hash，避免与上游漂移）。
  else
    log "RUN_DOWNLOAD=0：请手动把 tarball 放到 ${WORK_DIR}/${TARBALL}"
  fi
  log "解压源码 ..."
  tar -xf "${WORK_DIR}/${TARBALL}" -C "${WORK_DIR}"
else
  log "复用已有源码树 ${SRC_DIR}"
fi

cd "${SRC_DIR}"

# ---------------- 2) 应用补丁 ----------------
apply_patch() {
  local p="$1"
  log "应用补丁: $(basename "${p}")"
  if git rev-parse --git-dir >/dev/null 2>&1; then
    git apply --check "${p}" && git apply "${p}"
  elif command -v patch >/dev/null 2>&1; then
    patch -p1 --dry-run -f < "${p}" >/dev/null
    patch -p1 -f < "${p}"
  else
    echo "无 git/patch，无法应用 ${p}"; exit 1
  fi
}
apply_patch "${PATCH_MAIN}"
# UB 修复补丁标【推断/待实测】：若你的工具链对 usleep/htons 已无警告，可整文件跳过：
#   apply_patch "${PATCH_UB}"
apply_patch "${PATCH_UB}"

# ---------------- 3) CMake 配置（lldb + clang，clang 是 LLDB 的强制依赖）----------------
# LLVM_ENABLE_PROJECTS 必须含 clang：lldb 的 CMake 引用 clang-tablegen-targets，
# 只写 "lldb" 会在配置期报该目标不存在（T62 §5 坑 4）。
# LLVM_INCLUDE_BENCHMARKS=OFF 必须显式给：稀疏检出不含 third-party/benchmark，
# 否则 llvm/CMakeLists.txt 报 benchmark_main 目标不存在（T62 §5 坑 2）。
log "CMake 配置 ..."
cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="${CC_BIN}" \
  -DCMAKE_CXX_COMPILER="${CXX_BIN}" \
  -DLLVM_ENABLE_PROJECTS="clang;lldb" \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_PARALLEL_COMPILE_JOBS="${JOBS}" \
  -DLLVM_PARALLEL_LINK_JOBS=1 \
  -DLLDB_ENABLE_PYTHON=OFF \
  -DLLDB_ENABLE_CURSES=OFF \
  -DLLDB_ENABLE_LIBEDIT=OFF \
  -DLLDB_ENABLE_LZMA=OFF \
  -DLLDB_INCLUDE_TESTS=OFF

# ---------------- 4) 构建 ----------------
# 建议放后台并 10 秒轮询：
#   nohup ./build-lldb-linux.sh > build.log 2>&1 &
#   tail -f build.log   # 或每 10 秒查看 ninja 进度行
log "开始构建（ninja lldb, -j${JOBS}；链接串行）..."
cmake --build "${BUILD_DIR}" --target lldb -j "${JOBS}"

# ---------------- 5) 自检 ----------------
LLDB_SO="$(find "${BUILD_DIR}" -name 'liblldb.so*' -type f | head -n1 || true)"
log "liblldb: ${LLDB_SO:-未找到}"
if [ -n "${LLDB_SO}" ]; then
  log "判据 A：插件注册串 kdp-remote / darwin-kernel"
  strings "${LLDB_SO}" | grep -i -m5 -e 'kdp-remote' -e 'darwin-kernel' || true
fi
"${BUILD_DIR}/bin/lldb" --version || true

log "完成。连接设备："
log "  ${BUILD_DIR}/bin/lldb -o 'process connect --plugin kdp-remote udp://127.0.0.1:41139'"
