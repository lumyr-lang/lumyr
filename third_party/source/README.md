# Third Party Source 第三方依赖源码

本目录包含 Lumyr 编译器所需的所有第三方依赖库和工具的源码，用于在各平台上从源码编译，保持工具链在各平台的一致性。

## 目录结构

```
source/
├── README.md           # 本文件，源码总览说明
├── libiconv/           # libiconv 源码（字符编码转换）
├── libcurl/            # libcurl 源码（HTTP 客户端）- 待下载
├── flex/               # flex 源码（词法分析器生成工具）- 待下载
├── bison/              # bison 源码（语法分析器生成工具）- 待下载
├── tre/                # TRE 源码（正则表达式匹配库）- 待下载
└── m4/                 # m4 源码（宏处理器，bison 依赖）- 待下载
```

## 各工具源码状态

| 工具 | 版本 | 状态 | 源码地址 |
|------|------|------|----------|
| libiconv | 1.17+ | ✅ 已包含 | https://www.gnu.org/software/libiconv/ |
| libcurl | 8.22.0 | ⏳ 待下载 | https://curl.se/download.html |
| flex | 2.6.4 | ⏳ 待下载 | https://github.com/westes/flex |
| bison | 3.8.2 | ⏳ 待下载 | https://www.gnu.org/software/bison/ |
| TRE | 0.8.0 | ⏳ 待下载 | https://github.com/laurikari/tre |
| m4 | 1.4.19 | ⏳ 待下载 | https://www.gnu.org/software/m4/ |

**说明：**
- ✅ 已包含：源码已包含在对应目录中
- ⏳ 待下载：需要从官方地址下载源码并解压到对应目录中

## 各平台编译说明

### Windows
- 编译器：MinGW-w64 GCC
- 依赖工具：MSYS2 或 Cygwin（用于运行 configure 脚本）
- 编译脚本：`third_party/windows/build_tools.bat`（待创建）

### macOS
- 编译器：Clang（Xcode Command Line Tools）或 GCC
- 依赖工具：Homebrew（用于安装依赖）
- 编译脚本：`third_party/macos/build_tools.sh`（待创建）

### Linux
- 编译器：GCC 或 Clang
- 依赖工具：build-essential、autoconf、automake、libtool 等
- 编译脚本：`third_party/linux/build_tools.sh`（待创建）

## 编译顺序

由于工具之间存在依赖关系，建议按照以下顺序编译：

1. **m4** - 宏处理器，bison 依赖
2. **libiconv** - 字符编码转换，flex/bison 可能依赖
3. **flex** - 词法分析器生成工具
4. **bison** - 语法分析器生成工具（依赖 m4）
5. **libcurl** - HTTP 客户端库
6. **TRE** - 正则表达式匹配库

## 注意事项

1. **源码版本一致性**：各平台使用相同版本的源码，确保编译产物的行为一致
2. **编译选项一致性**：各平台使用相同的编译选项（如优化级别、调试信息等）
3. **静态库优先**：优先编译为静态库，减少运行时依赖
4. **平台特定修改**：如果需要对源码进行平台特定的修改，应在对应平台的目录中创建补丁文件
5. **许可证合规**：各工具的许可证不同，使用时请遵守对应许可证的要求
6. **不要提交编译产物**：源码目录中只包含源码，不包含编译产物（.o、.a、.exe 等）

## 下载源码的方法

对于标记为"待下载"的工具，可以通过以下方式获取源码：

### 方法1：从官方网站下载
```bash
# 示例：下载 libcurl 源码
wget https://curl.se/download/curl-8.22.0.tar.gz
tar -xzf curl-8.22.0.tar.gz
mv curl-8.22.0 libcurl/
```

### 方法2：使用 Git 克隆
```bash
# 示例：克隆 flex 源码
git clone https://github.com/westes/flex.git flex/
cd flex/
git checkout v2.6.4
```

### 方法3：使用包管理器获取源码包
```bash
# Debian/Ubuntu
apt-get source libcurl4-openssl-dev

# macOS (Homebrew)
brew download --build-from-source curl
```

下载完成后，将源码解压到对应目录中，并确保目录结构正确。
