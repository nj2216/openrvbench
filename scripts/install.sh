#!/usr/bin/env bash
# ─── OpenRVBench installer ────────────────────────────────────────────────────
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/YOUR_USERNAME/openrvbench/main/scripts/install.sh | bash
#   or with a specific version:
#   curl -fsSL ... | bash -s -- --version v1.0.0
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO="YOUR_USERNAME/openrvbench"
INSTALL_DIR="/usr/local"
VERSION="latest"

# Parse args
while [[ $# -gt 0 ]]; do
  case $1 in
    --version) VERSION="$2"; shift ;;
    --prefix)  INSTALL_DIR="$2"; shift ;;
    *) echo "Unknown option: $1" ;;
  esac
  shift
done

# ─── ANSI ─────────────────────────────────────────────────────────────────────
GRN='\033[0;32m'; CYN='\033[0;36m'; RED='\033[0;31m'; BLD='\033[1m'; RST='\033[0m'
info() { echo -e "${CYN}${BLD}[openrvbench]${RST} $*"; }
ok()   { echo -e "${GRN}${BLD}[openrvbench]${RST} $*"; }
die()  { echo -e "${RED}${BLD}[openrvbench]${RST} $*" >&2; exit 1; }

# ─── Check arch ───────────────────────────────────────────────────────────────
ARCH=$(uname -m)
[[ "$ARCH" != "riscv64" ]] && die "This installer is for RISC-V 64-bit Linux only (detected: $ARCH)"

# ─── Check dependencies ───────────────────────────────────────────────────────
for tool in curl tar python3; do
  command -v "$tool" &>/dev/null || die "$tool is required but not installed"
done

# ─── Resolve version ──────────────────────────────────────────────────────────
if [[ "$VERSION" == "latest" ]]; then
  info "Fetching latest release version..."
  VERSION=$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" \
    | python3 -c "import sys,json; print(json.load(sys.stdin)['tag_name'])")
  info "Latest: $VERSION"
fi

# ─── Download ─────────────────────────────────────────────────────────────────
VER_NUM="${VERSION#v}"
TARBALL="openrvbench-${VER_NUM}-rv64gc-linux.tar.gz"
URL="https://github.com/${REPO}/releases/download/${VERSION}/${TARBALL}"

info "Downloading $TARBALL..."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

curl -fsSL --progress-bar "$URL" -o "$TMP/$TARBALL" \
  || die "Download failed. Check that $VERSION exists at github.com/${REPO}/releases"

# ─── Extract ──────────────────────────────────────────────────────────────────
info "Extracting..."
tar -xzf "$TMP/$TARBALL" -C "$TMP"
EXTRACTED=$(find "$TMP" -maxdepth 1 -type d -name "openrvbench-*" | head -1)
[[ -z "$EXTRACTED" ]] && die "Could not find extracted directory"

# ─── Install ──────────────────────────────────────────────────────────────────
info "Installing to $INSTALL_DIR..."
mkdir -p "$INSTALL_DIR/bin" "$INSTALL_DIR/lib/openrvbench/modules"

install -m 755 "$EXTRACTED/bin/openrvbench"   "$INSTALL_DIR/bin/openrvbench"

for bench in "$EXTRACTED/lib/openrvbench/modules/"bench_*; do
  [[ -f "$bench" ]] && install -m 755 "$bench" "$INSTALL_DIR/lib/openrvbench/modules/"
done

# ─── Verify ───────────────────────────────────────────────────────────────────
"$INSTALL_DIR/bin/openrvbench" info > /dev/null 2>&1 \
  || die "Installation verification failed"

ok "OpenRVBench ${VERSION} installed successfully!"
echo ""
echo "  Run:  openrvbench run all"
echo "  Info: openrvbench info"
echo ""
echo "  If not in PATH: export PATH=\$PATH:$INSTALL_DIR/bin"
