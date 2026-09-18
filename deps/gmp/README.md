# GMP 高精度数学库

GNU Multiple Precision Arithmetic Library (GMP) 6.3.0

## 目录结构

```
deps/gmp/
├── include/          # 跨平台头文件
│   └── gmp.h
├── lib/
│   ├── macos-x86_64/    # macOS Intel 动态库 (.dylib)
│   │   ├── libgmp.10.dylib
│   │   └── libgmp.dylib → libgmp.10.dylib
│   ├── macos-arm64/     # macOS Apple Silicon 动态库 - 待编译
│   │   └── libgmp.10.dylib
│   ├── linux-x86_64/    # Linux x86_64 动态库 (.so) - 待编译
│   │   └── libgmp.so
│   ├── linux-aarch64/   # Linux ARM64 动态库 (.so) - 待编译
│   │   └── libgmp.so
│   ├── windows-x86/     # Windows 32位 动态库 (.dll) - 待编译
│   │   └── gmp-10.dll
│   └── windows-x64/     # Windows 64位 动态库 (.dll) - 待编译
│       └── gmp-10.dll
└── README.md
```

## 版本信息

- 版本：6.3.0
- 许可证：LGPL v3 / GPL v2
- 官网：https://gmplib.org/

## 各平台编译说明

### macOS Intel (x86_64)
```bash
./configure --enable-shared --disable-static --prefix=./安装目录
make -j4
make install
# 修改 install_name 为 @loader_path/libgmp.10.dylib
install_name_tool -id @loader_path/libgmp.10.dylib libgmp.10.dylib
# 复制到 lib/macos-x86_64/
```

### macOS Apple Silicon (arm64)
```bash
# 在 M1/M2 Mac 上
./configure --enable-shared --disable-static --prefix=./安装目录
make -j4
make install
install_name_tool -id @loader_path/libgmp.10.dylib libgmp.10.dylib
# 复制到 lib/macos-arm64/
```

### Linux x86_64
```bash
# 在 Linux x86_64 环境下
./configure --enable-shared --disable-static --prefix=./安装目录
make -j4
make install
# 把 lib/libgmp.so* 复制到 lib/linux-x86_64/
```

### Linux ARM64 (aarch64)
```bash
# 在 Linux ARM64 环境下
./configure --enable-shared --disable-static --prefix=./安装目录
make -j4
make install
# 把 lib/libgmp.so* 复制到 lib/linux-aarch64/
```

### Windows (MinGW-w64)
```bash
# 在 MSYS2/MinGW 环境下
./configure --enable-shared --disable-static --prefix=./安装目录
make -j4
make install
# 把 bin/gmp-10.dll 复制到 lib/windows-x64/ 或 lib/windows-x86/
```

## Makefile 配置

```makefile
GMP_DIR := $(CURDIR)/deps/gmp
ARCH := $(shell uname -m)

# 按操作系统和架构选择动态库目录
ifeq ($(OS_NAME),macos)
    ifeq ($(ARCH),arm64)
        GMP_LIB_DIR := $(GMP_DIR)/lib/macos-arm64
    else
        GMP_LIB_DIR := $(GMP_DIR)/lib/macos-x86_64
    endif
else ifeq ($(OS_NAME),linux)
    ifeq ($(ARCH),aarch64)
        GMP_LIB_DIR := $(GMP_DIR)/lib/linux-aarch64
    else ifeq ($(ARCH),x86_64)
        GMP_LIB_DIR := $(GMP_DIR)/lib/linux-x86_64
    endif
else ifeq ($(OS_NAME),windows)
    ifeq ($(ARCH),x86_64)
        GMP_LIB_DIR := $(GMP_DIR)/lib/windows-x64
    else
        GMP_LIB_DIR := $(GMP_DIR)/lib/windows-x86
    endif
endif

CFLAGS += -I$(GMP_DIR)/include
LDFLAGS += -L$(GMP_LIB_DIR) -Wl,-rpath,$(GMP_LIB_DIR)
LDLIBS += -lgmp
```

## 注意事项

1. **许可证**：GMP 使用 LGPL v3 许可证，动态链接方式不影响主程序的许可证，lm 用户可以闭源商用
2. **跨平台**：头文件是跨平台的，动态库需要按"操作系统+架构"分别编译
3. **运行时**：动态库自动复制到 bin 目录，使用 `@loader_path` 在可执行文件所在目录查找
4. **分发**：用户只需把整个 bin 目录复制走，就能直接运行，不需要安装 GMP
