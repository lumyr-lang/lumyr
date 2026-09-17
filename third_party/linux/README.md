# Linux 平台第三方依赖

本目录包含 Linux 平台编译 Lumyr 编译器所需的第三方依赖库和工具。

## 目录结构

```
linux/
├── include/          # 头文件
│   ├── curl/         # libcurl 头文件
│   ├── tre/          # TRE 正则表达式库头文件
│   └── iconv.h       # libiconv 头文件（glibc 自带，可选）
├── lib/              # 静态库文件
│   ├── libcurl.a     # libcurl 静态库
│   ├── libiconv.a    # libiconv 静态库（glibc 自带，可选）
│   └── libtre.a      # TRE 正则表达式库静态库
└── tools/            # 构建工具
    └── (flex/bison)  # 大多数发行版可通过包管理器安装
```

## 依赖列表

| 依赖 | 版本 | 用途 | 安装方式（Debian/Ubuntu） | 安装方式（RHEL/CentOS） |
|------|------|------|---------------------------|--------------------------|
| libcurl | 8.22.0 | HTTP 客户端，用于网络请求 | `sudo apt install libcurl4-openssl-dev` | `sudo yum install libcurl-devel` |
| libiconv | - | 字符编码转换 | glibc 自带 | glibc 自带 |
| TRE | - | 正则表达式匹配（近似匹配） | `sudo apt install libtre-dev` | `sudo yum install tre-devel` |
| flex | 2.6.4+ | 词法分析器生成工具 | `sudo apt install flex` | `sudo yum install flex` |
| bison | 3.8.2+ | 语法分析器生成工具 | `sudo apt install bison` | `sudo yum install bison` |
| gcc | - | C 编译器 | `sudo apt install build-essential` | `sudo yum groupinstall "Development Tools"` |
| make | - | 构建工具 | `sudo apt install make` | `sudo yum install make` |

## 编译环境

- 编译器：GCC 或 Clang
- 构建工具：GNU Make
- 依赖安装：系统包管理器（apt、yum、dnf 等）

## 安装依赖（Debian/Ubuntu）

```bash
sudo apt update
sudo apt install build-essential flex bison libcurl4-openssl-dev libtre-dev
```

## 安装依赖（RHEL/CentOS/Fedora）

```bash
sudo yum groupinstall "Development Tools"
sudo yum install flex bison libcurl-devel tre-devel
```

## 安装依赖（Arch Linux）

```bash
sudo pacman -S base-devel flex bison curl tre
```

## 编译命令

```bash
make -B CC=gcc all
# 或
make -B CC=clang all
```

## 注意事项

1. 大多数 Linux 发行版的 glibc 已自带 iconv 功能，无需额外安装 libiconv
2. 如果系统包管理器中的 TRE 版本较旧或不可用，可以从源码编译安装
3. 建议使用 GCC 7.0 或更高版本，以支持 C11 标准
4. 如果使用 musl libc 的发行版（如 Alpine Linux），需要额外安装 libiconv
