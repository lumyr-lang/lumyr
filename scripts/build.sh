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
if ! command -v flex &> /dev/null; then
    echo "ERROR: flex not found!"
    echo "Please install flex first:"
    echo "  macOS: brew install flex"
    echo "  Debian/Ubuntu: sudo apt install flex"
    exit 1
fi

if ! command -v bison &> /dev/null; then
    echo "ERROR: bison not found!"
    echo "Please install bison first:"
    echo "  macOS: brew install bison"
    echo "  Debian/Ubuntu: sudo apt install bison"
    exit 1
fi

if ! command -v gcc &> /dev/null; then
    echo "ERROR: gcc not found!"
    exit 1
fi

flex --version
bison --version | head -1
gcc --version | head -1
echo ""

# Build
echo "Starting build..."
echo ""
make -B CC=gcc all

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
