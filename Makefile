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

# ========== m4 路径（跨平台） ==========
ifeq ($(OS_NAME),macos)
    BREW_M4_INTEL := /usr/local/opt/m4/bin/m4
    BREW_M4_ARM   := /opt/homebrew/opt/m4/bin/m4
    ifeq ($(shell test -x $(BREW_M4_INTEL) && echo yes),yes)
        M4_PATH := $(BREW_M4_INTEL)
    else ifeq ($(shell test -x $(BREW_M4_ARM) && echo yes),yes)
        M4_PATH := $(BREW_M4_ARM)
    else
        M4_PATH := m4
    endif
    BISON_M4_ENV := M4=$(M4_PATH)
else ifeq ($(OS_NAME),windows)
    M4_PATH :=
    BISON_M4_ENV :=
else
    M4_PATH := m4
    BISON_M4_ENV := M4=$(M4_PATH)
endif

# ========== 链接库（跨平台） ==========
LDLIBS := -lcurl -liconv

ifneq ($(CURL_DIR),)
    CFLAGS += -I$(CURL_DIR)/include
    LDFLAGS += -L$(CURL_DIR)/lib
endif
ifneq ($(ICONV_DIR),)
    CFLAGS += -I$(ICONV_DIR)/include
    LDFLAGS += -L$(ICONV_DIR)/lib
endif

# ========== Runtime 静态库源文件 ==========
# lm_runtime.c 已 include 了 gc_runtime.c / lm_string.c / lm_array.c / lm_math.c / lm_io.c
# 这些文件不再单独编译，避免重复定义
RUNTIME_SRCS := $(RUNTIME_DIR)/src/lm_runtime.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_value.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_class.c
RUNTIME_SRCS += $(RUNTIME_DIR)/src/lm_struct.c
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

RUNTIME_OBJS := $(RUNTIME_SRCS:.c=.o)

# ========== 编译器本体源文件（不含 runtime） ==========
C_SRCS := $(wildcard $(SRC_DIR)/ast/*.c)
C_SRCS := $(filter-out $(RUNTIME_DIR)/src/lumyr_value.c, $(C_SRCS))
C_SRCS += $(wildcard $(SRC_DIR)/ir/*.c)
C_SRCS += $(wildcard $(SRC_DIR)/parse/*.c)
C_SRCS += $(wildcard $(SRC_DIR)/i18n/*.c)
C_SRCS += $(wildcard $(SRC_DIR)/annotation/*.c)
C_SRCS += $(SRC_DIR)/main.c
C_SRCS += $(filter-out $(SRC_DIR)/yacc/lex.yy.c $(SRC_DIR)/yacc/yacc.tab.c, $(wildcard $(SRC_DIR)/yacc/*.c))
C_SRCS += $(LEX_GEN) $(YACC_GEN_C)

OBJS := $(C_SRCS:.c=.o)

# ========== Windows 兼容层 ==========
WIN_DEPS := third_party/windows
ifeq ($(OS_NAME),windows)
    export PATH := $(CURDIR)/$(WIN_DEPS)/tools/winflexbison;$(PATH)
    CFLAGS += -I$(WIN_DEPS)/include -DCURL_STATICLIB
    LDFLAGS += -L$(WIN_DEPS)/lib
    LDLIBS += -ltre -lcrypt32 -lws2_32 -lwldap32 -lwinmm -lnormaliz -liphlpapi -lbcrypt -lsecur32
endif

.PHONY: all clean distclean check-env parser-gen env-info runtime-lib

# ========== 主目标 ==========
all: check-env runtime-lib parser-gen $(BIN_LOCAL)

runtime-lib: $(RUNTIME_LIB)

env-info:
	@echo "=== Build Environment ==="
	@echo "OS: $(OS_NAME) ($(UNAME_S))"
	@echo "CC: $(CC)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo "LDLIBS: $(LDLIBS)"
	@echo "M4: $(M4_PATH)"
	@echo "EXE_EXT: $(EXE_EXT)"
	@echo "Runtime lib: $(RUNTIME_LIB)"
	@echo "Target: $(BIN_LOCAL)"

check-env:
	@echo "=== Toolchain Check ($(OS_NAME)) ==="
	@flex --version
	@bison --version | head -1
	@bison --version | grep -q " 3." || (echo "ERROR: bison >=3.x required"; exit 1)
ifeq ($(OS_NAME),macos)
	@test -x $(M4_PATH) || (echo "ERROR: m4 missing, brew install m4"; exit 1)
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
	$(BISON_M4_ENV) bison -v --report-file=$(YACC_REPORT) -d $< -o $(YACC_GEN_C)

$(LEX_GEN): $(LEX_SRC) $(YACC_GEN_H)
	@mkdir -p $(YACC_DIR)
	flex -o $@ $<

# ========== 编译器本体链接 ==========
$(BIN_LOCAL): $(OBJS) $(RUNTIME_LIB)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -L$(LIB_DIR) -lruntime $(LDLIBS) -o $@
	@echo "    Built: $@"

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
