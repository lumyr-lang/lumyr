# macOS 平台第三方依赖

本目录包含 macOS 平台编译 Lumyr 编译器所需的第三方依赖库和工具。

## 目录结构

```
macos/
├── include/          # 头文件
│   ├── curl/         # libcurl 头文件
│   ├── tre/          # TRE 正则表达式库头文件
│   └── iconv.h       # libiconv 头文件（macOS 系统自带，可选）
├── lib/              # 静态库文件
│   ├── libcurl.a     # libcurl 静态库
│   ├── libiconv.a    # libiconv 静态库（macOS 系统自带，可选）
│   └── libtre.a      # TRE 正则表达式库静态库
└── tools/            # 构建工具
    └── (flex/bison)  # macOS 系统自带 flex 和 bison，无需额外安装
```

## 依赖列表

| 依赖 | 版本 | 用途 | 安装方式 |
|------|------|------|----------|
| libcurl | 8.22.0 | HTTP 客户端，用于网络请求 | `brew install curl` 或系统自带 |
| libiconv | - | 字符编码转换 | macOS 系统自带 |
| TRE | - | 正则表达式匹配（近似匹配） | `brew install tre` |
| flex | 2.6.4+ | 词法分析器生成工具 | macOS 系统自带 |
| bison | 3.8.2+ | 语法分析器生成工具 | `brew install bison`（系统自带版本较旧） |

## 编译环境

- 编译器：Clang（Xcode Command Line Tools）或 GCC
- 构建工具：make
- 依赖安装：Homebrew

## 安装依赖

```bash
# 安装 Xcode Command Line Tools
xcode-select --install

# 安装 Homebrew（如果尚未安装）
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 安装依赖
brew install curl tre bison
```

## 编译命令

```bash
make -B CC=clang all
# 或
make -B CC=gcc all
```

## 注意事项

1. macOS 系统自带的 bison 版本较旧（通常是 2.3），建议使用 Homebrew 安装较新版本
2. macOS 系统自带 libiconv 和 libcurl，无需额外安装
3. 如果使用 Homebrew 安装的 curl，需要在编译时指定正确的头文件和库路径
