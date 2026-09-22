# lumyr-lang-compiler Makefile
# Cross-platform: macOS / Linux / Windows (MinGW-w64)
# 大项目架构：runtime 编译为静态库 libruntime.a，编译器和生成代码都链接它
CC ?= gcc
CFLAGS ?= -Wall -Wextra -g -I./src -I./build/gen -I./kit/runtime/include

# ========== 操作系统检测 ==========
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    OS_NAME := macos
    EXE_EXT :=
else ifeq ($(findstring MINGW,$(UNAME_S)),MINGW)
    OS_NAME := windows
    EXE_EXT := .exe
else ifeq ($(findstring MSYS,$(UNAME_S)),MSYS)
    OS_NAME := windows
    EXE_EXT := .exe
else ifeq ($(findstring CYGWIN,$(UNAME_S)),CYGWIN)
    OS_NAME := windows
    EXE_EXT := .exe
else
    OS_NAME := linux
    EXE_EXT :=
endif

# ========== bison/flex 工具链 ==========
# macOS 系统自带 bison 2.3 过旧：使用 Homebrew 版本（brew install bison flex）。
# 用绝对路径调用，同时规避 make 直接 execvp 时沿用启动 PATH 的问题。
ARCH := $(shell uname -m)
ifeq ($(OS_NAME),macos)
    BREW_BISON := $(firstword $(wildcard /usr/local/opt/bison/bin/bison /opt/homebrew/opt/bison/bin/bison))
    BREW_FLEX  := $(firstword $(wildcard /usr/local/opt/flex/bin/flex  /opt/homebrew/opt/flex/bin/flex))
    BISON_CMD  := $(if $(BREW_BISON),$(BREW_BISON),bison)
    FLEX_CMD   := $(if $(BREW_FLEX),$(BREW_FLEX),flex)
    # bison 用的 m4：brew m4 优先
    BREW_M4 := $(firstword $(wildcard /usr/local/opt/m4/bin/m4 /opt/homebrew/opt/m4/bin/m4))
    M4_PATH := $(if $(BREW_M4),$(BREW_M4),m4)
    BISON_M4_ENV := M4=$(M4_PATH)
else ifeq ($(OS_NAME),windows)
    BISON_CMD := bison
    FLEX_CMD  := flex
    M4_PATH   :=
    BISON_M4_ENV :=
else
    BISON_CMD := bison
    FLEX_CMD  := flex
    M4_PATH   := m4
    BISON_M4_ENV := M4=$(M4_PATH)
endif

# ========== 目录定义 ==========
SRC_DIR     := src
GEN_DIR     := build/gen
YACC_DIR    := build/gen
PARSE_SRC   := src/parse
TEST_DIR    := tests
BIN_DIR     := bin
LIB_DIR     := lib
RUNTIME_DIR := kit/runtime

BIN_NAME    := lumyr
BIN_LOCAL   := $(BIN_DIR)/$(BIN_NAME)$(EXE_EXT)
RUNTIME_LIB := $(LIB_DIR)/libruntime.a

LEX_SRC     := $(SRC_DIR)/lex/lex.l
YACC_SRC    := $(PARSE_SRC)/yacc.y

LEX_GEN     := $(YACC_DIR)/lex.yy.c
YACC_GEN_C  := $(YACC_DIR)/yacc.tab.c
YACC_GEN_H  := $(YACC_DIR)/yacc.tab.h
YACC_REPORT := $(GEN_DIR)/yacc.output

# ========== 链接库（跨平台） ==========
ifeq ($(OS_NAME),macos)
    # 系统 libcurl（SecureTransport）+ 系统 libiconv + libc POSIX regex
    LDLIBS := -lcurl -liconv
else ifeq ($(OS_NAME),windows)
    LDLIBS :=
else
    # Linux：glibc 已内置 iconv 与 POSIX regex，无独立 libiconv，只需 curl；
    # libm 提供数学函数；glibc 2.34 之前 pthread 为独立库（新版为空操作兼容）
    LDLIBS := -lcurl -lm -lpthread
endif

# ========== GMP 高精度数学库（动态链接，LGPL v3 合规） ==========
GMP_DIR := $(CURDIR)/deps/gmp
ifeq ($(OS_NAME),macos)
    # Homebrew GMP（brew install gmp）
    GMP_BREW := $(firstword $(wildcard /usr/local/opt/gmp /opt/homebrew/opt/gmp))
    ifeq ($(GMP_BREW),)
        $(warning GMP not found: run "brew install gmp")
    endif
    GMP_LIB_DIR := $(GMP_BREW)/lib
    CFLAGS += -I$(GMP_BREW)/include
    LDFLAGS += -L$(GMP_BREW)/lib
else ifeq ($(OS_NAME),linux)
    # 系统 GMP（apt install libgmp-dev），走默认搜索路径
    GMP_LIB_DIR :=
else ifeq ($(OS_NAME),windows)
    # GMP 与其他 Windows 库同由 build_deps_mingw.sh 构建于 prebuilt/windows
    # （lib/libgmp.dll.a 导入库 + bin/libgmp-10.dll），-I/-L 已在 Windows 块给出
    GMP_LIB_DIR := $(CURDIR)/prebuilt/windows
endif
LDLIBS += -lgmp
# ========== Runtime 静态库源文件 ==========
# lm_runtime.c 已 include 了 gc_runtime.c / lm_string.c / lm_array.c / lm_math.c / lm_io.c
# 这些文件不再单独编译，避免重复定义
RUNTIME_SRCS := $(RUNTIME_DIR)/src/lm_runtime.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_value.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_type.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_map.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_thread.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_lock.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_tls.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_http.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_json.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_charset.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_crypto.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_regex.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_time.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_qs.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lumyr_ffi.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lumyr_log.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lumyr_typed_arrays.c
# Value 类型定义已迁移到 kit/runtime/
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lumyr_value.c
# bigint 任意精度整数
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_bigint.c
# decimal 高精度十进制浮点
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_decimal.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_bitdecimal.c
# 容器与数值扩展类型（tuple/set/bytes/complex）
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_container.c

RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_calendar.c
# 文件与目录对象（file/folder）
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_file.c

RUNTIME_OBJS := $(RUNTIME_SRCS:.c=.o)

# ========== 编译器本体源文件（不含 runtime） ==========
C_SRCS := $(wildcard $(SRC_DIR)/ast/*.c)
C_SRCS += $(wildcard $(SRC_DIR)/ir/*.c)
C_SRCS += $(wildcard $(SRC_DIR)/parse/*.c)
C_SRCS += $(wildcard $(SRC_DIR)/i18n/*.c)
C_SRCS += $(wildcard $(SRC_DIR)/annotation/*.c)
C_SRCS += $(SRC_DIR)/main.c
C_SRCS += $(filter-out $(SRC_DIR)/yacc/lex.yy.c $(SRC_DIR)/yacc/yacc.tab.c, $(wildcard $(SRC_DIR)/yacc/*.c))
C_SRCS += $(LEX_GEN) $(YACC_GEN_C)

OBJS := $(C_SRCS:.c=.o)

# ========== Windows 兼容层 ==========
WIN_DEPS := prebuilt/windows
ifeq ($(OS_NAME),windows)
    export PATH := $(CURDIR)/$(WIN_DEPS)/tools/winflexbison;$(PATH)
    CFLAGS += -I$(WIN_DEPS)/include -DCURL_STATICLIB
    LDFLAGS += -L$(WIN_DEPS)/lib
    # 静态 curl(Schannel TLS)/tre + 动态 iconv（libiconv.dll.a 导入库，LGPL 合规）
    LDLIBS += -lcurl -liconv -ltre \
              -lcrypt32 -lws2_32 -lwldap32 -lwinmm -lnormaliz -liphlpapi -lbcrypt -lsecur32
endif

# ========== 生成 C 代码的编译配置（注入 main.o，消除 main.c 硬编码） ==========
# lumyr -c 模式用 system() 调 gcc 编译用户代码，链接库须与 lumyr 本体一致
GEN_INC := -Ikit/runtime/include
GEN_LIB := -Llib
ifeq ($(OS_NAME),windows)
    GEN_INC += -I$(WIN_DEPS)/include -DCURL_STATICLIB
    GEN_LIB += -L$(WIN_DEPS)/lib
endif

.PHONY: all clean distclean check-env parser-gen env-info runtime-lib

# ========== 主目标 ==========
all: check-env parser-gen runtime-lib $(BIN_LOCAL)

runtime-lib: $(RUNTIME_LIB)

env-info:
	@echo "=== Build Environment ==="
	@echo "OS: $(OS_NAME) ($(UNAME_S))"
	@echo "CC: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo "LDLIBS: $(LDLIBS)"
	@echo "M4: $(M4_PATH)"
	@echo "bison: $(BISON_CMD)"
	@echo "flex: $(FLEX_CMD)"
	@echo "GMP lib: $(GMP_LIB_DIR) [dynamic, LGPL]"
	@echo "curl/iconv/regex: system libraries"
	@echo "EXE_EXT: $(EXE_EXT)"
	@echo "Runtime lib: $(RUNTIME_LIB)"
	@echo "Target: $(BIN_LOCAL)"

check-env:
	@echo "=== Toolchain Check ($(OS_NAME)) ==="
	@echo "flex:   $(FLEX_CMD)"
	@$(FLEX_CMD) --version
	@echo "bison:  $(BISON_CMD)"
	@$(BISON_CMD) --version | head -1
	@$(BISON_CMD) --version | grep -q " 3." || (echo "ERROR: bison >=3.x required"; exit 1)
ifeq ($(OS_NAME),macos)
	@case "$(M4_PATH)" in /*) test -x "$(M4_PATH)" || (echo "ERROR: m4 missing, brew install m4"; exit 1) ;; *) command -v "$(M4_PATH)" >/dev/null || (echo "ERROR: m4 missing, brew install m4"; exit 1) ;; esac
endif

# ========== Runtime 静态库 ==========
$(RUNTIME_LIB): $(RUNTIME_OBJS)
	@echo "==> Building runtime static library"
	@mkdir -p $(LIB_DIR)
	ar rcs $@ $(RUNTIME_OBJS)
	@echo "    Built: $@"

# ========== bison/flex 解析器生成 ==========
parser-gen: $(YACC_GEN_C) $(LEX_GEN)
	@mkdir -p $(YACC_DIR)

$(YACC_GEN_C) $(YACC_GEN_H): $(YACC_SRC)
	@mkdir -p $(GEN_DIR) $(YACC_DIR)
	M4=$(M4_PATH) $(BISON_CMD) -v --report-file=$(YACC_REPORT) -d $< -o $(YACC_GEN_C)

$(LEX_GEN): $(LEX_SRC) $(YACC_GEN_H)
	@mkdir -p $(YACC_DIR)
	$(FLEX_CMD) -o $@ $<

# ========== main.o 特殊编译规则（注入生成代码的链接配置） ==========
src/main.o: src/main.c
	$(CC) $(CFLAGS) \
	  -DLUMYR_GEN_INC='"$(GEN_INC)"' \
	  -DLUMYR_GEN_LIB='"$(GEN_LIB)"' \
	  -DLUMYR_GEN_LDLIBS='"$(LDLIBS)"' \
	  -c -o $@ $<

# ========== 编译器本体链接 ==========
$(BIN_LOCAL): $(OBJS) $(RUNTIME_LIB)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -L$(LIB_DIR) -lruntime $(LDLIBS) -o $@
	@echo "    Built: $@"
ifeq ($(OS_NAME),macos)
	@# GMP 随包分发（LGPL）：拷入 bin/ 并改为 @loader_path 定位，用户可替换
	@rm -f $(BIN_DIR)/libgmp.10.dylib
	@cp $(GMP_LIB_DIR)/libgmp.10.dylib $(BIN_DIR)/
	@chmod u+w $(BIN_DIR)/libgmp.10.dylib
	@OLD_ID=$$(otool -D $(BIN_DIR)/libgmp.10.dylib | tail -1); \
	 install_name_tool -id @loader_path/libgmp.10.dylib $(BIN_DIR)/libgmp.10.dylib; \
	 install_name_tool -change "$$OLD_ID" @loader_path/libgmp.10.dylib $@
	@echo "    bundled libgmp.10.dylib (@loader_path)"
	@# GMP 许可证（LGPL）随分发包提供
	@rm -rf $(BIN_DIR)/licenses
	@cp -R $(GMP_DIR)/licenses $(BIN_DIR)/licenses
	@echo "    copied licenses -> bin/licenses/"
endif
ifeq ($(OS_NAME),windows)
	@echo "==> Copying runtime DLLs to $(BIN_DIR)/ (LGPL: GMP/iconv)"
	@rm -f $(BIN_DIR)/libiconv-2.dll $(BIN_DIR)/libgmp-10.dll
	@cp $(WIN_DEPS)/bin/libiconv-2.dll $(BIN_DIR)/ && echo "    copied libiconv-2.dll"
	@cp $(WIN_DEPS)/bin/libgmp-10.dll $(BIN_DIR)/ && echo "    copied libgmp-10.dll"
	@rm -rf $(BIN_DIR)/licenses
	@cp -R $(WIN_DEPS)/licenses $(BIN_DIR)/licenses
	@cp -R $(GMP_DIR)/licenses $(BIN_DIR)/licenses/gmp
	@echo "    copied licenses -> bin/licenses/"
endif
# ========== 单元测试 ==========
TEST_STACKFRAME := $(TEST_DIR)/stackframe_test$(EXE_EXT)

$(TEST_STACKFRAME): $(OBJS) $(RUNTIME_LIB) tests/stackframe_test.c
	$(CC) $(CFLAGS) $(LDFLAGS) $(filter-out src/main.o,$(OBJS)) tests/stackframe_test.c -L$(LIB_DIR) -lruntime $(LDLIBS) -o $@

.PHONY: test
test: $(TEST_STACKFRAME)
	./$(TEST_STACKFRAME)

# ========== 清理 ==========
clean:
	rm -f $(OBJS) $(RUNTIME_OBJS)
	rm -rf $(BIN_DIR) $(LIB_DIR)
	@echo "clean done"

distclean: clean
	rm -f $(LEX_GEN) $(YACC_GEN_C) $(YACC_GEN_H) $(YACC_REPORT)
	rm -rf $(GEN_DIR)
	@echo "distclean done: restore to source-only state"
