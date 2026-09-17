# Prebuilt 预编译产物

本目录包含 Lumyr 编译器在各平台预编译好的第三方库和工具，用于快速开始编译，无需从源码编译。

## 快速开始

### Windows
直接使用本目录下的预编译产物，无需额外下载或编译。

### macOS / Linux
推荐使用系统包管理器安装依赖：

```bash
# macOS (Homebrew)
brew install curl libiconv tre flex bison m4

# Debian/Ubuntu
sudo apt install libcurl4-openssl-dev libtre-dev flex bison m4
```

## 目录结构

```
prebuilt/
├── README.md           # 本文件
├── windows/           # Windows 平台预编译产物
│   ├── README.md       # Windows 平台说明
│   ├── include/        # 头文件
│   ├── lib/            # 静态库文件（.a）
│   └── tools/          # 构建工具（winflexbison）
├── macos/             # macOS 平台预编译产物（预留）
└── linux/             # Linux 平台预编译产物（预留）
```

## Windows 平台

### 已包含的库
| 库 | 版本 | 类型 | 用途 |
|------|------|------|------|
| libcurl | 8.22.0 | 静态库 | HTTP 客户端 |
| libiconv | - | 静态库 | 字符编码转换 |
| TRE | - | 静态库 | 正则表达式匹配 |

### 已包含的工具
| 工具 | 版本 | 用途 |
|------|------|------|
| WinFlexBison | 2.5.25 | flex 2.6.4 / bison 3.8.2 |

### 编译环境
- 编译器：MinGW-w64 GCC
- 构建工具：mingw32-make
- PATH 需要包含：
  - `C:\mingw64\bin`
  - `D:\apps\git\Git\usr\bin`
  - `prebuilt\windows\tools\winflexbison`

### 编译命令
```bash
mingw32-make -B CC=gcc all
```

## macOS 平台

### 状态
预留目录，待后续添加预编译产物。

### 推荐安装方式
```bash
# 使用 Homebrew 安装
brew install curl libiconv tre flex bison m4
```

## Linux 平台

### 状态
预留目录，待后续添加预编译产物。

### 推荐安装方式
```bash
# Debian/Ubuntu
sudo apt install libcurl4-openssl-dev libtre-dev flex bison m4

# RHEL/CentOS
sudo yum install libcurl-devel tre-devel flex bison m4
```

## 注意事项

1. **平台特定**：预编译产物是平台特定的，不能跨平台使用（如 Windows 的 .a 文件不能在 macOS 或 Linux 上使用）
2. **源码优先**：如果需要修改或优化第三方库，建议从 `vendor/` 或 `build_tools/` 中的源码编译
3. **版本一致性**：预编译产物的版本应与 `vendor/` 和 `build_tools/` 中的源码版本一致
4. **不要提交大文件**：预编译产物中的大文件（如 .a、.exe）不应提交到 git，建议通过 release 或其他方式分发
5. **更新策略**：更新第三方库版本时，需要重新编译并更新对应平台的预编译产物
