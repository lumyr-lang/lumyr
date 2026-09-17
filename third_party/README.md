# Third Party 第三方依赖

本目录包含 Lumyr 编译器在各平台编译所需的第三方依赖库和工具。

## 目录结构

```
third_party/
├── README.md           # 本文件，总览说明
├── windows/            # Windows 平台依赖
│   ├── README.md       # Windows 平台依赖说明
│   ├── include/        # 头文件（curl、tre、iconv）
│   ├── lib/            # 静态库文件（libcurl.a、libiconv.a、libtre.a）
│   └── tools/          # 构建工具（winflexbison）
├── macos/              # macOS 平台依赖
│   ├── README.md       # macOS 平台依赖说明
│   ├── include/        # 头文件（预留，系统自带或通过 Homebrew 安装）
│   ├── lib/            # 静态库文件（预留，系统自带或通过 Homebrew 安装）
│   └── tools/          # 构建工具（预留，系统自带 flex/bison）
└── linux/              # Linux 平台依赖
    ├── README.md       # Linux 平台依赖说明
    ├── include/        # 头文件（预留，通过系统包管理器安装）
    ├── lib/            # 静态库文件（预留，通过系统包管理器安装）
    └── tools/          # 构建工具（预留，通过系统包管理器安装）
```

## 各平台依赖概览

| 依赖 | Windows | macOS | Linux |
|------|---------|-------|-------|
| libcurl | ✅ 已包含（8.22.0） | ⚠️ 系统自带 / Homebrew | ⚠️ 包管理器安装 |
| libiconv | ✅ 已包含 | ⚠️ 系统自带 | ⚠️ glibc 自带 |
| TRE | ✅ 已包含 | ⚠️ Homebrew 安装 | ⚠️ 包管理器安装 |
| flex | ✅ WinFlexBison（2.6.4） | ⚠️ 系统自带 | ⚠️ 包管理器安装 |
| bison | ✅ WinFlexBison（3.8.2） | ⚠️ Homebrew 安装（系统版本较旧） | ⚠️ 包管理器安装 |

**说明：**
- ✅ 已包含：库文件和工具已包含在对应平台目录中，无需额外安装
- ⚠️ 需安装：需要通过系统包管理器或其他方式安装
- Windows 平台的所有依赖都已预编译并包含在目录中，开箱即用
- macOS 和 Linux 平台的依赖需要通过系统包管理器安装，目录结构已预留

## 平台适配说明

### Windows
- 编译器：MinGW-w64 GCC
- 所有依赖库已预编译为静态库（.a 文件）
- WinFlexBison 工具已包含在 tools 目录中

### macOS
- 编译器：Clang（Xcode Command Line Tools）或 GCC
- 系统自带 libiconv、libcurl、flex
- 建议使用 Homebrew 安装较新版本的 bison 和 TRE

### Linux
- 编译器：GCC 或 Clang
- glibc 自带 iconv 功能
- 所有依赖可通过系统包管理器安装（apt、yum、dnf、pacman 等）

## 编译命令

各平台的详细编译说明请参考对应平台目录下的 README.md 文件。

通用编译命令：
```bash
make -B CC=gcc all
```

## 注意事项

1. 各平台的静态库文件（.a）不通用，必须使用对应平台编译的库文件
2. Windows 平台的 .a 文件是 MinGW 格式，不能在 macOS 或 Linux 上使用
3. macOS 和 Linux 平台的目录结构已预留，实际库文件需要在对应平台上编译或安装
4. 升级依赖库版本时，需要在各平台上分别编译并更新对应目录中的文件
