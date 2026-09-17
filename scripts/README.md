# Scripts 项目脚本

本目录包含 Lumyr 编译器的各种实用脚本，用于简化开发和构建流程。

## 脚本列表

### download_deps.bat (Windows)
一键下载所有依赖源码到对应的目录：
- `vendor/` 目录：libcurl、libiconv、TRE
- `build_tools/` 目录：flex、bison、m4

**使用方法：**
```bash
scripts\download_deps.bat
```

### download_deps.sh (macOS/Linux)
一键下载所有依赖源码到对应的目录：
- `vendor/` 目录：libcurl、libiconv、TRE
- `build_tools/` 目录：flex、bison、m4

**使用方法：**
```bash
chmod +x scripts/download_deps.sh
./scripts/download_deps.sh
```

## 下载的工具版本

| 工具 | 版本 | 目录 |
|------|------|------|
| libcurl | 8.22.0 | vendor/libcurl |
| libiconv | 1.17 | vendor/libiconv |
| TRE | 0.9.0 | vendor/tre |
| flex | 2.6.4 | build_tools/flex |
| bison | 3.8.2 | build_tools/bison |
| m4 | 1.4.19 | build_tools/m4 |

## 注意事项

1. **源码不提交到 git**：依赖源码不提交到 git 仓库，通过下载脚本一键获取
2. **版本一致性**：各平台使用相同版本的源码，确保编译产物的行为一致
3. **Windows 快速开始**：Windows 用户可以直接使用 `prebuilt/windows/` 中的预编译产物，无需从源码编译
4. **macOS/Linux 推荐**：macOS/Linux 用户推荐使用系统包管理器安装依赖，而不是从源码编译
