# Prebuilt 预编译产物

本目录存放第三方预编译库。当前策略：

- **Windows**：使用本目录下 `windows/` 的预编译产物（MinGW-w64），随 git 提供
- **macOS / Linux**：使用操作系统自带库（curl / iconv / POSIX regex），
  本目录下不再保留平台产物；GMP 通过 Homebrew / 发行版包安装

## 各平台构建准备

### macOS
```bash
brew install gmp bison flex
make
```

### Linux
```bash
# Debian/Ubuntu
sudo apt install libgmp-dev bison flex
make
```

### Windows
```bat
scripts\download_deps.bat
```
然后在 MSYS2 MINGW64 终端：
```bash
bash scripts/build_deps_mingw.sh
```
最后 `scripts\build.bat`。

## 目录结构

```
prebuilt/
├── README.md
└── windows/               # Windows 预编译产物（随 git 提供）
    ├── include/           # curl / iconv / tre 头文件
    ├── bin/               # libiconv-2.dll（动态，LGPL）
    ├── lib/               # libcurl.a / libtre.a 静态 + libiconv.dll.a 导入库
    ├── licenses/          # curl/iconv/tre 许可证原文
    └── tools/             # WinFlexBison（flex 2.6.4 / bison 3.8.2）
```

## Windows 已包含组件

| 库 | 版本 | 类型 | 许可证 |
|------|------|------|------|
| libcurl | 8.22.0 | 静态（TLS: Schannel） | MIT/X |
| libiconv | 1.17 | 动态 libiconv-2.dll | LGPL-2.1 |
| TRE | 0.9.0 | 静态（Windows 上充当 POSIX regex） | BSD 2-Clause |
| WinFlexBison | 2.5.25 | 构建工具，不随产品分发 | GPL（生成物例外） |

构建后 lumyr.exe 旁还会复制 GMP（deps/gmp/lib/windows-*/libgmp-10.dll，LGPLv3）。

## 注意事项

1. 平台产物不可跨平台使用
2. Windows 组件从源码可重现构建：scripts/build_deps_mingw.sh
3. 完整许可证清单见根目录 NOTICE；原文在 prebuilt/windows/licenses、deps/gmp/licenses
