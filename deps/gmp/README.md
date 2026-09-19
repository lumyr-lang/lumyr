# GMP 高精度数学库

GNU Multiple Precision Arithmetic Library (GMP)

本目录现在只存放 **GMP 许可证原文**；头文件与动态库的来源按平台划分：

| 平台 | GMP 头文件 / 库来源 |
|------|----------|
| macOS | Homebrew（`brew install gmp`），include 与 lib 在 brew 目录 |
| Linux | 发行版包（libgmp-dev），系统默认路径 |
| Windows | `scripts/build_deps_mingw.sh` 从源码构建，装到 prebuilt/windows：
  include/gmp.h、lib/libgmp.dll.a（导入库）、bin/libgmp-10.dll |

## 目录结构

```
deps/gmp/
├── licenses/            # LGPLv3 / GPLv2 / GPLv3 原文 + INDEX
└── README.md
```

## 许可证与动态链接

- 许可证：LGPL v3（库主体）/ 部分文件 GPL v2+，原文见 licenses/
- 全平台**动态链接**：
  - macOS：构建时把 brew 的 libgmp.10.dylib 复制到 lumyr 旁，并改为
    `@loader_path/libgmp.10.dylib`，用户可自行替换（满足 LGPL）
  - Windows：libgmp-10.dll 复制到 lumyr.exe 旁
  - Linux：使用系统 libgmp.so.10（随发行版分发，不在本产品内）
- 动态链接不影响主程序许可证，lm 用户可闭源商用
