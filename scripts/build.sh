#!/usr/bin/env bash
# ─── OpenRVBench :: Build & Install Script ───────────────────────────────────
# Builds all benchmark binaries and installs the CLI.
# Usage: ./scripts/build.sh [options]
#
# Options:
#   --with-rvv        Enable RISC-V Vector Extension (default: auto-detect)
#   --with-openmp     Enable OpenMP (default: auto-detect)
#   --with-ai         Build AI benchmark (requires llama.cpp / ONNX Runtime)
#   --release         Full optimisation build (default: -O2)
#   --jobs N          Parallel jobs (default: nproc)
#   --prefix PATH     Install prefix (default: /usr/local)
#   --clean           Clean build directory first
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ─── Defaults ────────────────────────────────────────────────────────────────
BUILD_DIR="$ROOT_DIR/build"
PREFIX="/usr/local"
JOBS=$(nproc 2>/dev/null || echo 4)
ENABLE_RVV=ON
ENABLE_AI=OFF
RELEASE=OFF
CLEAN=OFF

# ─── ANSI ────────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; YLW='\033[0;33m'; RED='\033[0;31m'
CYN='\033[0;36m'; BLD='\033[1m';    RST='\033[0m'

info()  { echo -e "${CYN}${BLD}[INFO]${RST}  $*"; }
ok()    { echo -e "${GRN}${BLD}[ OK ]${RST}  $*"; }
warn()  { echo -e "${YLW}${BLD}[WARN]${RST}  $*"; }
error() { echo -e "${RED}${BLD}[ERR ]${RST}  $*" >&2; exit 1; }

# ─── Parse args ──────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case $1 in
    --with-rvv)     ENABLE_RVV=ON  ;;
    --no-rvv)       ENABLE_RVV=OFF ;;
    --with-ai)      ENABLE_AI=ON   ;;
    --release)      RELEASE=ON     ;;
    --clean)        CLEAN=ON       ;;
    --jobs)         JOBS="$2"; shift ;;
    --prefix)       PREFIX="$2"; shift ;;
    *) warn "Unknown option: $1" ;;
  esac
  shift
done

# ─── Banner ──────────────────────────────────────────────────────────────────
echo ""
echo -e "${CYN}${BLD}"
echo "  ╔══════════════════════════════════════════╗"
echo "  ║  OpenRVBench Build System v1.0           ║"
echo "  ╚══════════════════════════════════════════╝"
echo -e "${RST}"

# ─── Requirements check ──────────────────────────────────────────────────────
info "Checking build requirements..."

for tool in cmake make g++ python3; do
  if command -v "$tool" &>/dev/null; then
    ok "$tool: $(command -v $tool)"
  else
    error "$tool not found. Install: sudo apt install cmake build-essential python3"
  fi
done

# CMake version
CMAKE_VER=$(cmake --version | head -1 | awk '{print $3}')
info "CMake version: $CMAKE_VER"

# RISC-V detection
ARCH=$(uname -m)
if [[ "$ARCH" == "riscv64" ]]; then
  ok "RISC-V 64-bit detected"
  IS_RISCV=true
else
  warn "Not running on RISC-V ($ARCH) — simulation/cross-compile mode"
  IS_RISCV=false
fi

# Check RVV compiler support
if [[ "$ENABLE_RVV" == "ON" ]]; then
  if echo 'int main(){}' | g++ -x c++ - -march=rv64gcv -o /dev/null 2>/dev/null; then
    ok "RVV compiler support confirmed"
  else
    warn "Compiler doesn't support -march=rv64gcv — disabling RVV"
    ENABLE_RVV=OFF
  fi
fi

echo ""
info "Build configuration:"
echo "  Build dir    : $BUILD_DIR"
echo "  Install dir  : $PREFIX"
echo "  Parallel jobs: $JOBS"
echo "  RVV          : $ENABLE_RVV"
echo "  AI bench     : $ENABLE_AI"
echo "  Release mode : $RELEASE"
echo ""

# ─── Clean ───────────────────────────────────────────────────────────────────
if [[ "$CLEAN" == "ON" && -d "$BUILD_DIR" ]]; then
  info "Cleaning build directory..."
  rm -rf "$BUILD_DIR"
  ok "Build directory cleaned"
fi

mkdir -p "$BUILD_DIR"

# ─── CMake configure ─────────────────────────────────────────────────────────
info "Configuring with CMake..."

CMAKE_ARGS=(
  -DCMAKE_BUILD_TYPE=$([ "$RELEASE" == "ON" ] && echo "Release" || echo "RelWithDebInfo")
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
  -DENABLE_RVV="$ENABLE_RVV"
  -DENABLE_OPENMP=ON
  -DENABLE_AI_BENCH="$ENABLE_AI"
  -DENABLE_NETWORK_BENCH=ON
)

cd "$BUILD_DIR"
cmake "${CMAKE_ARGS[@]}" "$ROOT_DIR" || error "CMake configuration failed"
ok "CMake configuration successful"

# ─── Build ───────────────────────────────────────────────────────────────────
info "Building ($JOBS parallel jobs)..."
make -j"$JOBS" 2>&1 | tee /tmp/openrvbench_build.log
if [[ "${PIPESTATUS[0]}" -ne 0 ]]; then
  error "Build failed. See /tmp/openrvbench_build.log"
fi
ok "Build complete"

# ─── Verify binaries ─────────────────────────────────────────────────────────
echo ""
info "Verifying binaries..."
FOUND=0
MISSING=0
for bench in cpu memory crypto storage network thermal; do
  BIN=$(find "$BUILD_DIR" -name "bench_$bench" -type f 2>/dev/null | head -1)
  if [[ -n "$BIN" ]]; then
    SIZE=$(du -h "$BIN" | cut -f1)
    ok "bench_$bench ($SIZE)"
    ((FOUND++)) || true
  else
    warn "bench_$bench: NOT FOUND"
    ((MISSING++)) || true
  fi
done

for bench in vector ai; do
  BIN=$(find "$BUILD_DIR" -name "bench_$bench" -type f 2>/dev/null | head -1)
  if [[ -n "$BIN" ]]; then
    SIZE=$(du -h "$BIN" | cut -f1)
    ok "bench_$bench ($SIZE, optional)"
    ((FOUND++)) || true
  else
    info "bench_$bench: not built (optional)"
  fi
done

echo ""
ok "$FOUND benchmark binaries built"

# ─── Install CLI ─────────────────────────────────────────────────────────────
echo ""
info "Installing CLI to $PREFIX/bin..."
mkdir -p "$PREFIX/bin"
cp "$ROOT_DIR/cli/openrvbench" "$PREFIX/bin/openrvbench"
chmod +x "$PREFIX/bin/openrvbench"
ok "CLI installed: $PREFIX/bin/openrvbench"

# ─── Create symlinks for benchmark modules ───────────────────────────────────
info "Setting up benchmark module directory..."
MODULE_DIR="$PREFIX/lib/openrvbench/modules"
mkdir -p "$MODULE_DIR"
for bench in cpu memory crypto storage network thermal vector ai; do
  BIN=$(find "$BUILD_DIR" -name "bench_$bench" -type f 2>/dev/null | head -1)
  if [[ -n "$BIN" ]]; then
    cp "$BIN" "$MODULE_DIR/bench_$bench"
    ok "Installed bench_$bench → $MODULE_DIR/"
  fi
done

# ─── Post-install summary ─────────────────────────────────────────────────────
echo ""
echo -e "${GRN}${BLD}════════════════════════════════════════════${RST}"
echo -e "${GRN}${BLD}  Build & Install Complete!${RST}"
echo -e "${GRN}${BLD}════════════════════════════════════════════${RST}"
echo ""
echo "  Run benchmarks:"
echo -e "    ${CYN}openrvbench run all${RST}"
echo -e "    ${CYN}openrvbench run cpu${RST}"
echo -e "    ${CYN}openrvbench info${RST}"
echo ""
echo "  If openrvbench is not in PATH:"
echo -e "    ${CYN}export PATH=\$PATH:$PREFIX/bin${RST}"
echo ""
