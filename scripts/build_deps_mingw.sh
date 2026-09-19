#!/usr/bin/env bash
# ============================================================
# build_deps_mingw.sh - Windows (MinGW-w64) 运行时依赖源码构建
#
# 与 macOS/Linux 使用同一套源码版本：
#   TRE 0.9.0      静态   (BSD-2)
#   libiconv 1.17  动态   (LGPL-2.1，静态链接不合规)
#   libcurl 8.22.0 静态   (MIT/X，TLS 用 Windows 原生 Schannel)
#   GMP 6.3.0      动态   (LGPLv3，静态链接不合规)
#
# 运行环境：MSYS2 的 "MSYS2 MINGW64" 终端（x64；32 位用 MINGW32 终端）
#   首次使用先装工具链：
#     pacman -S --needed base-devel mingw-w64-x86_64-toolchain autoconf automake libtool
#   x86 (32位) 构建安装 mingw-w64-i686-toolchain 并在 MINGW32 终端运行。
#
# 用法：
#   bash scripts/build_deps_mingw.sh
# ============================================================
set -e

cd "$(dirname "$0")/.."
ROOT=$(pwd)
VENDOR="$ROOT/scripts/vendor"
PREFIX="$ROOT/prebuilt/windows"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

# ---------- 环境检查 ----------
case "$(uname -s)" in
    MINGW*|MSYS*) ;;
    *) echo "ERROR: 此脚本必须在 MSYS2/MinGW 环境运行（当前 $(uname -s)）"; exit 1 ;;
esac

for tool in gcc make autoreconf; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ERROR: 未找到 $tool。请在 MSYS2 MINGW64 终端执行："
        echo "  pacman -S --needed base-devel mingw-w64-x86_64-toolchain autoconf automake libtool"
        exit 1
    fi
done
case "$(gcc -dumpmachine)" in
    *mingw32) ;;
    *) echo "WARNING: gcc 目标为 $(gcc -dumpmachine)，预期 *-w64-mingw32，产物可能不是原生 Windows 库" ;;
esac

mkdir -p "$PREFIX"
echo "前缀: $PREFIX"
echo ""

# ---------- 1. TRE 0.9.0（静态） ----------
if [ -f "$PREFIX/lib/libtre.a" ]; then
    echo "[1/3] TRE already built, skipping."
else
    echo "[1/3] Building TRE 0.9.0 (static)..."
    cd "$VENDOR/tre"
    autoreconf -fi
    make distclean >/dev/null 2>&1 || true
    ./configure --prefix="$PREFIX" --disable-shared --enable-static
    make -j"$JOBS"
    make install
    cd "$ROOT"
    echo "      Done: $PREFIX/lib/libtre.a"
fi
echo ""

# ---------- 2. libiconv 1.17（动态，LGPL 合规） ----------
if [ -f "$PREFIX/bin/libiconv-2.dll" ] && [ -f "$PREFIX/lib/libiconv.dll.a" ]; then
    echo "[2/3] libiconv already built, skipping."
else
    echo "[2/3] Building libiconv 1.17 (shared, LGPL compliance)..."
    cd "$VENDOR/libiconv"
    make distclean >/dev/null 2>&1 || true
    ./configure --prefix="$PREFIX" --enable-shared --disable-static
    make -j"$JOBS"
    make install
    cd "$ROOT"
    echo "      Done: $PREFIX/bin/libiconv-2.dll"
fi
echo ""

# ---------- 3. libcurl 8.22.0（静态，Schannel TLS） ----------
if [ -f "$PREFIX/lib/libcurl.a" ]; then
    echo "[3/4] libcurl already built, skipping."
else
    echo "[3/4] Building libcurl 8.22.0 (static, Schannel)..."
    cd "$VENDOR/libcurl"
    make distclean >/dev/null 2>&1 || true
    ./configure --prefix="$PREFIX" --disable-shared --enable-static \
        --with-schannel \
        --with-libiconv-prefix="$PREFIX" \
        --disable-ldap --disable-ldaps \
        --without-libpsl --without-libidn2 --without-brotli --without-zstd \
        --without-nghttp2 --without-ngtcp2 --without-nghttp3 --without-quic
    make -j"$JOBS"
    make install
    cd "$ROOT"
    echo "      Done: $PREFIX/lib/libcurl.a"
fi
echo ""

# ---------- 4. GMP 6.3.0（动态，LGPL 合规，含导入库） ----------
if [ -f "$PREFIX/bin/libgmp-10.dll" ] && [ -f "$PREFIX/lib/libgmp.dll.a" ]; then
    echo "[4/4] GMP already built, skipping."
else
    echo "[4/4] Building GMP 6.3.0 (shared, LGPL compliance)..."
    cd "$VENDOR/gmp"
    make distclean >/dev/null 2>&1 || true
    ./configure --prefix="$PREFIX" --enable-shared --disable-static --disable-cxx
    make -j"$JOBS"
    make install
    cd "$ROOT"
    echo "      Done: $PREFIX/bin/libgmp-10.dll + $PREFIX/lib/libgmp.dll.a"
fi
echo ""

# ---------- 5. 许可证汇总（随 prebuilt/deps 入库，合规义务） ----------
echo "[license] Assembling license texts..."
LIC="$PREFIX/licenses"
mkdir -p "$LIC/libcurl" "$LIC/libiconv" "$LIC/tre"
cp "$VENDOR/libcurl/COPYING"     "$LIC/libcurl/COPYING"
cp "$VENDOR/libiconv/COPYING.LIB" "$LIC/libiconv/COPYING.LGPL-2.1"
cp "$VENDOR/tre/LICENSE"         "$LIC/tre/LICENSE"

# GMP 许可证 → deps/gmp/licenses
GMP_LIC="$ROOT/deps/gmp/licenses"
mkdir -p "$GMP_LIC"
cp "$VENDOR/gmp/COPYING.LESSERv3" "$VENDOR/gmp/COPYINGv2" \
   "$VENDOR/gmp/COPYINGv3" "$GMP_LIC/"
echo "         prebuilt/windows/licenses/ + deps/gmp/licenses/"
echo ""

echo "============================================================"
echo "All Windows dependencies built."
echo "下一步：scripts\\build.bat（或 mingw32-make -B CC=gcc all）"
echo "lumyr.exe 旁将自动复制 libiconv-2.dll / libgmp-10.dll，"
echo "许可证汇总于 prebuilt\\windows\\licenses 与 deps\\gmp\\licenses。"
echo "============================================================"
