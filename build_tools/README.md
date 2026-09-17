# Build Tools 编译工具链

本目录包含 Lumyr 编译器的构建工具链源码，这些工具用于生成词法分析器和语法分析器，只在编译编译器时使用，不会出现在最终产物中。

## 快速开始

### Windows
```bash
# 下载所有依赖源码
download_deps.bat
```

### macOS / Linux
```bash
# 下载所有依赖源码
./download_deps.sh
```

## 目录结构

运行下载脚本后，本目录将包含：

```
build_tools/
├── README.md           # 本文件
├── flex/             # flex 源码（词法分析器生成工具）
├── bison/            # bison 源码（语法分析器生成工具）
└── m4/               # m4 源码（宏处理器，bison 依赖）
```

## 各工具说明

### flex
- **版本**：2.6.4
- **用途**：词法分析器生成工具，用于从 .l 文件生成词法分析器（lexer）
- **许可证**：BSD 许可证
- **官方地址**：https://github.com/westes/flex
- **下载地址**：https://github.com/westes/flex/releases/download/v2.6.4/flex-2.6.4.tar.gz

### bison
- **版本**：3.8.2
- **用途**：语法分析器生成工具，用于从 .y 文件生成语法分析器（parser）
- **许可证**：GNU GPL v3 许可证
- **官方地址**：https://www.gnu.org/software/bison/
- **下载地址**：https://ftp.gnu.org/gnu/bison/bison-3.8.2.tar.gz

### m4
- **版本**：1.4.19
- **用途**：宏处理器，bison 的运行时依赖
- **许可证**：GNU GPL v3 许可证
- **官方地址**：https://www.gnu.org/software/m4
- **下载地址**：https://ftp.gnu.org/gnu/m4/m4-1.4.19.tar.gz

## 编译顺序

由于工具之间存在依赖关系，建议按照以下顺序编译：

1. **m4** - 宏处理器，bison 依赖
2. **flex** - 词法分析器生成工具
3. **bison** - 语法分析器生成工具（依赖 m4）

## 编译说明

### Windows (MinGW/MSYS2)
```bash
# m4
cd build_tools/m4
./configure --prefix=/path/to/install --disable-shared --enable-static
make
make install

# flex
cd build_tools/flex
./autogen.sh
./configure --prefix=/path/to/install --disable-shared --enable-static
make
make install

# bison
cd build_tools/bison
./configure --prefix=/path/to/install --disable-shared --enable-static
make
make install
```

### macOS
```bash
# 使用 Homebrew 安装（推荐）
brew install m4 flex bison

# 或从源码编译
# （方法同 Windows）
```

### Linux
```bash
# Debian/Ubuntu（推荐）
sudo apt install m4 flex bison

# 或从源码编译
# （方法同 Windows）
```

## 注意事项

1. **版本一致性**：各平台使用相同版本的工具，确保生成的词法/语法分析器一致
2. **编译选项**：各平台使用相同的编译选项
3. **运行时依赖**：bison 运行时需要 m4，确保 m4 已安装
4. **Windows 特殊说明**：Windows 用户可以直接使用 `prebuilt/windows/tools/winflexbison/` 中的预编译工具，无需从源码编译
5. **不要提交编译产物**：源码目录中只包含源码，不包含编译产物（.o、.a、.exe 等）
