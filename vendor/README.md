# Vendor 第三方库源码依赖

本目录包含 Lumyr 编译器的第三方库源码依赖，这些库会被编译成静态库链接到编译器或用户程序中。

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
vendor/
├── README.md           # 本文件
├── libcurl/          # libcurl 源码（HTTP 客户端库）
├── libiconv/         # libiconv 源码（字符编码转换库）
└── tre/              # TRE 源码（正则表达式匹配库）
```

## 各库说明

### libcurl
- **版本**：8.22.0
- **用途**：HTTP 客户端，用于网络请求
- **许可证**：MIT/X derivate 许可证
- **官方地址**：https://curl.se/
- **下载地址**：https://curl.se/download/curl-8.22.0.tar.gz

### libiconv
- **版本**：1.17
- **用途**：字符编码转换，支持多种字符集之间的转换
- **许可证**：GNU LGPL 2.1
- **官方地址**：https://www.gnu.org/software/libiconv/
- **下载地址**：https://ftp.gnu.org/gnu/libiconv/libiconv-1.17.tar.gz

### TRE
- **版本**：0.9.0
- **用途**：正则表达式匹配库，支持近似匹配（模糊匹配）
- **许可证**：BSD 2-Clause 许可证
- **官方地址**：https://github.com/laurikari/tre
- **下载地址**：https://github.com/laurikari/tre/archive/refs/tags/v0.9.0.zip

## 编译说明

各平台的编译方法请参考各库目录下的 README 或官方文档。

### Windows (MinGW)
```bash
# libcurl
cd vendor/libcurl
./configure --prefix=/path/to/install --with-openssl --disable-shared --enable-static
make
make install

# libiconv
cd vendor/libiconv
./configure --prefix=/path/to/install --disable-shared --enable-static
make
make install

# TRE
cd vendor/tre
./autogen.sh
./configure --prefix=/path/to/install --disable-shared --enable-static
make
make install
```

### macOS
```bash
# 使用 Homebrew 安装（推荐）
brew install curl libiconv tre

# 或从源码编译
# （方法同 Windows）
```

### Linux
```bash
# Debian/Ubuntu（推荐）
sudo apt install libcurl4-openssl-dev libtre-dev

# 或从源码编译
# （方法同 Windows）
```

## 注意事项

1. **版本一致性**：各平台使用相同版本的源码，确保编译产物的行为一致
2. **静态库优先**：优先编译为静态库，减少运行时依赖
3. **编译选项**：各平台使用相同的编译选项（如优化级别、调试信息等）
4. **许可证合规**：各库的许可证不同，使用时请遵守对应许可证的要求
5. **不要提交编译产物**：源码目录中只包含源码，不包含编译产物（.o、.a、.exe 等）
6. **快速开始**：Windows 用户可以直接使用 `prebuilt/windows/` 中的预编译产物，无需从源码编译
