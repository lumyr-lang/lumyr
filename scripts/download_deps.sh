#!/bin/bash
# ============================================================
# download_deps.sh - Download Lumyr compiler dependencies
# macOS / Linux version
# ============================================================

set -e

echo "============================================================"
echo "Downloading Lumyr compiler dependencies..."
echo "============================================================"
echo ""

# Create directories
mkdir -p vendor
mkdir -p build_tools

# ========== vendor directory (third-party library source code) ==========

echo "[1/7] Downloading libcurl 8.22.0..."
if [ ! -d "vendor/libcurl" ]; then
    curl -L -o vendor/curl-8.22.0.tar.gz https://curl.se/download/curl-8.22.0.tar.gz
    cd vendor
    tar -xzf curl-8.22.0.tar.gz
    mv curl-8.22.0 libcurl
    rm curl-8.22.0.tar.gz
    cd ..
    echo "  Done."
else
    echo "  Already cached, skipping download, next..."
fi
echo ""

echo "[2/7] Downloading libiconv 1.17..."
if [ ! -d "vendor/libiconv" ]; then
    curl -L -o vendor/libiconv-1.17.tar.gz https://ftp.gnu.org/gnu/libiconv/libiconv-1.17.tar.gz
    cd vendor
    tar -xzf libiconv-1.17.tar.gz
    mv libiconv-1.17 libiconv
    rm libiconv-1.17.tar.gz
    cd ..
    echo "  Done."
else
    echo "  Already cached, skipping download, next..."
fi
echo ""

echo "[3/7] Downloading TRE 0.9.0..."
if [ ! -d "vendor/tre" ]; then
    curl -L -o vendor/tre-0.9.0.zip https://github.com/laurikari/tre/archive/refs/tags/v0.9.0.zip
    cd vendor
    unzip -q tre-0.9.0.zip
    mv tre-0.9.0 tre
    rm tre-0.9.0.zip
    cd ..
    echo "  Done."
else
    echo "  Already cached, skipping download, next..."
fi
echo ""

echo "[4/7] Downloading GMP 6.3.0..."
if [ ! -d "vendor/gmp" ]; then
    curl -L -o vendor/gmp-6.3.0.tar.gz https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.gz
    cd vendor
    tar -xzf gmp-6.3.0.tar.gz
    mv gmp-6.3.0 gmp
    rm gmp-6.3.0.tar.gz
    cd ..
    echo "  Done."
else
    echo "  Already cached, skipping download, next..."
fi
echo ""

# ========== build_tools directory (build toolchain source code) ==========

echo "[5/7] Downloading flex 2.6.4..."
if [ ! -d "build_tools/flex" ]; then
    curl -L -o build_tools/flex-2.6.4.tar.gz https://github.com/westes/flex/releases/download/v2.6.4/flex-2.6.4.tar.gz
    cd build_tools
    tar -xzf flex-2.6.4.tar.gz
    mv flex-2.6.4 flex
    rm flex-2.6.4.tar.gz
    cd ..
    echo "  Done."
else
    echo "  Already cached, skipping download, next..."
fi
echo ""

echo "[6/7] Downloading bison 3.8.2..."
if [ ! -d "build_tools/bison" ]; then
    curl -L -o build_tools/bison-3.8.2.tar.gz https://ftp.gnu.org/gnu/bison/bison-3.8.2.tar.gz
    cd build_tools
    tar -xzf bison-3.8.2.tar.gz
    mv bison-3.8.2 bison
    rm bison-3.8.2.tar.gz
    cd ..
    echo "  Done."
else
    echo "  Already cached, skipping download, next..."
fi
echo ""

echo "[7/7] Downloading m4 1.4.19..."
if [ ! -d "build_tools/m4" ]; then
    curl -L -o build_tools/m4-1.4.19.tar.gz https://ftp.gnu.org/gnu/m4/m4-1.4.19.tar.gz
    cd build_tools
    tar -xzf m4-1.4.19.tar.gz
    mv m4-1.4.19 m4
    rm m4-1.4.19.tar.gz
    cd ..
    echo "  Done."
else
    echo "  Already cached, skipping download, next..."
fi
echo ""

echo "============================================================"
echo "All dependencies downloaded successfully!"
echo "============================================================"
echo ""
echo "Directory structure:"
echo "  vendor/         - Third-party library source code"
echo "  build_tools/    - Build toolchain source code"
echo "  deps/gmp/       - GMP headers and prebuilt libraries"
echo "  prebuilt/       - Prebuilt binaries (Windows only)"
echo ""
echo "Next steps:"
echo "  1. Read vendor/README.md and build_tools/README.md for build instructions"
echo "  2. Read deps/gmp/README.md for GMP build instructions"
echo "  3. Or use prebuilt/windows/ for quick start on Windows"
echo ""
