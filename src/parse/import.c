/*
 * lumyr 模块系统（第二阶段）——文本预处理 + Name Mangling
 * 详见 import.h。
 *
 * 第二阶段在第一阶段（import/export 文本剥离 + 内联）基础上新增：
 *   1. Name Mangling：模块内全部顶层符号（func / type / enum / 顶层赋值 /
 *      解构赋值）重写为 __lm_mod_<id>_<name>，实现私有符号外部不可见。
 *   2. 去重导入：全局已处理表（key=realpath），同一模块只内联一次，
 *      后续 import 仅生成 `alias = <expvar>;`。
 *   3. 循环导入检测：维护 active 栈，导入前检查目标路径是否在栈中。
 *   4. export map 的值使用 mangled 名：{"add": __lm_mod_0_add}。
 *
 * 主文件不做 mangle，其符号为程序全局符号。
 */
#include "import.h"
#include "lumyr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#ifdef _WIN32
#include <direct.h>
#define realpath(N,R) _fullpath((R),(N),PATH_MAX)
#else
#include <unistd.h>
#endif

/* ---------------- 动态字符串缓冲 ---------------- */
typedef struct {
    char* buf;
    int   len;
    int   cap;
} SB;

static void sb_init(SB* s) {
    s->cap = 256;
    s->len = 0;
    s->buf = (char*)malloc(s->cap);
    s->buf[0] = '\0';
}
static void sb_reserve(SB* s, int extra) {
    if(s->len + extra + 1 > s->cap) {
        while(s->len + extra + 1 > s->cap) s->cap *= 2;
        s->buf = (char*)realloc(s->buf, s->cap);
    }
}
static void sb_putc(SB* s, char c) {
    sb_reserve(s, 1);
    s->buf[s->len++] = c;
    s->buf[s->len] = '\0';
}
static void sb_puts(SB* s, const char* str) {
    int n = (int)strlen(str);
    sb_reserve(s, n);
    memcpy(s->buf + s->len, str, n);
    s->len += n;
    s->buf[s->len] = '\0';
}
static void sb_putn(SB* s, const char* str, int n) {
    sb_reserve(s, n);
    memcpy(s->buf + s->len, str, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

/* ---------------- 小工具 ---------------- */
static int is_id_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int is_id_char(char c)  { return isalnum((unsigned char)c) || c == '_'; }

/* 读整个文件到 malloc'd 缓冲（NUL 结尾）；失败返回 NULL。 */
static char* slurp_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if(!f) return NULL;
    if(fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if(sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

/* 全局：导出 map 变量唯一序号 */
static int g_mod_seq = 0;

/* 模块别名注册表（预处理阶段填充，yacc 解析阶段查询） */
static char** g_aliases = NULL;
static int     g_naliases = 0;
static int     g_aliases_cap = 0;

void lm_register_alias(const char* name) {
    for(int i = 0; i < g_naliases; i++)
        if(strcmp(g_aliases[i], name) == 0) return;   /* 幂等 */
    if(g_naliases >= g_aliases_cap) {
        int nc = g_aliases_cap ? g_aliases_cap * 2 : 8;
        g_aliases = (char**)realloc(g_aliases, (size_t)nc * sizeof(char*));
        g_aliases_cap = nc;
    }
    g_aliases[g_naliases++] = strdup(name);
}
int lm_is_module_alias(const char* name) {
    for(int i = 0; i < g_naliases; i++)
        if(strcmp(g_aliases[i], name) == 0) return 1;
    return 0;
}

/* ---------------- 全局已处理模块表（去重） ---------------- */
typedef struct {
    char* path;        /* malloc'd, realpath */
    int   module_id;
    char* expvar;      /* malloc'd, 导出 map 变量名；处理完成时填充 */
} ProcessedMod;

static ProcessedMod* g_processed = NULL;
static int           g_nprocessed = 0;
static int           g_cap_processed = 0;
static int           g_next_module_id = 0;   /* 从 0 开始 */

/* 查已处理表；命中返回条目，否则 NULL。 */
static ProcessedMod* find_processed(const char* realpath) {
    for(int i = 0; i < g_nprocessed; i++)
        if(strcmp(g_processed[i].path, realpath) == 0) return &g_processed[i];
    return NULL;
}

/* 登记一个新模块，分配 module_id。expvar 留空，待处理完成后回填。 */
static ProcessedMod* register_processed(const char* realpath) {
    if(g_nprocessed >= g_cap_processed) {
        int nc = g_cap_processed ? g_cap_processed * 2 : 8;
        g_processed = (ProcessedMod*)realloc(g_processed, (size_t)nc * sizeof(ProcessedMod));
        g_cap_processed = nc;
    }
    ProcessedMod* p = &g_processed[g_nprocessed++];
    p->path = strdup(realpath);
    p->module_id = g_next_module_id++;
    p->expvar = NULL;
    return p;
}

/* ---------------- 数据结构 ---------------- */
typedef struct {
    char* path;   /* malloc'd */
    char* alias;  /* malloc'd */
} ImportSpec;

typedef struct {
    char*     text;       /* 变换后文本（import 已替换为占位符） */
    ImportSpec* imports;
    int       nimports;
    int       cap_imports;
    char**    exports;    /* 导出符号名 */
    int       nexports;
    int       cap_exports;
    char**    all_symbols; /* 模块内所有顶层符号名（含导出/私有） */
    int       nsymbols;
    int       cap_symbols;
} TransformResult;

static void tr_push_import(TransformResult* t, char* path, char* alias) {
    if(t->nimports >= t->cap_imports) {
        int nc = t->cap_imports ? t->cap_imports * 2 : 8;
        t->imports = (ImportSpec*)realloc(t->imports, (size_t)nc * sizeof(ImportSpec));
        t->cap_imports = nc;
    }
    t->imports[t->nimports].path  = path;
    t->imports[t->nimports].alias = alias;
    t->nimports++;
}
static void tr_push_export(TransformResult* t, const char* name, int n) {
    if(t->nexports >= t->cap_exports) {
        int nc = t->cap_exports ? t->cap_exports * 2 : 8;
        t->exports = (char**)realloc(t->exports, (size_t)nc * sizeof(char*));
        t->cap_exports = nc;
    }
    t->exports[t->nexports] = (char*)malloc((size_t)n + 1);
    memcpy(t->exports[t->nexports], name, (size_t)n);
    t->exports[t->nexports][n] = '\0';
    t->nexports++;
}
/* 记录一个顶层符号名（去重）。 */
static void tr_push_symbol(TransformResult* t, const char* name, int n) {
    for(int i = 0; i < t->nsymbols; i++) {
        size_t sl = strlen(t->all_symbols[i]);
        if((int)sl == n && strncmp(t->all_symbols[i], name, (size_t)n) == 0) return;
    }
    if(t->nsymbols >= t->cap_symbols) {
        int nc = t->cap_symbols ? t->cap_symbols * 2 : 8;
        t->all_symbols = (char**)realloc(t->all_symbols, (size_t)nc * sizeof(char*));
        t->cap_symbols = nc;
    }
    t->all_symbols[t->nsymbols] = (char*)malloc((size_t)n + 1);
    memcpy(t->all_symbols[t->nsymbols], name, (size_t)n);
    t->all_symbols[t->nsymbols][n] = '\0';
    t->nsymbols++;
}

/*
 * 单遍扫描 src：
 *   - 跳过注释 / 字符串 / 字符字面量内部，避免误判其中的 import/export 字样。
 *   - `export func NAME` -> `func NAME`，记录 NAME（导出 + 符号）。
 *   - `export const NAME` -> `NAME`（去掉 export/const），记录 NAME（导出）。
 *   - `import "path" as alias;` -> 占位符 `__LMIMP_<i>__;`，记录 path/alias。
 *
 * 第二阶段新增：花括号深度 depth、圆括号深度 pdepth 跟踪。仅在 depth==0 &&
 * pdepth==0 时识别顶层声明并记录到 all_symbols：
 *   - `func NAME`（命名函数；匿名 `func(` 不记录）
 *   - `type NAME` / `enum NAME`
 *   - 顶层赋值 `NAME = expr`（非 ==/>= 等）
 *   - 解构赋值 `a, b = ...`
 * 其他内容逐字节原样复制。
 */
static TransformResult* transform(const char* src) {
    TransformResult* t = (TransformResult*)calloc(1, sizeof(TransformResult));
    SB out; sb_init(&out);

    int n = (int)strlen(src);
    int i = 0;
    int pi = 0;   /* 占位符序号 */
    int depth = 0;     /* 花括号深度 */
    int pdepth = 0;    /* 圆括号深度 */
    int expect_kind = 0; /* 0=无, 1=func名, 2=type名, 3=enum名 */

    while(i < n) {
        char c = src[i];

        /* 行注释 */
        if(c == '/' && i + 1 < n && src[i + 1] == '/') {
            int j = i;
            while(j < n && src[j] != '\n') j++;
            sb_putn(&out, src + i, j - i);
            i = j;
            continue;
        }
        /* 块注释 */
        if(c == '/' && i + 1 < n && src[i + 1] == '*') {
            int j = i + 2;
            while(j + 1 < n && !(src[j] == '*' && src[j + 1] == '/')) j++;
            j = (j + 2 <= n) ? j + 2 : n;
            sb_putn(&out, src + i, j - i);
            i = j;
            continue;
        }
        /* 字符串字面量 */
        if(c == '"') {
            int j = i + 1;
            while(j < n && src[j] != '"') {
                if(src[j] == '\\' && j + 1 < n) j += 2;
                else j++;
            }
            j = (j < n) ? j + 1 : n;
            sb_putn(&out, src + i, j - i);
            i = j;
            continue;
        }
        /* 字符字面量 'x' / '\n'：原样复制（不会含模块关键字） */
        if(c == '\'') {
            int j = i + 1;
            if(j < n && src[j] == '\\') j++;       /* 转义 */
            if(j < n) j++;                          /* 跳过内容字符 */
            if(j < n && src[j] == '\'') j++;        /* 闭引号 */
            sb_putn(&out, src + i, j - i);
            i = j;
            continue;
        }
        /* 标识符起始 */
        if(is_id_start(c)) {
            int j = i;
            while(j < n && is_id_char(src[j])) j++;
            int wlen = j - i;
            /* 关键字前必须是真正的词边界（避免 obj.import / ximport 误判） */
            int bound_ok = (i == 0) || (!is_id_char(src[i - 1]) && src[i - 1] != '.');

            if(bound_ok && wlen == 6 && strncmp(src + i, "import", 6) == 0) {
                int k = j;
                while(k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) k++;
                if(k >= n || src[k] != '"') {
                    LOG_ERROR("[module] 语法错误：import 后应为字符串字面量路径\n");
                    free(out.buf); free(t);
                    return NULL;
                }
                k++;
                int pstart = k;
                while(k < n && src[k] != '"') k++;
                if(k >= n) { LOG_ERROR("[module] 语法错误：import 路径字符串未闭合\n"); free(out.buf); free(t); return NULL; }
                int plen = k - pstart;
                k++;
                while(k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) k++;
                /* 期望 as */
                if(k + 1 < n && src[k] == 'a' && src[k + 1] == 's' &&
                   (k + 2 >= n || !is_id_char(src[k + 2]))) {
                    k += 2;
                } else {
                    LOG_ERROR("[module] 语法错误：import 路径后应为 `as 别名`\n");
                    free(out.buf); free(t); return NULL;
                }
                while(k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) k++;
                if(k >= n || !is_id_start(src[k])) {
                    LOG_ERROR("[module] 语法错误：as 后应为别名标识符\n");
                    free(out.buf); free(t); return NULL;
                }
                int astart = k;
                while(k < n && is_id_char(src[k])) k++;
                int alen = k - astart;
                while(k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) k++;
                if(k < n && src[k] == ';') k++;

                char* path  = (char*)malloc((size_t)plen + 1);
                memcpy(path, src + pstart, (size_t)plen); path[plen] = '\0';
                char* alias = (char*)malloc((size_t)alen + 1);
                memcpy(alias, src + astart, (size_t)alen); alias[alen] = '\0';
                tr_push_import(t, path, alias);

                /* 占位符 */
                char ph[64];
                snprintf(ph, sizeof ph, "__LMIMP_%d__;", pi++);
                sb_puts(&out, ph);
                i = k;
                continue;
            }
            if(bound_ok && wlen == 6 && strncmp(src + i, "export", 6) == 0) {
                int k = j;
                while(k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) k++;
                /* 读取下一个词 */
                if(k < n && is_id_start(src[k])) {
                    int k2 = k;
                    while(k2 < n && is_id_char(src[k2])) k2++;
                    int w2len = k2 - k;
                    if(w2len == 4 && strncmp(src + k, "func", 4) == 0) {
                        int m = k2;
                        while(m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\r' || src[m] == '\n')) m++;
                        int ns = m;
                        while(m < n && is_id_char(src[m])) m++;
                        if(m > ns) {
                            tr_push_export(t, src + ns, m - ns);
                            tr_push_symbol(t, src + ns, m - ns);   /* 命名函数符号 */
                        }
                        sb_puts(&out, "func ");
                        i = ns;   /* 继续从函数名处原样输出 */
                        continue;
                    } else if(w2len == 5 && strncmp(src + k, "const", 5) == 0) {
                        int m = k2;
                        while(m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\r' || src[m] == '\n')) m++;
                        int ns = m;
                        while(m < n && is_id_char(src[m])) m++;
                        if(m > ns) tr_push_export(t, src + ns, m - ns);
                        i = ns;   /* 继续从常量名处原样输出（PI = 3.14;），赋值检测会记录符号 */
                        continue;
                    } else {
                        /* export 后跟其他：仅去掉 export 关键字，按普通标识符输出 */
                        sb_putc(&out, ' ');
                        i = k;
                        continue;
                    }
                }
                /* export 后无词：原样吐出 export 词 */
                sb_putn(&out, src + i, wlen);
                i = j;
                continue;
            }

            /* 第二阶段：顶层声明关键字识别（仅 depth==0 && pdepth==0） */
            if(bound_ok && depth == 0 && pdepth == 0) {
                if(wlen == 4 && strncmp(src + i, "func", 4) == 0) {
                    /* 命名函数 func NAME(...) vs 匿名 lambda func(...) */
                    int k = j;
                    while(k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) k++;
                    if(k < n && is_id_start(src[k])) expect_kind = 1;   /* 命名函数 */
                    /* else 匿名 func(...)：不设置 */
                    sb_putn(&out, src + i, wlen);
                    i = j;
                    continue;
                }
                if(wlen == 4 && strncmp(src + i, "type", 4) == 0) {
                    expect_kind = 2;
                    sb_putn(&out, src + i, wlen);
                    i = j;
                    continue;
                }
                if(wlen == 4 && strncmp(src + i, "enum", 4) == 0) {
                    expect_kind = 3;
                    sb_putn(&out, src + i, wlen);
                    i = j;
                    continue;
                }
            }

            /* expect_kind：当前标识符即声明名 */
            if(expect_kind && bound_ok) {
                tr_push_symbol(t, src + i, wlen);
                expect_kind = 0;
                sb_putn(&out, src + i, wlen);
                i = j;
                continue;
            }

            /* 第二阶段：顶层赋值 / 解构赋值符号发现（depth==0 && pdepth==0） */
            if(bound_ok && depth == 0 && pdepth == 0) {
                int k = j;
                while(k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) k++;
                if(k < n && src[k] == '=' && src[k + 1] != '=' && src[k + 1] != '>') {
                    /* 简单赋值：NAME = expr */
                    tr_push_symbol(t, src + i, wlen);
                } else if(k < n && src[k] == ',') {
                    /* 解构赋值：a, b, c = expr
                     * 收集 ident(,ident)* 直到真正的 '='（非 ==/>=）；命中才全部记录。 */
                    char names[64][128];
                    int nnames = 0;
                    if(wlen < 128) {
                        memcpy(names[nnames], src + i, (size_t)wlen);
                        names[nnames][wlen] = '\0';
                        nnames++;
                    }
                    int kk = k; /* 指向 ',' */
                    int destr_ok = 0;
                    while(1) {
                        kk++; /* 跳过 ',' */
                        while(kk < n && (src[kk]==' '||src[kk]=='\t'||src[kk]=='\r'||src[kk]=='\n')) kk++;
                        if(kk >= n || !is_id_start(src[kk])) break;
                        int ns = kk;
                        while(kk < n && is_id_char(src[kk])) kk++;
                        int nl = kk - ns;
                        if(nnames < 64 && nl < 128) {
                            memcpy(names[nnames], src + ns, (size_t)nl);
                            names[nnames][nl] = '\0';
                            nnames++;
                        }
                        while(kk < n && (src[kk]==' '||src[kk]=='\t'||src[kk]=='\r'||src[kk]=='\n')) kk++;
                        if(kk < n && src[kk] == '=' && src[kk+1] != '=' && src[kk+1] != '>') {
                            destr_ok = 1; break;
                        }
                        if(kk < n && src[kk] == ',') continue;
                        break;
                    }
                    if(destr_ok) {
                        for(int s = 0; s < nnames; s++)
                            tr_push_symbol(t, names[s], (int)strlen(names[s]));
                    }
                }
            }

            /* 普通标识符：原样复制 */
            sb_putn(&out, src + i, wlen);
            i = j;
            continue;
        }

        /* 花括号 / 圆括号深度跟踪（字符串/注释内的已由上方分支跳过） */
        if(c == '{') depth++;
        else if(c == '}') { if(depth > 0) depth--; }
        else if(c == '(') pdepth++;
        else if(c == ')') { if(pdepth > 0) pdepth--; }

        /* 其他字符：原样 */
        sb_putc(&out, c);
        i++;
    }

    t->text = out.buf;
    return t;
}

/*
 * mangle_text：把 text 中所有出现在 symbols 列表里的标识符引用替换为
 * `__lm_mod_<id>_<name>`。跳过字符串/注释/字符字面量；不替换 '.' 后的标识符；
 * 词边界由“整标识符匹配”天然保证（name2 不会命中 name）。
 */
static char* mangle_text(const char* text, char** symbols, int nsym, int mod_id) {
    SB out; sb_init(&out);
    char prefix[64];
    snprintf(prefix, sizeof prefix, "__lm_mod_%d_", mod_id);

    int n = (int)strlen(text);
    int i = 0;
    while(i < n) {
        char c = text[i];
        /* 行注释 */
        if(c == '/' && i + 1 < n && text[i + 1] == '/') {
            int j = i;
            while(j < n && text[j] != '\n') j++;
            sb_putn(&out, text + i, j - i);
            i = j;
            continue;
        }
        if(c == '/' && i + 1 < n && text[i + 1] == '*') {
            int j = i + 2;
            while(j + 1 < n && !(text[j] == '*' && text[j + 1] == '/')) j++;
            j = (j + 2 <= n) ? j + 2 : n;
            sb_putn(&out, text + i, j - i);
            i = j;
            continue;
        }
        if(c == '"') {
            int j = i + 1;
            while(j < n && text[j] != '"') {
                if(text[j] == '\\' && j + 1 < n) j += 2;
                else j++;
            }
            j = (j < n) ? j + 1 : n;
            sb_putn(&out, text + i, j - i);
            i = j;
            continue;
        }
        if(c == '\'') {
            int j = i + 1;
            if(j < n && text[j] == '\\') j++;
            if(j < n) j++;
            if(j < n && text[j] == '\'') j++;
            sb_putn(&out, text + i, j - i);
            i = j;
            continue;
        }
        if(is_id_start(c)) {
            int j = i;
            while(j < n && is_id_char(text[j])) j++;
            int wlen = j - i;
            /* '.' 后的标识符不 mangle（map 成员访问） */
            int after_dot = (i > 0 && text[i - 1] == '.');
            int hit = 0;
            if(!after_dot) {
                for(int s = 0; s < nsym; s++) {
                    int sl = (int)strlen(symbols[s]);
                    if(sl == wlen && strncmp(text + i, symbols[s], (size_t)wlen) == 0) { hit = 1; break; }
                }
            }
            if(hit) {
                sb_puts(&out, prefix);
                sb_putn(&out, text + i, wlen);
            } else {
                sb_putn(&out, text + i, wlen);
            }
            i = j;
            continue;
        }
        sb_putc(&out, c);
        i++;
    }
    return out.buf;
}

/* 取目录部分（dirname），写入 out（绝对路径）。 */
static void dir_of(const char* path, char* out, size_t outsz) {
    /* Windows 路径可能用 \\，同时处理 / 和 \\，取最后出现的 */
    const char* sf = strrchr(path, '/');
    const char* sb = strrchr(path, '\\');
    const char* slash = sf > sb ? sf : sb;
    if(!slash) { snprintf(out, outsz, "."); return; }
    size_t l = (size_t)(slash - path);
    if(l == 0) l = 1;
    snprintf(out, outsz, "%.*s", (int)l, path);
}

/* 释放 TransformResult 的所有堆成员。 */
static void tr_free(TransformResult* tr) {
    if(!tr) return;
    free(tr->text);
    for(int q = 0; q < tr->nimports; q++) { free(tr->imports[q].path); free(tr->imports[q].alias); }
    free(tr->imports);
    for(int q = 0; q < tr->nexports; q++) free(tr->exports[q]);
    free(tr->exports);
    for(int q = 0; q < tr->nsymbols; q++) free(tr->all_symbols[q]);
    free(tr->all_symbols);
    free(tr);
}

/* 递归展开：把 abs_path（已 realpath）模块的完整内联文本（含末尾导出 map）
 * 写入 out，返回导出 map 变量名（malloc'd）。失败返回 NULL。
 *
 * active 栈已含本模块自身（由调用方压入）；n_active 为栈长。
 */
static char* expand_file(const char* abs_path, SB* out,
                         const char** active, int n_active) {
    /* 1) 去重：已处理过则直接返回缓存的 expvar，不重复内联模块体。
     *    expvar 为 NULL 表示上次处理失败，不命中去重（直接返回错误）。 */
    ProcessedMod* hit = find_processed(abs_path);
    if(hit) {
        if(!hit->expvar) return NULL;
        return strdup(hit->expvar);
    }

    /* 2) 登记模块，分配 module_id。 */
    ProcessedMod* pm = register_processed(abs_path);
    int mod_id = pm->module_id;

    /* 3) 读模块源码 + transform */
    char* content = slurp_file(abs_path);
    if(!content) {
        LOG_ERROR("[module] 无法打开模块文件: %s\n", abs_path);
        return NULL;
    }
    TransformResult* tr = transform(content);
    free(content);
    if(!tr) return NULL;

    /* 4) Name Mangling：用模块内符号表变换模块体 */
    char* mangled = mangle_text(tr->text, tr->all_symbols, tr->nsymbols, mod_id);
    free(tr->text);
    tr->text = mangled;

    /* 模块所在目录 */
    char dir[PATH_MAX];
    dir_of(abs_path, dir, sizeof dir);

    /* 5) 递归展开该模块自己的 import（占位符） */
    SB body; sb_init(&body);
    const char* txt = tr->text;
    int tl = (int)strlen(txt);
    int i = 0;
    int ok = 1;
    while(i < tl) {
        if(strncmp(txt + i, "__LMIMP_", 8) == 0) {
            int k = i + 8;   /* __LMIMP_ 为 8 字符，数字从 i+8 开始 */
            int idx = 0;
            while(k < tl && txt[k] >= '0' && txt[k] <= '9') { idx = idx * 10 + (txt[k] - '0'); k++; }
            /* 跳到占位符结束的分号 */
            while(k < tl && txt[k] != ';') k++;
            if(k < tl) k++;

            if(idx < 0 || idx >= tr->nimports) {
                LOG_ERROR("[module] 内部错误：占位符越界\n"); ok = 0; break;
            }
            ImportSpec* isp = &tr->imports[idx];

            /* 解析模块路径（相对当前模块目录） */
            char joined[PATH_MAX];
            if(isp->path[0] == '/') snprintf(joined, sizeof joined, "%s", isp->path);
            else snprintf(joined, sizeof joined, "%s/%s", dir, isp->path);

            char real[PATH_MAX];
            if(!realpath(joined, real)) {
                LOG_ERROR("[module] 无法解析导入路径: %s (in %s)\n", isp->path, abs_path);
                ok = 0; break;
            }
            /* 循环导入检测（在递归之前） */
            int cyc = 0;
            for(int a = 0; a < n_active; a++) {
                if(strcmp(active[a], real) == 0) { cyc = 1; break; }
            }
            if(cyc) {
                LOG_ERROR("[module] 检测到循环导入: %s\n", real);
                ok = 0; break;
            }

            /* 递归展开子模块（栈压入子模块自身） */
            char* child_expvar = NULL;
            {
                const char* new_active[64];
                if(n_active + 1 > 63) { LOG_ERROR("[module] 导入嵌套过深\n"); ok = 0; break; }
                for(int a = 0; a < n_active; a++) new_active[a] = active[a];
                new_active[n_active] = real;

                SB child_body; sb_init(&child_body);
                child_expvar = expand_file(real, &child_body, new_active, n_active + 1);
                if(!child_expvar) { ok = 0; free(child_body.buf); break; }

                /* 子模块体（已含子模块自己的导出 map；命中去重时为空）+ 别名赋值 */
                sb_puts(&body, child_body.buf);
                free(child_body.buf);
                lm_register_alias(isp->alias);
                sb_putc(&body, '\n');
                sb_puts(&body, isp->alias);
                sb_puts(&body, " = ");
                sb_puts(&body, child_expvar);
                sb_puts(&body, ";\n");
                free(child_expvar);
            }
            i = k;
        } else {
            sb_putc(&body, txt[i]);
            i++;
        }
    }

    if(!ok) {
        free(body.buf);
        tr_free(tr);
        return NULL;
    }

    /* 6) 生成本模块的导出 map 变量（值用 mangled 名） */
    char expvar[64];
    snprintf(expvar, sizeof expvar, "__lmod_exp_%d", g_mod_seq++);

    sb_puts(out, body.buf);
    free(body.buf);

    /* 追加：expvar = {"orig": __lm_mod_<id>_orig, ...}; */
    sb_putc(out, '\n');
    sb_puts(out, expvar);
    sb_puts(out, " = {");
    for(int q = 0; q < tr->nexports; q++) {
        if(q) sb_puts(out, ", ");
        sb_putc(out, '"');
        sb_puts(out, tr->exports[q]);
        sb_puts(out, "\": ");
        sb_puts(out, "__lm_mod_");
        {
            char idstr[16];
            snprintf(idstr, sizeof idstr, "%d_", mod_id);
            sb_puts(out, idstr);
        }
        sb_puts(out, tr->exports[q]);
    }
    sb_puts(out, "};\n");

    /* 7) 回填已处理表的 expvar */
    pm->expvar = strdup(expvar);
    char* ret = strdup(expvar);
    tr_free(tr);
    return ret;
}

char* lm_preprocess_main(const char* src_path, int* had_mod_out) {
    *had_mod_out = 0;

    char real[PATH_MAX];
    if(!realpath(src_path, real)) {
        perror("realpath");
        *had_mod_out = -1;
        return NULL;
    }
    char* content = slurp_file(real);
    if(!content) {
        perror("open");
        *had_mod_out = -1;
        return NULL;
    }
    TransformResult* tr = transform(content);
    free(content);
    if(!tr) { *had_mod_out = -1; return NULL; }

    int has_any = (tr->nimports > 0) || (tr->nexports > 0);

    /* 主文件：展开其 import（主文件不 mangle，其符号为全局符号） */
    SB out; sb_init(&out);
    const char* txt = tr->text;
    int tl = (int)strlen(txt);
    int i = 0, ok = 1;
    char dir[PATH_MAX];
    dir_of(real, dir, sizeof dir);

    while(i < tl) {
        if(strncmp(txt + i, "__LMIMP_", 8) == 0) {
            int k = i + 8;   /* __LMIMP_ 为 8 字符，数字从 i+8 开始 */
            int idx = 0;
            while(k < tl && txt[k] >= '0' && txt[k] <= '9') { idx = idx * 10 + (txt[k] - '0'); k++; }
            while(k < tl && txt[k] != ';') k++;
            if(k < tl) k++;

            if(idx < 0 || idx >= tr->nimports) { ok = 0; break; }
            ImportSpec* isp = &tr->imports[idx];

            char joined[PATH_MAX];
            if(isp->path[0] == '/') snprintf(joined, sizeof joined, "%s", isp->path);
            else snprintf(joined, sizeof joined, "%s/%s", dir, isp->path);
            char mreal[PATH_MAX];
            if(!realpath(joined, mreal)) {
                LOG_ERROR("[module] 无法解析导入路径: %s (in %s)\n", isp->path, real);
                ok = 0; break;
            }
            /* 循环检测（主文件自身 + 当前子模块入栈） */
            const char* active[2];
            active[0] = real;
            active[1] = mreal;
            SB child_body; sb_init(&child_body);
            char* expvar = expand_file(mreal, &child_body, active, 2);
            if(!expvar) { ok = 0; free(child_body.buf); break; }
            lm_register_alias(isp->alias);
            sb_puts(&out, child_body.buf);
            free(child_body.buf);
            sb_putc(&out, '\n');
            sb_puts(&out, isp->alias);
            sb_puts(&out, " = ");
            sb_puts(&out, expvar);
            sb_puts(&out, ";\n");
            free(expvar);
            i = k;
        } else {
            sb_putc(&out, txt[i]);
            i++;
        }
    }

    tr_free(tr);

    if(!ok) {
        free(out.buf);
        *had_mod_out = -1;
        return NULL;
    }
    if(!has_any) {
        free(out.buf);
        *had_mod_out = 0;
        return NULL;
    }
    *had_mod_out = 1;
    return out.buf;
}
