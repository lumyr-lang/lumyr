@echo off
REM ============================================================
REM build.bat - Build Lumyr compiler
REM Windows version
REM ============================================================

setlocal enabledelayedexpansion

echo ============================================================
echo Building Lumyr compiler...
echo ============================================================
echo.

REM Check if prebuilt dependencies exist
if not exist "prebuilt\windows\lib\libcurl.a" (
    echo ERROR: Prebuilt dependencies not found!
    echo Please run scripts\download_deps.bat first to download dependencies.
    echo.
    exit /b 1
)

REM Set up PATH
set PATH=C:\mingw64\bin;D:\apps\git\Git\usr\bin;prebuilt\windows\tools\winflexbison;%PATH%

REM Check toolchain
echo Checking toolchain...
flex --version
if errorlevel 1 (
    echo ERROR: flex not found in PATH!
    exit /b 1
)

bison --version | findstr " 3." > nul
if errorlevel 1 (
    echo ERROR: bison ^>=3.x required!
    exit /b 1
)

gcc --version
if errorlevel 1 (
    echo ERROR: gcc not found in PATH!
    exit /b 1
)
echo.

REM Build
echo Starting build...
echo.
mingw32-make -B CC=gcc all
if errorlevel 1 (
    echo.
    echo ERROR: Build failed!
    exit /b 1
)

echo.
echo ============================================================
echo Build completed successfully!
echo ============================================================
echo.
echo Executable: bin\lumyr.exe
echo.
echo Usage:
echo   VM mode:     bin\lumyr.exe file.lm
echo   CC mode:     bin\lumyr.exe -c file.lm
echo.

endlocal
