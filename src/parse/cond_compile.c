/*
 * lumyr 条件编译实现（见 cond_compile.h 头注释）
 *
 * 实现要点：
 *   - 符号表：预定义符号（构建期 C 宏探测）+ -D 注册符号，纯开关语义；
 *   - 表达式：递归下降（|| && ! 括号 符号 0/1），未定义符号按 0；
 *   - 指令扫描：逐行处理，行首（允许前导空白）# 开头识别为指令；
 *     指令行与死分支行均输出空行 → 行号精确保留；
 *   - 字面量替换：仅对活跃非指令行执行；扫描时跳过 "..." 字符串与 // 行注释；
 *     标识符须满足边界（前字符非 '.' 非标识符字符）且紧随其后为 ()；
 *   - lumin 无块注释、无多行字符串，故行首 # 不会出现在字符串/注释内部。
 */
#include "cond_compile.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 平台/架构探测（构建期） ---------------- */

/* OS 名（platform() 字面量）与 OS 符号 */
#if defined(__EMSCRIPTEN__)
#  define LM_OS_NAME   "browser"
#  define LM_OS_SYM    "browser"
#elif defined(_WIN32)
#  define LM_OS_NAME   "windows"
#  define LM_OS_SYM    "windows"
#elif defined(__APPLE__)
#  include <TargetConditionals.h>
#  if TARGET_OS_IOS
#    define LM_OS_NAME "ios"
#    define LM_OS_SYM  "ios"
#  else
#    define LM_OS_NAME "macos"
#    define LM_OS_SYM  "macos"
#  endif
#elif defined(__ANDROID__)
#  define LM_OS_NAME   "android"
#  define LM_OS_SYM    "android"
#elif defined(__linux__)
#  define LM_OS_NAME   "linux"
#  define LM_OS_SYM    "linux"
#elif defined(__FreeBSD__)
#  define LM_OS_NAME   "freebsd"
#  define LM_OS_SYM    "freebsd"
#else
#  define LM_OS_NAME   "unknown"
#  define LM_OS_SYM    NULL
#endif

/* 架构名（arch() 字面量）与架构符号。顺序：MCU 特判先于通用 arm。 */
#if defined(__wasm32__) || defined(__EMSCRIPTEN__)
#  define LM_ARCH_NAME "wasm32"
#  define LM_ARCH_SYM  "wasm32"
#  define LM_MCU 0
#elif defined(__AVR__)
#  define LM_ARCH_NAME "avr"
#  define LM_ARCH_SYM  "avr"
#  define LM_MCU 1
#elif defined(__MSP430__)
#  define LM_ARCH_NAME "msp430"
#  define LM_ARCH_SYM  "msp430"
#  define LM_MCU 1
/* Microchip PIC：注意不能用 __PIC__（Position Independent Code，桌面编译器默认定义），
 * 须用 XC 编译器族专属宏 */
#elif defined(__XC8) || defined(__XC8__) || defined(__XC16) || defined(__XC16__) \
   || defined(__XC32__) || defined(__PIC24F__) || defined(__PIC24H__) \
   || defined(__dsPIC30F__) || defined(__dsPIC33F__)
#  define LM_ARCH_NAME "pic"
#  define LM_ARCH_SYM  "pic"
#  define LM_MCU 1
/* 8051 系（STC 等）：Keil C51 / SDCC mcs51 端口 */
#elif defined(__C51__) || defined(__SDCC_mcs51) || defined(__mcs51__) || defined(__8051__)
#  define LM_ARCH_NAME "mcs51"
#  define LM_ARCH_SYM  "mcs51"
#  define LM_MCU 1
#elif defined(__XTENSA__)
#  define LM_ARCH_NAME "xtensa"
#  define LM_ARCH_SYM  "xtensa"
#  define LM_MCU 1
#elif defined(__ARM_ARCH_6M__) || defined(__ARM_ARCH_7M__) || defined(__ARM_ARCH_7EM__) \
   || defined(__ARM_ARCH_8M_BASE__) || defined(__ARM_ARCH_8M_MAIN__)
#  define LM_ARCH_NAME "cortexm"
#  define LM_ARCH_SYM  "cortexm"
#  define LM_MCU 1
#elif defined(__x86_64__) || defined(_M_X64)
#  define LM_ARCH_NAME "x86_64"
#  define LM_ARCH_SYM  "x86_64"
#  define LM_MCU 0
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define LM_ARCH_NAME "arm64"
#  define LM_ARCH_SYM  "arm64"
#  define LM_MCU 0
#elif defined(__loongarch__)
#  if defined(__loongarch_grlen) && __loongarch_grlen == 32
#    define LM_ARCH_NAME "loongarch32"
#    define LM_ARCH_SYM  "loongarch32"
#  else
#    define LM_ARCH_NAME "loongarch64"
#    define LM_ARCH_SYM  "loongarch64"
#  endif
#  define LM_MCU 0
#elif defined(__sw_64__)
#  define LM_ARCH_NAME "sw64"
#  define LM_ARCH_SYM  "sw64"
#  define LM_MCU 0
#elif defined(__riscv)
#  if defined(__riscv_xlen) && __riscv_xlen == 32
#    define LM_ARCH_NAME "riscv32"
#    define LM_ARCH_SYM  "riscv32"
#  else
#    define LM_ARCH_NAME "riscv64"
#    define LM_ARCH_SYM  "riscv64"
#  endif
#  define LM_MCU 0
#elif defined(__arm__) || defined(_M_ARM)
#  define LM_ARCH_NAME "arm"
#  define LM_ARCH_SYM  "arm"
#  define LM_MCU 0
#elif defined(__i386__) || defined(_M_IX86)
#  define LM_ARCH_NAME "x86"
#  define LM_ARCH_SYM  "x86"
#  define LM_MCU 0
#else
#  define LM_ARCH_NAME "unknown"
#  define LM_ARCH_SYM  NULL
#  define LM_MCU 0
#endif

/* 形态（formFactor() 字面量） */
#if defined(__EMSCRIPTEN__)
#  define LM_FORM_NAME "browser"
#  define LM_FORM_SYM  "browser"
#elif LM_MCU
#  define LM_FORM_NAME "mcu"
#  define LM_FORM_SYM  "mcu"
#elif defined(TARGET_OS_IOS) && TARGET_OS_IOS
#  define LM_FORM_NAME "mobile"
#  define LM_FORM_SYM  "mobile"
#elif defined(__ANDROID__)
#  define LM_FORM_NAME "mobile"
#  define LM_FORM_SYM  "mobile"
#else
#  define LM_FORM_NAME "desktop"
#  define LM_FORM_SYM  "desktop"
#endif

/* 位宽符号 */
#if defined(__SIZEOF_POINTER__)
#  if __SIZEOF_POINTER__ == 8
#    define LM_BITS_SYM "bits64"
#  elif __SIZEOF_POINTER__ == 4
#    define LM_BITS_SYM "bits32"
#  elif __SIZEOF_POINTER__ == 2
#    define LM_BITS_SYM "bits16"
#  else
#    define LM_BITS_SYM "bits8"
#  endif
#else
#  define LM_BITS_SYM NULL
#endif

/* ---------------- 符号表 ---------------- */
#define LM_COND_MAX_SYMS 256
static const char* g_syms[LM_COND_MAX_SYMS];
static int         g_nsyms = 0;
static int         g_is_cc = 0;
static int         g_predef_done = 0;

static int is_id_start_c(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_id_char_c(char c)  { return isalnum((unsigned char)c) || c == '_'; }

static void sym_add(const char* name) {
    if(!name || !*name) return;
    for(int i = 0; i < g_nsyms; i++)
        if(strcmp(g_syms[i], name) == 0) return;   /* 幂等 */
    if(g_nsyms < LM_COND_MAX_SYMS)
        g_syms[g_nsyms++] = name;
}

static int sym_defined(const char* name, int len) {
    for(int i = 0; i < g_nsyms; i++)
        if((int)strlen(g_syms[i]) == len && strncmp(g_syms[i], name, (size_t)len) == 0)
            return 1;
    return 0;
}

/* 构建期预定义符号注册（幂等，首次 filter 时执行） */
static void ensure_predefined(void) {
    if(g_predef_done) return;
    g_predef_done = 1;
    if(LM_OS_SYM)   sym_add(LM_OS_SYM);
    if(LM_ARCH_SYM) sym_add(LM_ARCH_SYM);
    if(LM_FORM_SYM) sym_add(LM_FORM_SYM);
    if(LM_BITS_SYM) sym_add(LM_BITS_SYM);
#if LM_MCU
    sym_add("mcu");
#endif
#if defined(__APPLE__) && (!defined(TARGET_OS_IOS) || !TARGET_OS_IOS)
    /* macOS 同时属于 desktop（已由 LM_FORM_SYM 覆盖），无需额外符号 */
#endif
    sym_add(g_is_cc ? "cc" : "vm");
    if(g_is_cc) sym_add("c");   /* 当前 CC 唯一目标语言；未来 js/java/go 等在此扩展 */
}

void lm_cond_set_channel(int is_cc) {
    g_is_cc = is_cc ? 1 : 0;
}

void lm_cond_add_define(const char* name) {
    if(!name || !is_id_start_c(name[0])) {
        fprintf(stderr, "警告：忽略非法 -D 符号名 \"%s\" / warning: ignoring invalid -D symbol \"%s\"\n",
                name ? name : "(null)", name ? name : "(null)");
        return;
    }
    for(const char* p = name; *p; p++) {
        if(!is_id_char_c(*p)) {
            fprintf(stderr, "警告：忽略非法 -D 符号名 \"%s\" / warning: ignoring invalid -D symbol \"%s\"\n",
                    name, name);
            return;
        }
    }
    sym_add(name);
}

/* ---------------- 表达式求值（递归下降） ----------------
 * expr := or
 * or   := and ('||' and)*
 * and  := unary ('&&' unary)*
 * unary:= '!' unary | '(' expr ')' | SYMBOL | 0 | 1
 */
typedef struct {
    const char* p;      /* 当前扫描位置 */
    int         err;    /* 语法错误标志 */
} ExprParser;

static void ep_skip_ws(ExprParser* ep) {
    while(*ep->p == ' ' || *ep->p == '\t') ep->p++;
}

static int ep_parse_or(ExprParser* ep);

static int ep_parse_unary(ExprParser* ep) {
    ep_skip_ws(ep);
    if(ep->err) return 0;
    if(*ep->p == '!') { ep->p++; return !ep_parse_unary(ep); }
    if(*ep->p == '(') {
        ep->p++;
        int v = ep_parse_or(ep);
        ep_skip_ws(ep);
        if(*ep->p != ')') { ep->err = 1; return 0; }
        ep->p++;
        return v;
    }
    if(*ep->p == '0' && !is_id_char_c(ep->p[1])) { ep->p++; return 0; }
    if(*ep->p == '1' && !is_id_char_c(ep->p[1])) { ep->p++; return 1; }
    if(is_id_start_c(*ep->p)) {
        const char* s = ep->p;
        while(is_id_char_c(*ep->p)) ep->p++;
        return sym_defined(s, (int)(ep->p - s));
    }
    ep->err = 1;
    return 0;
}

static int ep_parse_and(ExprParser* ep) {
    int v = ep_parse_unary(ep);
    for(;;) {
        ep_skip_ws(ep);
        if(ep->p[0] == '&' && ep->p[1] == '&') {
            ep->p += 2;
            int r = ep_parse_unary(ep);
            v = v && r;
            if(ep->err) return 0;
        } else return v;
    }
}

static int ep_parse_or(ExprParser* ep) {
    int v = ep_parse_and(ep);
    for(;;) {
        ep_skip_ws(ep);
        if(ep->p[0] == '|' && ep->p[1] == '|') {
            ep->p += 2;
            int r = ep_parse_and(ep);
            v = v || r;
            if(ep->err) return 0;
        } else return v;
    }
}

/* 求值表达式文本；语法错误返回 -1。 */
static int eval_expr(const char* expr) {
    ExprParser ep = { expr, 0 };
    int v = ep_parse_or(&ep);
    ep_skip_ws(&ep);
    if(ep.err || *ep.p != '\0') return -1;
    return v;
}

/* ---------------- 字面量替换 ---------------- */

typedef struct {
    const char* name;        /* 函数名 */
    const char* (*get_lit)(void);  /* 取替换字面量（不含引号） */
} LitEntry;

static const char* lit_platform(void)  { return LM_OS_NAME; }
static const char* lit_arch(void)      { return LM_ARCH_NAME; }
static const char* lit_form(void)      { return LM_FORM_NAME; }
static const char* lit_channel(void)   { return g_is_cc ? "cc" : "vm"; }
static const char* lit_lang(void)      { return g_is_cc ? "c" : "vm"; }

static const LitEntry g_lits[] = {
    { "platform",   lit_platform },
    { "arch",       lit_arch },
    { "formFactor", lit_form },
    { "channel",    lit_channel },
    { "targetLang", lit_lang },
};

static const LitEntry* lit_find(const char* id, int len) {
    for(size_t i = 0; i < sizeof(g_lits) / sizeof(g_lits[0]); i++)
        if((int)strlen(g_lits[i].name) == len && strncmp(g_lits[i].name, id, (size_t)len) == 0)
            return &g_lits[i];
    return NULL;
}

/* ---------------- 行缓冲输出（动态字符串） ---------------- */
typedef struct {
    char*  buf;
    size_t len, cap;
    int    changed;     /* 是否有任何改动 */
} OutBuf;

static void ob_reserve(OutBuf* o, size_t need) {
    if(o->len + need + 1 <= o->cap) return;
    size_t nc = o->cap ? o->cap * 2 : 4096;
    while(nc < o->len + need + 1) nc *= 2;
    o->buf = (char*)realloc(o->buf, nc);
    o->cap = nc;
}
static void ob_putn(OutBuf* o, const char* s, size_t n) {
    ob_reserve(o, n);
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}
static void ob_puts(OutBuf* o, const char* s) { ob_putn(o, s, strlen(s)); }
static void ob_putc(OutBuf* o, char c) { ob_putn(o, &c, 1); }

/* ---------------- 指令栈 ---------------- */
#define LM_COND_MAX_DEPTH 64
typedef struct {
    int parent_active;  /* 进入本级前外层是否激活 */
    int branch_on;      /* 本级当前分支是否激活（已含 parent_active） */
    int any_taken;      /* 本级是否已有分支被激活过 */
    int in_else;        /* 是否已进入 #else */
    int if_line;        /* #if 所在行号（错误信息用） */
} CondLevel;

/* 对一行活跃源码做字面量替换，追加到 out。行内容不含换行符。 */
static void emit_line_with_literals(OutBuf* out, const char* line, size_t n) {
    size_t i = 0;
    while(i < n) {
        char c = line[i];
        /* 字符串字面量：原样拷贝（lumin 无 \" 转义，配对到下一个 "） */
        if(c == '"') {
            size_t j = i + 1;
            while(j < n && line[j] != '"') j++;
            if(j < n) j++;      /* 含收尾引号 */
            ob_putn(out, line + i, j - i);
            i = j;
            continue;
        }
        /* 行注释：其余部分原样 */
        if(c == '/' && i + 1 < n && line[i + 1] == '/') {
            ob_putn(out, line + i, n - i);
            break;
        }
        /* 标识符：检查字面量替换 */
        if(is_id_start_c(c)) {
            char prev = (i > 0) ? line[i - 1] : ' ';
            size_t j = i + 1;
            while(j < n && is_id_char_c(line[j])) j++;
            const LitEntry* le = NULL;
            if(prev != '.' && !is_id_char_c(prev))
                le = lit_find(line + i, (int)(j - i));
            if(le) {
                /* 紧随其后（允许空白）须为 () */
                size_t k = j;
                while(k < n && (line[k] == ' ' || line[k] == '\t')) k++;
                if(k < n && line[k] == '(') {
                    size_t m = k + 1;
                    while(m < n && (line[m] == ' ' || line[m] == '\t')) m++;
                    if(m < n && line[m] == ')') {
                        ob_putc(out, '"');
                        ob_puts(out, le->get_lit());
                        ob_putc(out, '"');
                        out->changed = 1;
                        i = m + 1;
                        continue;
                    }
                }
            }
            ob_putn(out, line + i, j - i);
            i = j;
            continue;
        }
        ob_putc(out, c);
        i++;
    }
}

int lm_cond_might_have(const char* text) {
    return strstr(text, "#if") != NULL
        || strstr(text, "platform(") != NULL
        || strstr(text, "arch(") != NULL
        || strstr(text, "formFactor(") != NULL
        || strstr(text, "channel(") != NULL
        || strstr(text, "targetLang(") != NULL;
}

char* lm_cond_filter_text(const char* text, const char* path, int* err_out) {
    *err_out = 0;
    if(!text) return NULL;
    if(!lm_cond_might_have(text)) return NULL;
    ensure_predefined();

    const char* fname = path ? path : "<input>";
    CondLevel stack[LM_COND_MAX_DEPTH];
    int depth = 0;
    int active = 1;             /* 当前是否激活（所有嵌套层合取） */

    OutBuf out = { NULL, 0, 0, 0 };
    ob_reserve(&out, strlen(text) + 64);

    int lineno = 1;
    const char* p = text;
    while(*p) {
        /* 取一行（不含换行符） */
        const char* eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);

        /* 行首指令检测：前导空白后 # 开头 */
        size_t ws = 0;
        while(ws < n && (p[ws] == ' ' || p[ws] == '\t')) ws++;
        int is_directive = (ws < n && p[ws] == '#');

        if(is_directive) {
            /* 解析指令名 */
            size_t k = ws + 1;
            while(k < n && (p[k] == ' ' || p[k] == '\t')) k++;
            size_t name_s = k;
            while(k < n && isalpha((unsigned char)p[k])) k++;
            size_t name_n = k - name_s;
            /* 指令参数（表达式/符号），去掉尾部空白 */
            while(k < n && (p[k] == ' ' || p[k] == '\t')) k++;
            size_t arg_s = k;
            size_t arg_e = n;
            while(arg_e > arg_s && (p[arg_e - 1] == ' ' || p[arg_e - 1] == '\t' || p[arg_e - 1] == '\r')) arg_e--;

            char dname[16];
            if(name_n >= sizeof(dname)) name_n = sizeof(dname) - 1;
            memcpy(dname, p + name_s, name_n); dname[name_n] = '\0';

            char arg[512];
            size_t an = arg_e - arg_s;
            if(an >= sizeof(arg)) an = sizeof(arg) - 1;
            memcpy(arg, p + arg_s, an); arg[an] = '\0';

            out.changed = 1;    /* 指令行必然替换为空行 → 有改动 */

            if(strcmp(dname, "if") == 0 || strcmp(dname, "ifdef") == 0 || strcmp(dname, "ifndef") == 0) {
                if(depth >= LM_COND_MAX_DEPTH) {
                    fprintf(stderr, "%s:%d: 条件编译嵌套过深（>%d）/ conditional nesting too deep (>%d)\n",
                            fname, lineno, LM_COND_MAX_DEPTH, LM_COND_MAX_DEPTH);
                    *err_out = 1; free(out.buf); return NULL;
                }
                int cond;
                if(strcmp(dname, "if") == 0) {
                    cond = eval_expr(arg);
                    if(cond < 0) {
                        fprintf(stderr, "%s:%d: #if 表达式语法错误: \"%s\" / bad #if expression: \"%s\"\n",
                                fname, lineno, arg, arg);
                        *err_out = 1; free(out.buf); return NULL;
                    }
                } else {
                    /* ifdef/ifndef：参数须为单个标识符 */
                    if(!is_id_start_c(arg[0])) {
                        fprintf(stderr, "%s:%d: #%s 参数须为标识符 / argument must be an identifier\n",
                                fname, lineno, dname);
                        *err_out = 1; free(out.buf); return NULL;
                    }
                    size_t idl = 0;
                    while(is_id_char_c(arg[idl])) idl++;
                    if(arg[idl] != '\0') {
                        fprintf(stderr, "%s:%d: #%s 参数须为单个标识符 / argument must be a single identifier\n",
                                fname, lineno, dname);
                        *err_out = 1; free(out.buf); return NULL;
                    }
                    cond = sym_defined(arg, (int)idl);
                    if(strcmp(dname, "ifndef") == 0) cond = !cond;
                }
                stack[depth].parent_active = active;
                stack[depth].branch_on = active && cond;
                stack[depth].any_taken = active && cond;
                stack[depth].in_else = 0;
                stack[depth].if_line = lineno;
                depth++;
                active = stack[depth - 1].branch_on;
            } else if(strcmp(dname, "elif") == 0) {
                if(depth == 0 || stack[depth - 1].in_else) {
                    fprintf(stderr, "%s:%d: #%s 无匹配的 #if 或已在 #else 之后 / unmatched or after #else\n",
                            fname, lineno, dname);
                    *err_out = 1; free(out.buf); return NULL;
                }
                CondLevel* lv = &stack[depth - 1];
                if(lv->any_taken || !lv->parent_active) {
                    lv->branch_on = 0;
                } else {
                    int cond = eval_expr(arg);
                    if(cond < 0) {
                        fprintf(stderr, "%s:%d: #elif 表达式语法错误: \"%s\" / bad #elif expression: \"%s\"\n",
                                fname, lineno, arg, arg);
                        *err_out = 1; free(out.buf); return NULL;
                    }
                    lv->branch_on = lv->parent_active && cond;
                    lv->any_taken = lv->branch_on;
                }
                active = lv->branch_on;
            } else if(strcmp(dname, "else") == 0) {
                if(depth == 0 || stack[depth - 1].in_else) {
                    fprintf(stderr, "%s:%d: #else 无匹配的 #if 或重复 / unmatched or duplicated #else\n",
                            fname, lineno);
                    *err_out = 1; free(out.buf); return NULL;
                }
                CondLevel* lv = &stack[depth - 1];
                lv->in_else = 1;
                lv->branch_on = lv->parent_active && !lv->any_taken;
                lv->any_taken = 1;
                active = lv->branch_on;
            } else if(strcmp(dname, "endif") == 0) {
                if(depth == 0) {
                    fprintf(stderr, "%s:%d: #endif 无匹配的 #if / unmatched #endif\n",
                            fname, lineno);
                    *err_out = 1; free(out.buf); return NULL;
                }
                depth--;
                active = depth > 0 ? stack[depth - 1].branch_on : 1;
            } else {
                fprintf(stderr, "%s:%d: 未知条件编译指令 #%s / unknown conditional directive #%s\n",
                        fname, lineno, dname, dname);
                *err_out = 1; free(out.buf); return NULL;
            }
            /* 指令行 → 空行（保行号） */
        } else if(active) {
            /* 活跃源码行：字面量替换后原样输出（去掉可能的 \r） */
            size_t ln = n;
            if(ln > 0 && p[ln - 1] == '\r') ln--;
            emit_line_with_literals(&out, p, ln);
        }
        /* 死分支行 → 空行（什么都不输出，仅保留换行符） */

        if(eol) {
            ob_putc(&out, '\n');
            p = eol + 1;
        } else {
            p += n;
        }
        lineno++;
    }

    if(depth > 0) {
        fprintf(stderr, "%s:%d: 未闭合的 #if（开启于第 %d 行）/ unclosed #if (opened at line %d)\n",
                fname, lineno, stack[depth - 1].if_line, stack[depth - 1].if_line);
        *err_out = 1; free(out.buf); return NULL;
    }

    if(!out.changed) { free(out.buf); return NULL; }
    return out.buf;
}

char* lm_cond_filter_file(const char* path, int* err_out) {
    *err_out = 0;
    FILE* f = fopen(path, "rb");
    if(!f) { perror(path); *err_out = 1; return NULL; }
    if(fseek(f, 0, SEEK_END) != 0) { fclose(f); *err_out = 1; return NULL; }
    long sz = ftell(f);
    if(sz < 0) { fclose(f); *err_out = 1; return NULL; }
    rewind(f);
    char* buf = (char*)malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    char* r = lm_cond_filter_text(buf, path, err_out);
    free(buf);
    return r;
}
