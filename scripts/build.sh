#!/bin/bash
# ============================================================
# build.sh - Build Lumyr compiler
# macOS / Linux version
# ============================================================

set -e

echo "============================================================"
echo "Building Lumyr compiler..."
echo "============================================================"
echo ""

# Check if dependencies are installed
echo "Checking toolchain..."

if ! command -v cc >/dev/null 2>&1 && ! command -v gcc >/dev/null 2>&1; then
    echo "ERROR: no C compiler found!"
    exit 1
fi

# GMP：唯一需要额外安装的第三方运行库（macOS brew / Linux 发行版）
if [ "$(uname -s)" = "Darwin" ]; then
    if ! ls /usr/local/opt/gmp/lib/libgmp.10.dylib >/dev/null 2>&1 && \
       ! ls /opt/homebrew/opt/gmp/lib/libgmp.10.dylib >/dev/null 2>&1; then
        echo "ERROR: GMP not found. Run: brew install gmp"
        exit 1
    fi
    # bison/flex 若未装则提示（Makefile 会自动定位 brew 版本）
    ls /usr/local/opt/bison/bin/bison >/dev/null 2>&1 || ls /opt/homebrew/opt/bison/bin/bison >/dev/null 2>&1 \
        || echo "HINT: bison >=3.x needed, run: brew install bison"
    ls /usr/local/opt/flex/bin/flex >/dev/null 2>&1 || ls /opt/homebrew/opt/flex/bin/flex >/dev/null 2>&1 \
        || echo "HINT: flex needed, run: brew install flex"
else
    if ! echo 'int main(){}' | ${CC:-cc} -lgmp -x c - -o /dev/null 2>/dev/null; then
        echo "ERROR: GMP not found. Debian/Ubuntu: sudo apt install libgmp-dev"
        exit 1
    fi
    command -v bison >/dev/null 2>&1 || { echo "ERROR: bison needed (sudo apt install bison)"; exit 1; }
    command -v flex  >/dev/null 2>&1 || { echo "ERROR: flex needed (sudo apt install flex)"; exit 1; }
fi
echo ""

# Build
echo "Starting build..."
echo ""
make -B all

echo ""
echo "============================================================"
echo "Build completed successfully!"
echo "============================================================"
echo ""
echo "Executable: bin/lumyr"
echo ""
echo "Usage:"
echo "  VM mode:     bin/lumyr <file.lm>"
echo "  CC mode:     bin/lumyr -c <file.lm>"
echo ""
