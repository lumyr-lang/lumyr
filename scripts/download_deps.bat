@echo off
REM ============================================================
REM download_deps.bat - Download Lumyr compiler dependencies
REM Windows version
REM ============================================================

setlocal enabledelayedexpansion

echo ============================================================
echo Downloading Lumyr compiler dependencies...
echo ============================================================
echo.

REM Create directories
if not exist "vendor" mkdir vendor
if not exist "build_tools" mkdir build_tools

REM ========== vendor directory (third-party library source code) ==========

echo [1/7] Downloading libcurl 8.22.0...
if not exist "vendor\libcurl" (
    curl -L -o vendor\curl-8.22.0.tar.gz https://curl.se/download/curl-8.22.0.tar.gz
    cd vendor
    tar -xzf curl-8.22.0.tar.gz
    ren curl-8.22.0 libcurl
    del curl-8.22.0.tar.gz
    cd ..
    echo   Done.
) else (
    echo   Already cached, skipping download, next...
)
echo.

echo [2/7] Downloading libiconv 1.17...
if not exist "vendor\libiconv" (
    curl -L -o vendor\libiconv-1.17.tar.gz https://ftp.gnu.org/gnu/libiconv/libiconv-1.17.tar.gz
    cd vendor
    tar -xzf libiconv-1.17.tar.gz
    ren libiconv-1.17 libiconv
    del libiconv-1.17.tar.gz
    cd ..
    echo   Done.
) else (
    echo   Already cached, skipping download, next...
)
echo.

echo [3/7] Downloading TRE 0.9.0...
if not exist "vendor\tre" (
    curl -L -o vendor\tre-0.9.0.zip https://github.com/laurikari/tre/archive/refs/tags/v0.9.0.zip
    cd vendor
    powershell -Command "Expand-Archive -Path tre-0.9.0.zip -DestinationPath . -Force"
    ren tre-0.9.0 tre
    del tre-0.9.0.zip
    cd ..
    echo   Done.
) else (
    echo   Already cached, skipping download, next...
)
echo.

echo [4/7] Downloading GMP 6.3.0...
if not exist "vendor\gmp" (
    curl -L -o vendor\gmp-6.3.0.tar.gz https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.gz
    cd vendor
    tar -xzf gmp-6.3.0.tar.gz
    ren gmp-6.3.0 gmp
    del gmp-6.3.0.tar.gz
    cd ..
    echo   Done.
) else (
    echo   Already cached, skipping download, next...
)
echo.

REM ========== build_tools directory (build toolchain source code) ==========

echo [5/7] Downloading flex 2.6.4...
if not exist "build_tools\flex" (
    curl -L -o build_tools\flex-2.6.4.tar.gz https://github.com/westes/flex/releases/download/v2.6.4/flex-2.6.4.tar.gz
    cd build_tools
    tar -xzf flex-2.6.4.tar.gz
    ren flex-2.6.4 flex
    del flex-2.6.4.tar.gz
    cd ..
    echo   Done.
) else (
    echo   Already cached, skipping download, next...
)
echo.

echo [6/7] Downloading bison 3.8.2...
if not exist "build_tools\bison" (
    curl -L -o build_tools\bison-3.8.2.tar.gz https://ftp.gnu.org/gnu/bison/bison-3.8.2.tar.gz
    cd build_tools
    tar -xzf bison-3.8.2.tar.gz
    ren bison-3.8.2 bison
    del bison-3.8.2.tar.gz
    cd ..
    echo   Done.
) else (
    echo   Already cached, skipping download, next...
)
echo.

echo [7/7] Downloading m4 1.4.19...
if not exist "build_tools\m4" (
    curl -L -o build_tools\m4-1.4.19.tar.gz https://ftp.gnu.org/gnu/m4/m4-1.4.19.tar.gz
    cd build_tools
    tar -xzf m4-1.4.19.tar.gz
    ren m4-1.4.19 m4
    del m4-1.4.19.tar.gz
    cd ..
    echo   Done.
) else (
    echo   Already cached, skipping download, next...
)
echo.

echo ============================================================
echo All dependencies downloaded successfully!
echo ============================================================
echo.
echo Directory structure:
echo   vendor/         - Third-party library source code
echo   build_tools/    - Build toolchain source code
echo   deps/gmp/       - GMP headers and prebuilt libraries
echo   prebuilt/       - Prebuilt binaries (windows ready to use)
echo.
echo Next steps:
echo   Windows: in "MSYS2 MINGW64" terminal run:
echo              bash scripts/build_deps_mingw.sh
echo            then: scripts\build.bat
echo   macOS/Linux: bash scripts/build_deps.sh ^&^& make
echo.

endlocal
