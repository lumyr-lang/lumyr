# GMP 许可证 — GNU Multiple Precision Arithmetic Library

GMP 在本项目中以**动态链接**方式使用（LGPL 合规）：

| 平台 | 来源 / 动态库 |
|------|----------------|
| macOS | Homebrew GMP；构建时复制 libgmp.10.dylib 到 lumyr 旁（@loader_path） |
| Linux | 系统 libgmp.so.10（发行版包） |
| Windows | prebuilt/windows/bin/libgmp-10.dll（源码构建，含 lib/libgmp.dll.a 导入库），复制到 lumyr.exe 旁 |

许可证原文：

| 文件 | 内容 |
|------|------|
| [COPYING.LESSERv3](COPYING.LESSERv3) | LGPL v3 — GMP 库主体许可证 |
| [COPYINGv3](COPYINGv3) | GPL v3 |
| [COPYINGv2](COPYINGv2) | GPL v2 — 部分源文件采用（GPL v2+） |

## 合规说明

- 动态链接 + 动态库随程序分发，用户可用自行编译的同版本 GMP 替换，满足 LGPL
  的可重新链接义务。
- 再分发二进制发布包时本目录必须随附。源码：https://gmplib.org/
