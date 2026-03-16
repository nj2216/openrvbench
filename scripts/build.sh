#!/usr/bin/env bash
# ─── OpenRVBench :: Build & Install Script ───────────────────────────────────
# Builds all benchmark binaries and installs the CLI.
#
# Usage: ./scripts/build.sh [options]
#
# Compiler options (highest priority first):
#   --compiler PROG   Derive both CC and CXX from one name, e.g.
#                       --compiler riscv64-linux-gnu-gcc
#                       --compiler /opt/riscv/bin/riscv64-unknown-linux-gnu-gcc
#                       --compiler gcc-13
#                       --compiler clang-17
#   --cc  PROG        Set C   compiler explicitly
#   --cxx PROG        Set C++ compiler explicitly
#   CC=/path CXX=/path ./build.sh   (standard env-var approach)
#   (no flags)        Auto-detect: tries RISC-V cross-compilers first,
#                     then versioned native GCC, then plain gcc/g++
#
# Other options:
#   --with-rvv        Force-enable RVV (default: auto-detect via compiler probe)
#   --no-rvv          Disable RVV
#   --with-ai         Build AI benchmark (llama.cpp / ONNX Runtime optional)
#   --release         Full -O3 + LTO build (default: -O2 RelWithDebInfo)
#   --jobs N          Parallel jobs (default: nproc)
#   --prefix PATH     Install prefix (default: /usr/local)
#   --clean           Wipe build directory before configuring
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ─── Defaults ─────────────────────────────────────────────────────────────────
BUILD_DIR="$ROOT_DIR/build"
PREFIX="/usr/local"
JOBS=$(nproc 2>/dev/null || echo 4)
ENABLE_RVV=ON
ENABLE_AI=OFF
RELEASE=OFF
CLEAN=OFF
USER_CC=""
USER_CXX=""

# ─── ANSI helpers ─────────────────────────────────────────────────────────────
GRN='\033[0;32m'; YLW='\033[0;33m'; RED='\033[0;31m'
CYN='\033[0;36m'; BLD='\033[1m';    RST='\033[0m'

info()  { echo -e "${CYN}${BLD}[INFO]${RST}  $*"; }
ok()    { echo -e "${GRN}${BLD}[ OK ]${RST}  $*"; }
warn()  { echo -e "${YLW}${BLD}[WARN]${RST}  $*"; }
error() { echo -e "${RED}${BLD}[ERR ]${RST}  $*" >&2; exit 1; }

# ─── Argument parser ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case $1 in
    --with-rvv)   ENABLE_RVV=ON  ;;
    --no-rvv)     ENABLE_RVV=OFF ;;
    --with-ai)    ENABLE_AI=ON   ;;
    --release)    RELEASE=ON     ;;
    --clean)      CLEAN=ON       ;;
    --jobs)       JOBS="$2";    shift ;;
    --prefix)     PREFIX="$2";  shift ;;

    # --compiler derives both CC and CXX from one program name.
    # Substitution rules (in order):
    #   *-gcc  →  *-g++   (cross-compiler prefix preserved)
    #   gcc-N  →  g++-N   (versioned native GCC)
    #   clang  →  clang++ / clang-N → clang++-N
    --compiler)
      RAW="$2"; shift
      USER_CC="$RAW"
      # 1. gcc → g++ (handles both bare gcc and prefixed cross-compilers)
      DERIVED="${RAW//gcc/g++}"
      if [[ "$DERIVED" != "$RAW" ]]; then
        USER_CXX="$DERIVED"
      else
        # 2. clang → clang++ (guard against double-apply)
        DERIVED="${RAW/clang/clang++}"
        DERIVED="${DERIVED//clang++++/clang++}"
        USER_CXX="$DERIVED"
      fi
      ;;

    --cc)   USER_CC="$2";  shift ;;
    --cxx)  USER_CXX="$2"; shift ;;

    *) warn "Unknown option: $1" ;;
  esac
  shift
done

# ─── Compiler resolution ──────────────────────────────────────────────────────
# Returns first argument that resolves as an executable in PATH or an
# absolute/relative path to an existing file.
first_available() {
  for candidate in "$@"; do
    if command -v "$candidate" &>/dev/null 2>&1; then
      echo "$candidate"; return
    fi
  done
  echo ""
}

# CLI flags > env vars > auto-detect
[[ -z "$USER_CC"  ]] && USER_CC="${CC:-}"
[[ -z "$USER_CXX" ]] && USER_CXX="${CXX:-}"

if [[ -z "$USER_CC" ]]; then
  USER_CC="$(first_available \
    riscv64-linux-gnu-gcc \
    riscv64-unknown-linux-gnu-gcc \
    riscv64-unknown-elf-gcc \
    gcc-14 gcc-13 gcc-12 gcc-11 \
    gcc clang)"
fi
if [[ -z "$USER_CXX" ]]; then
  USER_CXX="$(first_available \
    riscv64-linux-gnu-g++ \
    riscv64-unknown-linux-gnu-g++ \
    riscv64-unknown-elf-g++ \
    g++-14 g++-13 g++-12 g++-11 \
    g++ clang++)"
fi

[[ -z "$USER_CC"  ]] && error "No C compiler found.   Use --cc /path/to/gcc  or set CC=..."
[[ -z "$USER_CXX" ]] && error "No C++ compiler found. Use --cxx /path/to/g++ or set CXX=..."

# Resolve to absolute paths so CMake always gets /usr/bin/gcc-14 style paths,
# never a bare name that could resolve differently inside CMake's environment.
ABS_CC="$(command  -v "$USER_CC"  2>/dev/null || echo "$USER_CC")"
ABS_CXX="$(command -v "$USER_CXX" 2>/dev/null || echo "$USER_CXX")"

# ─── Banner ───────────────────────────────────────────────────────────────────
echo ""
echo -e "${CYN}${BLD}"
echo "  ╔══════════════════════════════════════════╗"
echo "  ║  OpenRVBench Build System v1.0.1         ║"
echo "  ╚══════════════════════════════════════════╝"
echo -e "${RST}"

# ─── Requirements check ───────────────────────────────────────────────────────
info "Checking build requirements..."

for var_name in USER_CC USER_CXX; do
  prog="${!var_name}"
  if command -v "$prog" &>/dev/null 2>&1; then
    full_path="$(command -v "$prog")"
    version="$("$prog" --version 2>&1 | head -1)"
    ok "$var_name → $full_path  ($version)"
  else
    error "$var_name compiler not found: '$prog'\n       Install it or use --compiler / --cc / --cxx to pick another"
  fi
done

for tool in cmake python3; do
  if command -v "$tool" &>/dev/null; then
    ok "$tool: $(command -v "$tool")"
  else
    error "$tool not found. Install: sudo apt install cmake python3"
  fi
done

CMAKE_VER=$(cmake --version | head -1 | awk '{print $3}')
info "CMake version: $CMAKE_VER"

# ─── Architecture detection ───────────────────────────────────────────────────
ARCH=$(uname -m)
if [[ "$ARCH" == "riscv64" ]]; then
  ok "Native RISC-V 64-bit build"
  IS_RISCV=true
else
  warn "Host is $ARCH — cross-compiling with $USER_CXX"
  IS_RISCV=false
fi

# ─── RVV probe (uses the chosen compiler) ─────────────────────────────────────
if [[ "$ENABLE_RVV" == "ON" ]]; then
  info "Probing $ABS_CXX for -march=rv64gcv support..."
  if echo 'int main(){}' | "$ABS_CXX" -x c++ - -march=rv64gcv -o /dev/null 2>/dev/null; then
    ok "RVV supported by $ABS_CXX"
  else
    warn "$ABS_CXX does not support -march=rv64gcv — disabling RVV"
    ENABLE_RVV=OFF
  fi
fi

# ─── Configuration summary ────────────────────────────────────────────────────
echo ""
info "Build configuration:"
echo "  CC           : $ABS_CC"
echo "  CXX          : $ABS_CXX"
echo "  Build dir    : $BUILD_DIR"
echo "  Install dir  : $PREFIX"
echo "  Parallel jobs: $JOBS"
echo "  RVV          : $ENABLE_RVV"
echo "  AI bench     : $ENABLE_AI"
echo "  Release mode : $RELEASE"
echo ""

# ─── Clean ────────────────────────────────────────────────────────────────────
if [[ "$CLEAN" == "ON" && -d "$BUILD_DIR" ]]; then
  info "Cleaning build directory..."
  rm -rf "$BUILD_DIR"
  ok "Build directory cleaned"
fi

mkdir -p "$BUILD_DIR"

# ─── Stale cache detection ────────────────────────────────────────────────────
# If CMakeCache.txt records a different C++ compiler than we're about to use,
# wipe just the cache file (not the whole build dir) so CMake starts fresh.
# This avoids the "you have changed variables" error without a full --clean.
CACHE_FILE="$BUILD_DIR/CMakeCache.txt"
if [[ -f "$CACHE_FILE" ]]; then
  CACHED_CXX="$(grep -m1 '^CMAKE_CXX_COMPILER:' "$CACHE_FILE"                 | cut -d= -f2 | tr -d '[:space:]')"
  if [[ -n "$CACHED_CXX" && "$CACHED_CXX" != "$ABS_CXX" ]]; then
    warn "Compiler changed: cache has '$CACHED_CXX', now using '$ABS_CXX'"
    info "Auto-wiping CMakeCache.txt and CMakeFiles/ to avoid stale-cache errors..."
    rm -f  "$CACHE_FILE"
    rm -rf "$BUILD_DIR/CMakeFiles"
    ok "Cache cleared — full re-configure will run"
  fi
fi

# ─── CMake configure ──────────────────────────────────────────────────────────
info "Configuring with CMake..."

BUILD_TYPE=$([ "$RELEASE" == "ON" ] && echo "Release" || echo "RelWithDebInfo")

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
  -DCMAKE_C_COMPILER="$ABS_CC"
  -DCMAKE_CXX_COMPILER="$ABS_CXX"
  -DENABLE_RVV="$ENABLE_RVV"
  -DENABLE_OPENMP=ON
  -DENABLE_AI_BENCH="$ENABLE_AI"
  -DENABLE_NETWORK_BENCH=ON
)

# Tell CMake not to try to execute target binaries when cross-compiling
if [[ "$IS_RISCV" == "false" ]]; then
  CMAKE_ARGS+=(
    -DCMAKE_CROSSCOMPILING=TRUE
    -DCMAKE_SYSTEM_NAME=Linux
    -DCMAKE_SYSTEM_PROCESSOR=riscv64
  )
fi

cd "$BUILD_DIR"
cmake "${CMAKE_ARGS[@]}" "$ROOT_DIR" || error "CMake configuration failed"
ok "CMake configuration successful"

# ─── Build ────────────────────────────────────────────────────────────────────
info "Building with $ABS_CXX ($JOBS parallel jobs)..."
make -j"$JOBS" 2>&1 | tee /tmp/openrvbench_build.log
if [[ "${PIPESTATUS[0]}" -ne 0 ]]; then
  error "Build failed. Full log: /tmp/openrvbench_build.log"
fi
ok "Build complete"

# ─── Verify binaries ──────────────────────────────────────────────────────────
echo ""
info "Verifying binaries..."
FOUND=0
for bench in cpu memory crypto storage network thermal; do
  BIN=$(find "$BUILD_DIR" -name "bench_$bench" -type f 2>/dev/null | head -1)
  if [[ -n "$BIN" ]]; then
    ok "bench_$bench ($(du -h "$BIN" | cut -f1))"
    ((FOUND++)) || true
  else
    warn "bench_$bench: NOT FOUND"
  fi
done

for bench in vector ai; do
  BIN=$(find "$BUILD_DIR" -name "bench_$bench" -type f 2>/dev/null | head -1)
  if [[ -n "$BIN" ]]; then
    ok "bench_$bench ($(du -h "$BIN" | cut -f1), optional)"
    ((FOUND++)) || true
  else
    info "bench_$bench: not built (optional)"
  fi
done

echo ""
ok "$FOUND benchmark binaries built"

# ─── Install CLI ──────────────────────────────────────────────────────────────
echo ""
info "Installing CLI to $PREFIX/bin..."
mkdir -p "$PREFIX/bin"
cp "$ROOT_DIR/cli/openrvbench" "$PREFIX/bin/openrvbench"
chmod +x "$PREFIX/bin/openrvbench"
ok "CLI installed: $PREFIX/bin/openrvbench"

# ─── Install modules ──────────────────────────────────────────────────────────
info "Setting up module directory: $PREFIX/lib/openrvbench/modules..."
MODULE_DIR="$PREFIX/lib/openrvbench/modules"
mkdir -p "$MODULE_DIR"
for bench in cpu memory crypto storage network thermal vector ai; do
  BIN=$(find "$BUILD_DIR" -name "bench_$bench" -type f 2>/dev/null | head -1)
  if [[ -n "$BIN" ]]; then
    cp "$BIN" "$MODULE_DIR/bench_$bench"
    ok "Installed bench_$bench → $MODULE_DIR/"
  fi
done

# ─── Done ─────────────────────────────────────────────────────────────────────
echo ""
echo -e "${GRN}${BLD}════════════════════════════════════════════${RST}"
echo -e "${GRN}${BLD}  Build & Install Complete!${RST}"
echo -e "${GRN}${BLD}════════════════════════════════════════════${RST}"
echo ""
echo "  Compiler used  : $ABS_CXX"
echo "  RVV enabled    : $ENABLE_RVV"
echo ""
echo "  Run benchmarks:"
echo -e "    ${CYN}openrvbench run all${RST}"
echo -e "    ${CYN}openrvbench run cpu${RST}"
echo -e "    ${CYN}openrvbench info${RST}"
echo ""
echo "  If openrvbench is not in PATH:"
echo -e "    ${CYN}export PATH=\$PATH:$PREFIX/bin${RST}"
echo ""
