# GMP 高精度数学库

GNU Multiple Precision Arithmetic Library (GMP)

## 平台策略

| 平台 | GMP 来源 |
|------|----------|
| macOS | Homebrew（`brew install gmp`） |
| Linux | 发行版包（libgmp-dev） |
| Windows | 本目录 lib/windows-x64、lib/windows-x86 的预编译 dll |

## 目录结构

```
deps/gmp/
├── include/             # Windows 构建使用的头文件
├── lib/
│   ├── windows-x64/     # Windows 64位 libgmp-10.dll
│   └── windows-x86/     # Windows 32位 libgmp-10.dll
├── licenses/            # LGPLv3 / GPLv2 / GPLv3 原文 + INDEX
└── README.md
```

macOS/Linux 不使用本目录的头文件与库（走各自系统路径）。

## 许可证与动态链接

- 许可证：LGPL v3（库主体）/ 部分文件 GPL v2+，原文见 licenses/
- 全平台**动态链接**：
  - macOS：构建时把 brew 的 libgmp.10.dylib 复制到 lumyr 旁，并改为
    `@loader_path/libgmp.10.dylib`，用户可自行替换（满足 LGPL）
  - Windows：libgmp-10.dll 复制到 lumyr.exe 旁
  - Linux：使用系统 libgmp.so.10（随发行版分发，不在本产品内）
- 动态链接不影响主程序许可证，lm 用户可闭源商用

## Windows GMP 更新方式

从 GMP 官方源码（https://gmplib.org/）在 MSYS2/MinGW 下构建：
```bash
./configure --enable-shared --disable-static --prefix=<stage>
make && make install
# 把 stage/bin/libgmp-10.dll 复制到 lib/windows-x64/（或 x86）
```
