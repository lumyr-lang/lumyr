# Scripts 项目脚本

本目录包含 Lumyr 编译器的各种实用脚本，用于简化开发和构建流程。

## 各平台构建依赖准备

### macOS
除系统自带库（curl / iconv / POSIX regex）外，只需 Homebrew 安装三项：

```bash
brew install gmp bison flex
```

然后仓库根目录直接 `make`。无需下载本目录的第三方源码。

### Linux
使用发行版包（curl / iconv / regex 均为系统库）：

```bash
# Debian/Ubuntu
sudo apt install libgmp-dev bison flex
```

### Windows
先下载依赖源码（在仓库根目录执行）：

```bat
scripts\download_deps.bat
```

再于 **MSYS2 MINGW64 终端**从源码构建运行时库：

```bash
# 首次需安装工具链：
pacman -S --needed base-devel mingw-w64-x86_64-toolchain autoconf automake libtool
bash scripts/build_deps_mingw.sh
```

最后 `scripts\build.bat`。

## 脚本列表

### download_deps.bat (Windows)
下载 Windows 依赖源码到 `scripts/vendor/`（libcurl、libiconv、TRE、gmp）
和 `scripts/build_tools/`（flex、bison、m4）。脚本使用相对路径，需在仓库根目录执行。

### build_deps_mingw.sh (Windows)
在 MSYS2 MINGW64 下从源码构建：
- TRE 0.9.0 静态、libcurl 8.22.0 静态（TLS 用 Windows 原生 Schannel）
- libiconv 1.17 动态（libiconv-2.dll，LGPL 合规）
- GMP 沿用 `deps/gmp/lib/windows-x64/` 已有动态库，不在此脚本构建
- 同时装配许可证到 prebuilt/windows/licenses 与 deps/gmp/licenses

### build.bat (Windows)
检查 prebuilt/windows 产物是否齐备，设置 PATH 后执行 mingw32-make 完整构建。
构建后 bin/ 含 lumyr.exe、libiconv-2.dll、libgmp-10.dll、licenses/。

### build.sh (macOS/Linux)
一键构建入口（实际调用 make）。

## 依赖版本（Windows 源码构建锁定）

| 依赖 | 版本 | 源码目录 |
|------|------|------|
| libcurl | 8.22.0 | scripts/vendor/libcurl |
| libiconv | 1.17 | scripts/vendor/libiconv |
| TRE | 0.9.0 | scripts/vendor/tre |
| GMP | 6.3.0 | scripts/vendor/gmp / deps/gmp |
| flex | 2.6.4 | scripts/build_tools/flex |
| bison | 3.8.2 | scripts/build_tools/bison |
| m4 | 1.4.19 | scripts/build_tools/m4 |

macOS/Linux 使用系统库与包管理器版本，不锁定上述版本。

## 注意事项

1. **源码不提交 git**：scripts/vendor、scripts/build_tools 通过下载脚本获取
2. **平台策略**：macOS/Linux 优先系统库（适配代码内置），Windows 使用 prebuilt/windows
3. **许可证**：GMP（LGPLv3）全平台动态链接；Windows 另有 libiconv（LGPL-2.1）动态、
   curl（MIT）/TRE（BSD）静态，许可证原文见 prebuilt/windows/licenses、deps/gmp/licenses
   及根目录 NOTICE
