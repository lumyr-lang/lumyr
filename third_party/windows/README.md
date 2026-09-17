# Windows 平台第三方依赖

本目录包含 Windows 平台编译 Lumyr 编译器所需的第三方依赖库和工具。

## 目录结构

```
windows/
├── include/          # 头文件
│   ├── curl/         # libcurl 头文件
│   ├── tre/          # TRE 正则表达式库头文件
│   └── iconv.h       # libiconv 头文件
├── lib/              # 静态库文件
│   ├── libcurl.a     # libcurl 静态库
│   ├── libiconv.a    # libiconv 静态库
│   └── libtre.a      # TRE 正则表达式库静态库
└── tools/            # 构建工具
    └── winflexbison/ # WinFlexBison（flex 2.6.4 / bison 3.8.2）
```

## 依赖列表

| 依赖 | 版本 | 用途 |
|------|------|------|
| libcurl | 8.22.0 | HTTP 客户端，用于网络请求 |
| libiconv | - | 字符编码转换 |
| TRE | - | 正则表达式匹配（近似匹配） |
| WinFlexBison | 2.5.25 | 词法分析器和语法分析器生成工具 |

## 编译环境

- 编译器：MinGW-w64 GCC
- 构建工具：mingw32-make
- PATH 需要包含：
  - `C:\mingw64\bin`
  - `D:\apps\git\Git\usr\bin`
  - `third_party\windows\tools\winflexbison`

## 编译命令

```bash
mingw32-make -B CC=gcc all
```
