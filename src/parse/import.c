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
#include "cond_compile.h"
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
#include <pwd.h>
#include <sys/stat.h>
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
static char*** g_alias_exports = NULL;   /* 与 g_aliases 平行：每个别名的导出成员名表 */
static int*    g_alias_nexports = NULL;

void lm_register_alias(const char* name, char* const* exports, int nexports) {
    for(int i = 0; i < g_naliases; i++)
        if(strcmp(g_aliases[i], name) == 0) return;   /* 幂等 */
    if(g_naliases >= g_aliases_cap) {
        int nc = g_aliases_cap ? g_aliases_cap * 2 : 8;
        g_aliases = (char**)realloc(g_aliases, (size_t)nc * sizeof(char*));
        g_alias_exports = (char***)realloc(g_alias_exports, (size_t)nc * sizeof(char**));
        g_alias_nexports = (int*)realloc(g_alias_nexports, (size_t)nc * sizeof(int));
        g_aliases_cap = nc;
    }
    g_aliases[g_naliases] = strdup(name);
    g_alias_exports[g_naliases] = NULL;
    g_alias_nexports[g_naliases] = 0;
    if(exports && nexports > 0) {
        g_alias_exports[g_naliases] = (char**)malloc((size_t)nexports * sizeof(char*));
        for(int j = 0; j < nexports; j++)
            g_alias_exports[g_naliases][j] = strdup(exports[j]);
        g_alias_nexports[g_naliases] = nexports;
    }
    g_naliases++;
}
int lm_is_module_alias(const char* name) {
    for(int i = 0; i < g_naliases; i++)
        if(strcmp(g_aliases[i], name) == 0) return 1;
    return 0;
}
/* 别名成员编译期校验（无兜底：不存在即由调用方报错）：
 * 返回 1=是别名且有此导出成员；0=是别名但无此成员；-1=不是别名。 */
int lm_alias_export_lookup(const char* alias, const char* member) {
    for(int i = 0; i < g_naliases; i++)
        if(strcmp(g_aliases[i], alias) == 0) {
            for(int j = 0; j < g_alias_nexports[i]; j++)
                if(strcmp(g_alias_exports[i][j], member) == 0) return 1;
            return 0;
        }
    return -1;
}

/* ---------------- 全局已处理模块表（去重） ---------------- */
typedef struct {
    char*  path;        /* malloc'd, realpath */
    int    module_id;
    char*  expvar;      /* malloc'd, 导出 map 变量名；处理完成时填充 */
    /* selective 累计（去重 + shim 生成） */
    char** sel_old;     /* 累计已提升的原 mangle 名 */
    char** sel_new;     /* 累计已提升的新名 */
    int    nselective;
    int    cap_selective;
    char** mangled;     /* 实际被 mangle 的符号集（首次处理时记录） */
    int    nmangled;
    char** class_names; /* class/interface 名（shim 生成时跳过，因为类名非一等值） */
    int    nclass_names;
    char** export_names; /* 实际写入导出 map 的键名（供别名成员编译期校验） */
    int    nexport_names;
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
    p->sel_old = NULL; p->sel_new = NULL; p->nselective = 0; p->cap_selective = 0;
    p->mangled = NULL; p->nmangled = 0;
    p->class_names = NULL; p->nclass_names = 0;
    p->export_names = NULL; p->nexport_names = 0;
    return p;
}

/* pm selective 辅助：判断某符号是否已提升 */
static int pm_has_selective(ProcessedMod* pm, const char* name) {
    for(int i = 0; i < pm->nselective; i++)
        if(strcmp(pm->sel_old[i], name) == 0) return 1;
    return 0;
}
/* pm selective 辅助：添加已提升符号 */
static void pm_add_selective(ProcessedMod* pm, const char* old_name, const char* new_name) {
    if(pm_has_selective(pm, old_name)) return;
    if(pm->nselective >= pm->cap_selective) {
        int nc = pm->cap_selective ? pm->cap_selective * 2 : 8;
        pm->sel_old = (char**)realloc(pm->sel_old, (size_t)nc * sizeof(char*));
        pm->sel_new = (char**)realloc(pm->sel_new, (size_t)nc * sizeof(char*));
        pm->cap_selective = nc;
    }
    pm->sel_old[pm->nselective] = strdup(old_name);
    pm->sel_new[pm->nselective] = new_name ? strdup(new_name) : NULL;
    pm->nselective++;
}
/* pm 辅助：判断某符号是否在 mangle 集中 */
static int pm_was_mangled(ProcessedMod* pm, const char* name) {
    for(int i = 0; i < pm->nmangled; i++)
        if(strcmp(pm->mangled[i], name) == 0) return 1;
    return 0;
}
/* pm 辅助：判断某符号是否为 class/interface 名 */
static int pm_is_class_name(ProcessedMod* pm, const char* name) {
    for(int i = 0; i < pm->nclass_names; i++)
        if(strcmp(pm->class_names[i], name) == 0) return 1;
    return 0;
}

/* ---------------- 数据结构 ---------------- */
typedef struct {
    char*  path;          /* malloc'd：路径文本（不含 " 或 < > 分隔符） */
    int    is_framework; /* 1 = <...> 框架导入；0 = "..." 第三方 */
    char*  alias;         /* malloc'd；NULL 表示无 alias（no-alias 或 selective-only） */
    int    is_no_alias;  /* 1 = 无 as 关键字（import <fw>; 全部 export 符号全局原名） */
    char** sel_old;      /* selective 原名数组（malloc'd）；NULL/0 = 无 */
    char** sel_new;      /* selective 新名数组（malloc'd；NULL 项 = 沿用原名） */
    int    nselective;
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
    int*      sym_hash;    /* all_symbols → 下标开放寻址哈希（-1 空槽），O(1) 去重 */
    int       sym_hash_cap;
    char**    class_names; /* class/interface 名（用于从 export map 排除，因为类名非一等值） */
    int       nclass_names;
    int       cap_class_names;
} TransformResult;

static void tr_push_import(TransformResult* t, char* path, int is_framework,
                           char* alias, int is_no_alias,
                           char** sel_old, char** sel_new, int nsel) {
    if(t->nimports >= t->cap_imports) {
        int nc = t->cap_imports ? t->cap_imports * 2 : 8;
        t->imports = (ImportSpec*)realloc(t->imports, (size_t)nc * sizeof(ImportSpec));
        t->cap_imports = nc;
    }
    ImportSpec* isp = &t->imports[t->nimports];
    isp->path = path;
    isp->is_framework = is_framework;
    isp->alias = alias;
    isp->is_no_alias = is_no_alias;
    isp->sel_old = sel_old;
    isp->sel_new = sel_new;
    isp->nselective = nsel;
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
/* 记录 class/interface 名（用于从 export map 排除） */
static void tr_push_class_name(TransformResult* t, const char* name, int n) {
    if(t->nclass_names >= t->cap_class_names) {
        int nc = t->cap_class_names ? t->cap_class_names * 2 : 8;
        t->class_names = (char**)realloc(t->class_names, (size_t)nc * sizeof(char*));
        t->cap_class_names = nc;
    }
    t->class_names[t->nclass_names] = (char*)malloc((size_t)n + 1);
    memcpy(t->class_names[t->nclass_names], name, (size_t)n);
    t->class_names[t->nclass_names][n] = '\0';
    t->nclass_names++;
}
/* 判断 name 是否为 class/interface 名 */
static int tr_is_class_name(TransformResult* t, const char* name) {
    for(int i = 0; i < t->nclass_names; i++)
        if(strcmp(t->class_names[i], name) == 0) return 1;
    return 0;
}
/* FNV-1a：对长度 n 的（可能非 NUL 结尾）符号片段求哈希 */
static unsigned tr_hash_n(const char* s, int n) {
    unsigned h = 2166136261u;
    for(int k = 0; k < n; k++) { h ^= (unsigned char)s[k]; h *= 16777619u; }
    return h;
}

/* 记录一个顶层符号名（去重）。O(1) 开放寻址，替代线性 strlen/strncmp 扫描（O(N^2)）。 */
static void tr_push_symbol(TransformResult* t, const char* name, int n) {
    /* 1. 确保哈希能容纳 nsymbols+1（load < 0.5）；扩容则全量 rehash 现有符号 */
    int need_cap = (t->nsymbols + 1) * 2;
    if(t->sym_hash_cap < need_cap) {
        int nc = t->sym_hash_cap ? t->sym_hash_cap : 16;
        while(nc < need_cap) nc *= 2;
        int* nh = (int*)malloc((size_t)nc * sizeof(int));
        for(int i = 0; i < nc; i++) nh[i] = -1;
        for(int i = 0; i < t->nsymbols; i++) {
            int sl = (int)strlen(t->all_symbols[i]);
            int p = (int)(tr_hash_n(t->all_symbols[i], sl) & (unsigned)(nc - 1));
            while(nh[p] != -1) p = (p + 1) & (nc - 1);
            nh[p] = i;
        }
        free(t->sym_hash);
        t->sym_hash = nh; t->sym_hash_cap = nc;
    }
    /* 2. 探测新符号（name 为长度 n 的源码片段） */
    int mask = t->sym_hash_cap - 1;
    int p = (int)(tr_hash_n(name, n) & (unsigned)mask);
    while(t->sym_hash[p] != -1) {
        int idx = t->sym_hash[p];
        if((int)strlen(t->all_symbols[idx]) == n &&
           strncmp(t->all_symbols[idx], name, (size_t)n) == 0) return;  /* 已存在 */
        p = (p + 1) & mask;
    }
    /* 3. 追加符号（all_symbols 独立扩容，不影响已建好的 sym_hash） */
    if(t->nsymbols >= t->cap_symbols) {
        int nc = t->cap_symbols ? t->cap_symbols * 2 : 8;
        t->all_symbols = (char**)realloc(t->all_symbols, (size_t)nc * sizeof(char*));
        t->cap_symbols = nc;
    }
    int idx = t->nsymbols;
    t->all_symbols[idx] = (char*)malloc((size_t)n + 1);
    memcpy(t->all_symbols[idx], name, (size_t)n);
    t->all_symbols[idx][n] = '\0';
    t->nsymbols++;
    /* 4. p 是探测到的空槽，登记新下标 */
    t->sym_hash[p] = idx;
}

/* ---------------- 框架配置文件 ---------------- */
typedef struct { char* name; char* root; } FwEntry;
static FwEntry* g_fw_config = NULL;
static int      g_fw_config_n = 0;
static int      g_fw_config_cap = 0;
static int      g_fw_config_loaded = 0;

/* 简易 JSON 解析：从 json 文本中提取 frameworks 对象的 key→value 对。
 * 只处理 {"frameworks": {"k": "v", ...}} 格式。
 * base_dir：配置文件所在目录——相对路径 root 以它为基准解析
 * （而非调用时 CWD），否则换目录运行编译器后配置路径失效。 */
static void parse_frameworks_json(const char* json, const char* base_dir) {
    const char* p = json;
    /* 找 "frameworks" */
    while(*p) {
        if(*p == '"') {
            p++;
            const char* start = p;
            while(*p && *p != '"') p++;
            int klen = (int)(p - start);
            if(klen == 10 && strncmp(start, "frameworks", 10) == 0) {
                if(*p == '"') p++;
                /* 找冒号后的 { */
                while(*p && *p != '{') p++;
                if(*p == '{') p++;
                /* 解析 "key": "value" 对 */
                while(*p && *p != '}') {
                    while(*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',')) p++;
                    if(*p != '"') break;
                    p++;
                    const char* kstart = p;
                    while(*p && *p != '"') { if(*p == '\\') p++; p++; }
                    int klen2 = (int)(p - kstart);
                    if(*p == '"') p++;
                    while(*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
                    if(*p != ':') break;
                    p++;
                    while(*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
                    if(*p != '"') break;
                    p++;
                    const char* vstart = p;
                    while(*p && *p != '"') { if(*p == '\\') p++; p++; }
                    int vlen = (int)(p - vstart);
                    if(*p == '"') p++;
                    /* 登记到 g_fw_config（相对路径以 base_dir 为基准） */
                    if(g_fw_config_n >= g_fw_config_cap) {
                        int nc = g_fw_config_cap ? g_fw_config_cap * 2 : 8;
                        g_fw_config = (FwEntry*)realloc(g_fw_config, (size_t)nc * sizeof(FwEntry));
                        g_fw_config_cap = nc;
                    }
                    char resolved[PATH_MAX];
                    if(vlen > 0 && vstart[0] == '/') {
                        snprintf(resolved, sizeof resolved, "%.*s", vlen, vstart);
                    } else {
                        snprintf(resolved, sizeof resolved, "%s/%.*s", base_dir, vlen, vstart);
                    }
                    g_fw_config[g_fw_config_n].name = (char*)malloc((size_t)klen2 + 1);
                    memcpy(g_fw_config[g_fw_config_n].name, kstart, (size_t)klen2);
                    g_fw_config[g_fw_config_n].name[klen2] = '\0';
                    g_fw_config[g_fw_config_n].root = strdup(resolved);
                    g_fw_config_n++;
                }
                return;
            }
            if(*p == '"') p++;
        } else {
            p++;
        }
    }
}

/* 加载框架配置：先 lumyr.json（CWD），再 ~/.lumyr/frameworks.json */
static void load_framework_config(void) {
    if(g_fw_config_loaded) return;
    g_fw_config_loaded = 1;
    /* 1. ./lumyr.json（相对 root 以配置文件目录为基准） */
    char* json = slurp_file("lumyr.json");
    if(json) { parse_frameworks_json(json, "."); free(json); return; }
    /* 2. ./lumyr-lms/lumyr.json */
    json = slurp_file("lumyr-lms/lumyr.json");
    if(json) { parse_frameworks_json(json, "lumyr-lms"); free(json); return; }
    /* 3. ~/.lumyr/frameworks.json */
    const char* home = getenv("HOME");
#ifdef _WIN32
    if(!home) home = getenv("USERPROFILE");
#endif
    if(home) {
        char path[PATH_MAX];
        char basedir[PATH_MAX];
        snprintf(path, sizeof path, "%s/.lumyr/frameworks.json", home);
        snprintf(basedir, sizeof basedir, "%s/.lumyr", home);
        json = slurp_file(path);
        if(json) { parse_frameworks_json(json, basedir); free(json); return; }
    }
}

/* 查配置表中 fw_name 对应的 root；不存在返回 NULL */
static const char* fw_lookup(const char* name) {
    load_framework_config();
    for(int i = 0; i < g_fw_config_n; i++)
        if(strcmp(g_fw_config[i].name, name) == 0) return g_fw_config[i].root;
    return NULL;
}

/*
 * 解析框架导入路径。
 *   fw_spec：framework 名，如 "function/object" 或 "myfw/sub/mod"
 *   out：成功时写入 realpath；失败返回 0。
 *
 * 拆分第一个 '/'：fw_name / module_path
 * 查配置 → 系统回退 → realpath
 *   <function/object> → <root>/object.lm
 *   <myfw/sub/mod>    → <root>/sub/mod.lm
 */
static int resolve_framework(const char* fw_spec, char* out, size_t outsz) {
    /* 拆分 fw_name / module_path */
    const char* slash = strchr(fw_spec, '/');
    if(!slash || slash == fw_spec) {
        LOG_ERROR("[module] 非法框架名: %s\n", fw_spec);
        return 0;
    }
    int fw_nlen = (int)(slash - fw_spec);
    char fw_name[256];
    if(fw_nlen >= 256) { LOG_ERROR("[module] 框架名过长\n"); return 0; }
    memcpy(fw_name, fw_spec, (size_t)fw_nlen);
    fw_name[fw_nlen] = '\0';
    const char* module_path = slash + 1;
    /* 路径安全检查 */
    if(strstr(fw_spec, "..")) {
        LOG_ERROR("[module] 非法框架路径(含 ..): %s\n", fw_spec);
        return 0;
    }

    /* 候选 root 列表 */
    const char* roots[8];
    int nroots = 0;
    char sys_root[PATH_MAX];
    char env_root[PATH_MAX];
    char home_root[PATH_MAX];

    /* 1. 配置文件 */
    const char* cfg_root = fw_lookup(fw_name);
    if(cfg_root) roots[nroots++] = cfg_root;

    /* 2. $LUMYR_FRAMEWORK / lumyr-<fw_name> */
    const char* env_fw = getenv("LUMYR_FRAMEWORK");
    if(env_fw && *env_fw) {
        snprintf(env_root, sizeof env_root, "%s/lumyr-%s", env_fw, fw_name);
        roots[nroots++] = env_root;
    }

    /* 3. ./lumyr-lms/lumyr-<fw_name> */
    snprintf(sys_root, sizeof sys_root, "./lumyr-lms/lumyr-%s", fw_name);
    roots[nroots++] = sys_root;

    /* 4. 编译期常量 */
#ifdef LUMYR_FRAMEWORK_PATH
    snprintf(env_root, sizeof env_root, "%s/lumyr-%s", LUMYR_FRAMEWORK_PATH, fw_name);
    roots[nroots++] = env_root;
#endif

    /* 5. ~/.lumyr/framework/lumyr-<fw_name> */
    const char* home = getenv("HOME");
#ifdef _WIN32
    if(!home) home = getenv("USERPROFILE");
#endif
    if(home) {
        snprintf(home_root, sizeof home_root, "%s/.lumyr/framework/lumyr-%s", home, fw_name);
        roots[nroots++] = home_root;
    }

    /* 遍历候选：拼 module_path.lm → realpath → access */
    for(int i = 0; i < nroots; i++) {
        char joined[PATH_MAX];
        snprintf(joined, sizeof joined, "%s/%s.lm", roots[i], module_path);
        char rp[PATH_MAX];
        if(realpath(joined, rp) && access(rp, R_OK) == 0) {
            snprintf(out, outsz, "%s", rp);
            return 1;
        }
    }
    LOG_ERROR("[module] 找不到框架模块: <%s>（搜索 %d 个路径）\n", fw_spec, nroots);
    return 0;
}

/* ---------------- selective 列表解析 ---------------- */
/*
 * 从 src[*pi] 位置解析 selective 列表：{ A (as B)? (, C (as D)?)* }
 *   pi：输入/输出位置（进入时指向 '{'，离开时指向 '}' 后）
 *   old_out/new_out：输出数组（malloc'd，caller free 各项和数组本身）
 *   返回项数；-1 = 语法错误
 *   new_out[i] 为 NULL 表示无 rename（沿用原名）
 */
static int parse_selective_list(const char* src, int* pi, int n,
                                char*** old_out, char*** new_out) {
    int i = *pi;
    /* 期望 '{' */
    if(i >= n || src[i] != '{') return -1;
    i++;
    char** olds = NULL; char** news = NULL; int cnt = 0, cap = 0;
    for(;;) {
        while(i < n && (src[i]==' '||src[i]=='\t'||src[i]=='\r'||src[i]=='\n')) i++;
        if(i >= n) goto err;
        if(src[i] == '}') { i++; break; }
        if(src[i] == ',') { i++; continue; }
        /* 读 IDENT */
        if(!is_id_start(src[i])) goto err;
        int s0 = i;
        while(i < n && is_id_char(src[i])) i++;
        int ol = i - s0;
        char* old_name = (char*)malloc((size_t)ol + 1);
        memcpy(old_name, src + s0, (size_t)ol); old_name[ol] = '\0';
        char* new_name = NULL;
        /* 可选 'as IDENT' */
        while(i < n && (src[i]==' '||src[i]=='\t'||src[i]=='\r'||src[i]=='\n')) i++;
        if(i + 1 < n && src[i]=='a' && src[i+1]=='s' &&
           (i+2 >= n || !is_id_char(src[i+2]))) {
            i += 2;
            while(i < n && (src[i]==' '||src[i]=='\t'||src[i]=='\r'||src[i]=='\n')) i++;
            if(i >= n || !is_id_start(src[i])) { free(old_name); goto err; }
            int ns = i;
            while(i < n && is_id_char(src[i])) i++;
            int nl = i - ns;
            new_name = (char*)malloc((size_t)nl + 1);
            memcpy(new_name, src + ns, (size_t)nl); new_name[nl] = '\0';
        }
        /* 去重 */
        int dup = 0;
        for(int d = 0; d < cnt; d++) {
            if(strcmp(olds[d], old_name) == 0) { dup = 1; free(old_name); free(new_name); break; }
        }
        if(!dup) {
            if(cnt >= cap) {
                int nc = cap ? cap * 2 : 4;
                olds = (char**)realloc(olds, (size_t)nc * sizeof(char*));
                news = (char**)realloc(news, (size_t)nc * sizeof(char*));
                cap = nc;
            }
            olds[cnt] = old_name;
            news[cnt] = new_name;
            cnt++;
        }
        while(i < n && (src[i]==' '||src[i]=='\t'||src[i]=='\r'||src[i]=='\n')) i++;
        if(i < n && src[i] == ',') { i++; continue; }
        if(i < n && src[i] == '}') { i++; break; }
        goto err;
    }
    if(cnt == 0) goto err;
    *pi = i;
    *old_out = olds; *new_out = news;
    return cnt;
err:
    free(olds); free(news);
    return -1;
}

/* ---------------- 单遍扫描 src ----------------
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
    int expect_kind = 0; /* 0=无, 1=func名, 2=type名, 3=enum名, 4=class名, 5=interface名 */
    int implicit_export = 0; /* public 修饰符：下一个声明名同时记录为 export */

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
                while(k < n && (src[k]==' '||src[k]=='\t'||src[k]=='\r'||src[k]=='\n')) k++;
                if(k >= n) { LOG_ERROR("[module] 语法错误：import 后应为路径\n"); free(out.buf); free(t); return NULL; }
                /* 路径："<path>" 或 <fw_spec> */
                int is_framework = 0;
                int pstart, plen;
                if(src[k] == '"') {
                    is_framework = 0;
                    k++;
                    pstart = k;
                    while(k < n && src[k] != '"') k++;
                    if(k >= n) { LOG_ERROR("[module] 语法错误：import 路径字符串未闭合\n"); free(out.buf); free(t); return NULL; }
                    plen = k - pstart; k++;
                } else if(src[k] == '<') {
                    is_framework = 1;
                    k++;
                    pstart = k;
                    while(k < n && src[k] != '>') k++;
                    if(k >= n) { LOG_ERROR("[module] 语法错误：import <path> 未闭合\n"); free(out.buf); free(t); return NULL; }
                    plen = k - pstart; k++;
                } else {
                    LOG_ERROR("[module] 语法错误：import 后应为 \"path\" 或 <fw>\n");
                    free(out.buf); free(t); return NULL;
                }
                char* path = (char*)malloc((size_t)plen + 1);
                memcpy(path, src + pstart, (size_t)plen); path[plen] = '\0';
                /* 跳过空白 */
                while(k < n && (src[k]==' '||src[k]=='\t'||src[k]=='\r'||src[k]=='\n')) k++;
                /* 三模式解析：; | as IDENT | as IDENT, {list} | {list} */
                char* alias = NULL;
                int is_no_alias = 0;
                char** sel_old = NULL; char** sel_new = NULL; int nsel = 0;
                if(k < n && src[k] == ';') {
                    /* no-alias 模式：import <fw>; */
                    is_no_alias = 1; k++;
                } else if(k < n && src[k] == '{') {
                    /* selective-only 模式：import <fw> {A as B}; */
                    int r = parse_selective_list(src, &k, n, &sel_old, &sel_new);
                    if(r < 0) { LOG_ERROR("[module] 语法错误：selective 列表解析失败\n"); free(path); free(out.buf); free(t); return NULL; }
                    nsel = r;
                    while(k < n && (src[k]==' '||src[k]=='\t'||src[k]=='\r'||src[k]=='\n')) k++;
                    if(k < n && src[k] == ';') k++;
                } else if(k + 1 < n && src[k]=='a' && src[k+1]=='s' &&
                          (k+2 >= n || !is_id_char(src[k+2]))) {
                    /* as 关键字 */
                    k += 2;
                    while(k < n && (src[k]==' '||src[k]=='\t'||src[k]=='\r'||src[k]=='\n')) k++;
                    if(k < n && src[k] == '{') {
                        /* selective-only via as：import <fw> as {A as B}; */
                        int r = parse_selective_list(src, &k, n, &sel_old, &sel_new);
                        if(r < 0) { LOG_ERROR("[module] 语法错误：selective 列表解析失败\n"); free(path); free(out.buf); free(t); return NULL; }
                        nsel = r;
                    } else if(k < n && is_id_start(src[k])) {
                        /* alias 模式（可能带 mixed） */
                        int astart = k;
                        while(k < n && is_id_char(src[k])) k++;
                        int alen = k - astart;
                        alias = (char*)malloc((size_t)alen + 1);
                        memcpy(alias, src + astart, (size_t)alen); alias[alen] = '\0';
                        /* 检查混合模式：alias, {list} */
                        while(k < n && (src[k]==' '||src[k]=='\t'||src[k]=='\r'||src[k]=='\n')) k++;
                        if(k < n && src[k] == ',') {
                            k++;
                            while(k < n && (src[k]==' '||src[k]=='\t'||src[k]=='\r'||src[k]=='\n')) k++;
                            if(k < n && src[k] == '{') {
                                int r = parse_selective_list(src, &k, n, &sel_old, &sel_new);
                                if(r < 0) { LOG_ERROR("[module] 语法错误：selective 列表解析失败\n"); free(path); free(alias); free(out.buf); free(t); return NULL; }
                                nsel = r;
                            }
                        }
                        while(k < n && (src[k]==' '||src[k]=='\t'||src[k]=='\r'||src[k]=='\n')) k++;
                        if(k < n && src[k] == ';') k++;
                    } else {
                        LOG_ERROR("[module] 语法错误：as 后应为别名或 {selective}\n");
                        free(path); free(out.buf); free(t); return NULL;
                    }
                } else {
                    LOG_ERROR("[module] 语法错误：import 后应为 as / { / ;\n");
                    free(path); free(out.buf); free(t); return NULL;
                }
                tr_push_import(t, path, is_framework, alias, is_no_alias, sel_old, sel_new, nsel);
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
                    } else if(w2len == 5 && strncmp(src + k, "class", 5) == 0) {
                        /* export class NAME → 记录 export + symbol + class_name，输出 "class " */
                        int m = k2;
                        while(m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\r' || src[m] == '\n')) m++;
                        int ns = m;
                        while(m < n && is_id_char(src[m])) m++;
                        if(m > ns) {
                            tr_push_export(t, src + ns, m - ns);
                            tr_push_symbol(t, src + ns, m - ns);
                            tr_push_class_name(t, src + ns, m - ns);
                        }
                        sb_puts(&out, "class ");
                        i = ns;
                        continue;
                    } else if(w2len == 9 && strncmp(src + k, "interface", 9) == 0) {
                        /* export interface NAME → 记录 export + symbol + class_name，输出 "interface " */
                        int m = k2;
                        while(m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\r' || src[m] == '\n')) m++;
                        int ns = m;
                        while(m < n && is_id_char(src[m])) m++;
                        if(m > ns) {
                            tr_push_export(t, src + ns, m - ns);
                            tr_push_symbol(t, src + ns, m - ns);
                            tr_push_class_name(t, src + ns, m - ns);
                        }
                        sb_puts(&out, "interface ");
                        i = ns;
                        continue;
                    } else if(w2len == 4 && strncmp(src + k, "type", 4) == 0) {
                        /* export type NAME → 记录 export + symbol，输出 "type " */
                        int m = k2;
                        while(m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\r' || src[m] == '\n')) m++;
                        int ns = m;
                        while(m < n && is_id_char(src[m])) m++;
                        if(m > ns) {
                            tr_push_export(t, src + ns, m - ns);
                            tr_push_symbol(t, src + ns, m - ns);
                        }
                        sb_puts(&out, "type ");
                        i = ns;
                        continue;
                    } else if(w2len == 4 && strncmp(src + k, "enum", 4) == 0) {
                        /* export enum NAME → 记录 export + symbol，输出 "enum " */
                        int m = k2;
                        while(m < n && (src[m] == ' ' || src[m] == '\t' || src[m] == '\r' || src[m] == '\n')) m++;
                        int ns = m;
                        while(m < n && is_id_char(src[m])) m++;
                        if(m > ns) {
                            tr_push_export(t, src + ns, m - ns);
                            tr_push_symbol(t, src + ns, m - ns);
                        }
                        sb_puts(&out, "enum ");
                        i = ns;
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
                /* public 修饰符：隐式导出标记（public class/interface/type/enum） */
                if(wlen == 6 && strncmp(src + i, "public", 6) == 0) {
                    /* 向前看：public 后跟 class/interface/type/enum ?
                     * 也支持 public abstract class/interface/type/enum */
                    int k = j;
                    while(k < n && (src[k] == ' ' || src[k] == '\t' || src[k] == '\r' || src[k] == '\n')) k++;
                    if(k < n && is_id_start(src[k])) {
                        int k2 = k;
                        while(k2 < n && is_id_char(src[k2])) k2++;
                        int w2len = k2 - k;
                        /* 检查是否为 abstract，若是则继续向后看 */
                        if(w2len == 8 && strncmp(src + k, "abstract", 8) == 0) {
                            int k3 = k2;
                            while(k3 < n && (src[k3] == ' ' || src[k3] == '\t' || src[k3] == '\r' || src[k3] == '\n')) k3++;
                            if(k3 < n && is_id_start(src[k3])) {
                                int k4 = k3;
                                while(k4 < n && is_id_char(src[k4])) k4++;
                                int w3len = k4 - k3;
                                if((w3len == 5 && strncmp(src + k3, "class", 5) == 0) ||
                                   (w3len == 9 && strncmp(src + k3, "interface", 9) == 0) ||
                                   (w3len == 4 && strncmp(src + k3, "type", 4) == 0) ||
                                   (w3len == 4 && strncmp(src + k3, "enum", 4) == 0)) {
                                    implicit_export = 1;
                                }
                            }
                        }
                        if((w2len == 5 && strncmp(src + k, "class", 5) == 0) ||
                           (w2len == 9 && strncmp(src + k, "interface", 9) == 0) ||
                           (w2len == 4 && strncmp(src + k, "type", 4) == 0) ||
                           (w2len == 4 && strncmp(src + k, "enum", 4) == 0)) {
                            implicit_export = 1;
                        }
                    }
                    /* 输出 "public"（parser 已支持 public class 语法） */
                    sb_putn(&out, src + i, wlen);
                    i = j;
                    continue;
                }
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
                if(wlen == 5 && strncmp(src + i, "class", 5) == 0) {
                    expect_kind = 4;   /* class 名 */
                    sb_putn(&out, src + i, wlen);
                    i = j;
                    continue;
                }
                if(wlen == 9 && strncmp(src + i, "interface", 9) == 0) {
                    expect_kind = 5;   /* interface 名 */
                    sb_putn(&out, src + i, wlen);
                    i = j;
                    continue;
                }
            }

            /* expect_kind：当前标识符即声明名 */
            if(expect_kind && bound_ok) {
                tr_push_symbol(t, src + i, wlen);
                /* class/interface 名记录到 class_names（用于从 export map 排除） */
                int ek = expect_kind & 0x3F;   /* 低 6 位为实际 kind */
                if(ek == 4 || ek == 5) {
                    tr_push_class_name(t, src + i, wlen);
                }
                /* public 隐式导出：同时记录为 export */
                if(implicit_export) {
                    tr_push_export(t, src + i, wlen);
                    implicit_export = 0;
                }
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
    for(int q = 0; q < tr->nimports; q++) {
        free(tr->imports[q].path);
        free(tr->imports[q].alias);
        for(int s = 0; s < tr->imports[q].nselective; s++) {
            free(tr->imports[q].sel_old[s]);
            free(tr->imports[q].sel_new[s]);
        }
        free(tr->imports[q].sel_old);
        free(tr->imports[q].sel_new);
    }
    free(tr->imports);
    for(int q = 0; q < tr->nexports; q++) free(tr->exports[q]);
    free(tr->exports);
    for(int q = 0; q < tr->nsymbols; q++) free(tr->all_symbols[q]);
    free(tr->all_symbols);
    free(tr->sym_hash);
    for(int q = 0; q < tr->nclass_names; q++) free(tr->class_names[q]);
    free(tr->class_names);
    free(tr);
}

/* 构建实际需要 mangle 的符号集（从 all_symbols 中排除不需要 mangle 的）。
 * skip_names: 要排除的符号名（no-alias 模式下为 exports 列表）。
 * 返回 malloc'd 指针数组（不拷贝字符串，仅指向 tr->all_symbols 中的项）。
 * *out_n 为结果数组长度。调用方只 free 数组本身，不 free 各项。 */
static char** build_mangle_set(TransformResult* tr,
                                const char* const* skip_names, int nskip,
                                int* out_n) {
    *out_n = 0;
    char** result = (char**)malloc((size_t)(tr->nsymbols + 1) * sizeof(char*));
    for(int i = 0; i < tr->nsymbols; i++) {
        int skip = 0;
        for(int j = 0; j < nskip; j++) {
            if(strcmp(tr->all_symbols[i], skip_names[j]) == 0) { skip = 1; break; }
        }
        if(!skip) result[(*out_n)++] = tr->all_symbols[i];
    }
    return result;
}

/* 判断 name 是否在列表中（线性扫描，列表通常很短） */
static int name_in_list(const char* name, const char* const* list, int n) {
    for(int i = 0; i < n; i++) {
        if(strcmp(name, list[i]) == 0) return 1;
    }
    return 0;
}

/* 生成 selective shim 行：新名 = __lm_mod_<mod_id>_原名;
 * 仅当原名在 mangle 集中时才需要 shim（否则原名已全局可见）。 */
static void emit_shim(SB* out, const char* new_name, int mod_id, const char* old_name) {
    sb_puts(out, new_name);
    sb_puts(out, " = __lm_mod_");
    char idstr[16];
    snprintf(idstr, sizeof idstr, "%d_", mod_id);
    sb_puts(out, idstr);
    sb_puts(out, old_name);
    sb_puts(out, ";\n");
}

/* 递归展开：把 abs_path（已 realpath）模块的完整内联文本（含末尾导出 map + shim）
 * 写入 out，返回导出 map 变量名（malloc'd）。失败返回 NULL。
 *
 * active 栈已含本模块自身（由调用方压入）；n_active 为栈长。
 * is_no_alias: no-alias 模式（export 符号不 mangle）
 * sel_old/sel_new/nsel: selective 列表（原名/新名/数量；新名 NULL=沿用原名）
 */
static char* expand_file(const char* abs_path, SB* out,
                         const char** active, int n_active,
                         int is_no_alias,
                         const char* const* sel_old,
                         const char* const* sel_new,
                         int nsel) {
    /* 1) 去重：已处理过则直接返回缓存的 expvar，不重复内联模块体。 */
    ProcessedMod* hit = find_processed(abs_path);
    if(hit) {
        if(!hit->expvar) return NULL;
        /* 去重命中：为新的 selective 项发 shim（若符号已被 mangle 且非类名） */
        for(int s = 0; s < nsel; s++) {
            if(pm_has_selective(hit, sel_old[s])) continue;
            const char* new_name = sel_new[s] ? sel_new[s] : sel_old[s];
            if(pm_was_mangled(hit, sel_old[s]) && !pm_is_class_name(hit, sel_old[s])) {
                emit_shim(out, new_name, hit->module_id, sel_old[s]);
            }
            pm_add_selective(hit, sel_old[s], new_name);
        }
        return strdup(hit->expvar);
    }

    /* 2) 登记模块，分配 module_id。 */
    ProcessedMod* pm = register_processed(abs_path);
    int mod_id = pm->module_id;

    /* 3) 读模块源码 + 条件编译过滤 + transform */
    char* content = slurp_file(abs_path);
    if(!content) {
        LOG_ERROR("[module] 无法打开模块文件: %s\n", abs_path);
        return NULL;
    }
    {
        int cerr = 0;
        char* filtered = lm_cond_filter_text(content, abs_path, &cerr);
        if(cerr) { free(content); return NULL; }
        if(filtered) { free(content); content = filtered; }
    }
    TransformResult* tr = transform(content);
    free(content);
    if(!tr) return NULL;

    /* 4) build_mangle_set + Name Mangling */
    int mangle_n = 0;
    char** mangle_set;
    if(is_no_alias) {
        /* no-alias：export 符号不 mangle（保持原名全局） */
        mangle_set = build_mangle_set(tr, (const char* const*)tr->exports, tr->nexports, &mangle_n);
    } else {
        /* alias/selective/mixed：全部 mangle */
        mangle_set = build_mangle_set(tr, NULL, 0, &mangle_n);
    }
    /* 记录 mangle 集到 ProcessedMod（供后续 dedup selective shim 使用） */
    pm->mangled = (char**)malloc((size_t)(mangle_n + 1) * sizeof(char*));
    for(int m = 0; m < mangle_n; m++) pm->mangled[m] = strdup(mangle_set[m]);
    pm->nmangled = mangle_n;
    /* 记录 class_names 到 ProcessedMod（shim 生成时跳过类名） */
    pm->class_names = (char**)malloc((size_t)(tr->nclass_names + 1) * sizeof(char*));
    for(int m = 0; m < tr->nclass_names; m++) pm->class_names[m] = strdup(tr->class_names[m]);
    pm->nclass_names = tr->nclass_names;

    char* mangled = mangle_text(tr->text, mangle_set, mangle_n, mod_id);
    free(tr->text);
    tr->text = mangled;
    free(mangle_set);   /* 只 free 数组，不 free 各项（它们指向 tr->all_symbols） */

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
            int k = i + 8;
            int idx = 0;
            while(k < tl && txt[k] >= '0' && txt[k] <= '9') { idx = idx * 10 + (txt[k] - '0'); k++; }
            while(k < tl && txt[k] != ';') k++;
            if(k < tl) k++;

            if(idx < 0 || idx >= tr->nimports) {
                LOG_ERROR("[module] 内部错误：占位符越界\n"); ok = 0; break;
            }
            ImportSpec* isp = &tr->imports[idx];

            /* 解析模块路径 */
            char joined[PATH_MAX];
            if(isp->is_framework) {
                /* 框架导入：<fw/mod> → resolve_framework */
                if(!resolve_framework(isp->path, joined, sizeof joined)) {
                    LOG_ERROR("[module] 无法解析框架路径: <%s> (in %s)\n", isp->path, abs_path);
                    ok = 0; break;
                }
            } else if(isp->path[0] == '/') {
                snprintf(joined, sizeof joined, "%s", isp->path);
            } else {
                snprintf(joined, sizeof joined, "%s/%s", dir, isp->path);
            }

            char real[PATH_MAX];
            if(!realpath(joined, real)) {
                LOG_ERROR("[module] 无法解析导入路径: %s (in %s)\n", isp->path, abs_path);
                ok = 0; break;
            }
            /* 循环导入检测 */
            int cyc = 0;
            for(int a = 0; a < n_active; a++) {
                if(strcmp(active[a], real) == 0) { cyc = 1; break; }
            }
            if(cyc) {
                LOG_ERROR("[module] 检测到循环导入: %s\n", real);
                ok = 0; break;
            }

            /* 递归展开子模块（传递子模块自身的 mode 参数） */
            char* child_expvar = NULL;
            {
                const char* new_active[64];
                if(n_active + 1 > 63) { LOG_ERROR("[module] 导入嵌套过深\n"); ok = 0; break; }
                for(int a = 0; a < n_active; a++) new_active[a] = active[a];
                new_active[n_active] = real;

                SB child_body; sb_init(&child_body);
                child_expvar = expand_file(real, &child_body, new_active, n_active + 1,
                                          isp->is_no_alias,
                                          (const char* const*)isp->sel_old,
                                          (const char* const*)isp->sel_new,
                                          isp->nselective);
                if(!child_expvar) { ok = 0; free(child_body.buf); break; }

                /* 子模块体（含 export map + shim；命中去重时仅 shim） */
                sb_puts(&body, child_body.buf);
                free(child_body.buf);
                /* alias 赋值（仅 alias 模式 / 混合模式有 alias） */
                if(isp->alias) {
                    ProcessedMod* cpm = find_processed(real);
                    lm_register_alias(isp->alias,
                                      cpm ? (char* const*)cpm->export_names : NULL,
                                      cpm ? cpm->nexport_names : 0);
                    sb_putc(&body, '\n');
                    sb_puts(&body, isp->alias);
                    sb_puts(&body, " = ");
                    sb_puts(&body, child_expvar);
                    sb_puts(&body, ";\n");
                }
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

    /* 6) 输出模块体 */
    sb_puts(out, body.buf);
    free(body.buf);

    /* 7) 生成 selective shim（首次处理：为每个 selective 项发 shim） */
    for(int s = 0; s < nsel; s++) {
        const char* old_name = sel_old[s];
        const char* new_name = sel_new[s] ? sel_new[s] : old_name;
        /* 仅当原名在 mangle 集中且非类名时才需要 shim
         * （类名通过 rewrite_class_refs 的 extends/implements 文本重写处理） */
        if(pm_was_mangled(pm, old_name) && !pm_is_class_name(pm, old_name)) {
            emit_shim(out, new_name, mod_id, old_name);
        }
        pm_add_selective(pm, old_name, new_name);
    }

    /* 8) 生成本模块的导出 map 变量 */
    char expvar[64];
    snprintf(expvar, sizeof expvar, "__lmod_exp_%d", g_mod_seq++);

    sb_putc(out, '\n');
    sb_puts(out, expvar);
    sb_puts(out, " = {");
    int first = 1;
    /* 同步记录实际写入 map 的键名（供别名成员编译期校验，条件与下方发射一致） */
    pm->export_names = (char**)malloc((size_t)(tr->nexports + 1) * sizeof(char*));
    pm->nexport_names = 0;
    for(int q = 0; q < tr->nexports; q++) {
        /* selective 符号跳过（已有 shim） */
        if(name_in_list(tr->exports[q], sel_old, nsel)) continue;
        /* class/interface 名跳过（类名非一等值，不能放入 export map） */
        if(tr_is_class_name(tr, tr->exports[q])) continue;
        pm->export_names[pm->nexport_names++] = strdup(tr->exports[q]);
        if(!first) sb_puts(out, ", ");
        first = 0;
        sb_putc(out, '"');
        sb_puts(out, tr->exports[q]);
        sb_puts(out, "\": ");
        if(is_no_alias && !pm_was_mangled(pm, tr->exports[q])) {
            /* no-alias 模式且 export 未被 mangle：使用原名 */
            sb_puts(out, tr->exports[q]);
        } else {
            /* 已 mangle：使用 mangled 名 */
            sb_puts(out, "__lm_mod_");
            char idstr[16];
            snprintf(idstr, sizeof idstr, "%d_", mod_id);
            sb_puts(out, idstr);
            sb_puts(out, tr->exports[q]);
        }
    }
    sb_puts(out, "};\n");

    /* 9) 回填已处理表的 expvar */
    pm->expvar = strdup(expvar);
    char* ret = strdup(expvar);
    tr_free(tr);
    return ret;
}

/* ---------------- extends/implements 文本重写 ---------------- */
/* alias → module_id 映射 */
typedef struct { char* alias; int module_id; char** class_names; int nclass_names; } AliasEntry;
/* new_name → (module_id, old_name) 映射 */
typedef struct { char* new_name; int module_id; char* old_name; } RenameEntry;

/* 扫描 text，把 extends/implements 后的标识符按映射重写为 mangled 名。
 * alias_map/nalias: alias → module_id（extends alias.Class → extends __lm_mod_<id>_Class）
 * rename_map/nrename: new_name → (module_id, old_name)（extends NewName → extends __lm_mod_<id>_OrigName）
 * 返回 malloc'd 新文本。 */
static char* rewrite_class_refs(const char* text,
                                AliasEntry* alias_map, int nalias,
                                RenameEntry* rename_map, int nrename) {
    SB out; sb_init(&out);
    int n = (int)strlen(text);
    int i = 0;
    while(i < n) {
        /* 检查 extends / implements 关键字（词边界） */
        int matched = 0;
        int kw_len = 0;
        if((i == 0 || (!is_id_char(text[i-1]) && text[i-1] != '.')) &&
           i + 7 <= n && strncmp(text + i, "extends", 7) == 0 &&
           (i + 7 == n || !is_id_char(text[i+7]))) {
            matched = 1; kw_len = 7;
        } else if((i == 0 || (!is_id_char(text[i-1]) && text[i-1] != '.')) &&
                  i + 10 <= n && strncmp(text + i, "implements", 10) == 0 &&
                  (i + 10 == n || !is_id_char(text[i+10]))) {
            matched = 1; kw_len = 10;
        }

        if(matched) {
            /* 输出关键字 */
            sb_putn(&out, text + i, kw_len);
            int k = i + kw_len;
            /* 跳过空白 */
            while(k < n && (text[k]==' '||text[k]=='\t'||text[k]=='\r'||text[k]=='\n')) {
                sb_putc(&out, text[k]); k++;
            }
            /* 读取标识符（可能含 alias. 前缀） */
            if(k < n && is_id_start(text[k])) {
                int ns = k;
                while(k < n && is_id_char(text[k])) k++;
                int nl = k - ns;
                /* 检查是否有 alias. 前缀 */
                int has_dot = (k < n && text[k] == '.');
                if(has_dot) {
                    /* alias.Class 模式：查 alias_map */
                    char alias_name[256];
                    if(nl < 256) {
                        memcpy(alias_name, text + ns, (size_t)nl);
                        alias_name[nl] = '\0';
                        int found = -1;
                        for(int a = 0; a < nalias; a++) {
                            if(strcmp(alias_map[a].alias, alias_name) == 0) { found = alias_map[a].module_id; break; }
                        }
                        if(found >= 0) {
                            k++; /* 跳过 '.' */
                            int cs = k;
                            while(k < n && is_id_char(text[k])) k++;
                            int cl = k - cs;
                            sb_puts(&out, "__lm_mod_");
                            char idstr[16];
                            snprintf(idstr, sizeof idstr, "%d_", found);
                            sb_puts(&out, idstr);
                            sb_putn(&out, text + cs, cl);
                            i = k;
                            continue;
                        }
                    }
                    /* 未命中：原样输出 alias.Class */
                    sb_putn(&out, text + ns, nl);
                    /* 继续输出 .Class 部分 */
                } else {
                    /* 裸标识符模式：查 rename_map */
                    char name[256];
                    if(nl < 256) {
                        memcpy(name, text + ns, (size_t)nl);
                        name[nl] = '\0';
                        int found = -1;
                        const char* old_name = NULL;
                        for(int r = 0; r < nrename; r++) {
                            if(strcmp(rename_map[r].new_name, name) == 0) {
                                found = rename_map[r].module_id;
                                old_name = rename_map[r].old_name;
                                break;
                            }
                        }
                        if(found >= 0 && old_name) {
                            sb_puts(&out, "__lm_mod_");
                            char idstr[16];
                            snprintf(idstr, sizeof idstr, "%d_", found);
                            sb_puts(&out, idstr);
                            sb_puts(&out, old_name);
                            i = k;
                            continue;
                        }
                    }
                    /* 未命中：原样输出 */
                    sb_putn(&out, text + ns, nl);
                    i = k;
                    continue;
                }
            }
            i = k;
            continue;
        }

        /* 检查 alias.ClassName( 构造调用（类名非一等值，需文本重写） */
        if((i == 0 || (!is_id_char(text[i-1]) && text[i-1] != '.')) &&
           is_id_start(text[i])) {
            int j = i;
            while(j < n && is_id_char(text[j])) j++;
            int wl = j - i;
            for(int a = 0; a < nalias; a++) {
                int al = (int)strlen(alias_map[a].alias);
                if(al == wl && strncmp(text + i, alias_map[a].alias, (size_t)wl) == 0) {
                    /* 匹配 alias 名，检查 .ClassName */
                    int k = j;
                    if(k < n && text[k] == '.') {
                        k++;
                        if(k < n && is_id_start(text[k])) {
                            int cs = k;
                            while(k < n && is_id_char(text[k])) k++;
                            int cl = k - cs;
                            char cname[256];
                            if(cl < 256) {
                                memcpy(cname, text + cs, (size_t)cl);
                                cname[cl] = '\0';
                                for(int c = 0; c < alias_map[a].nclass_names; c++) {
                                    if(strcmp(alias_map[a].class_names[c], cname) == 0) {
                                        /* 重写：alias.ClassName → __lm_mod_<id>_ClassName */
                                        sb_puts(&out, "__lm_mod_");
                                        char idstr[16];
                                        snprintf(idstr, sizeof idstr, "%d_", alias_map[a].module_id);
                                        sb_puts(&out, idstr);
                                        sb_putn(&out, text + cs, cl);
                                        i = k;
                                        goto next_char;
                                    }
                                }
                            }
                        }
                    }
                    break;  /* alias 匹配但不是 class name，原样输出 */
                }
            }
        }

        /* 跳过字符串/注释（避免误匹配） */
        if(text[i] == '/' && i + 1 < n && text[i+1] == '/') {
            int j = i;
            while(j < n && text[j] != '\n') j++;
            sb_putn(&out, text + i, j - i);
            i = j;
            continue;
        }
        if(text[i] == '/' && i + 1 < n && text[i+1] == '*') {
            int j = i + 2;
            while(j + 1 < n && !(text[j] == '*' && text[j+1] == '/')) j++;
            j = (j + 2 <= n) ? j + 2 : n;
            sb_putn(&out, text + i, j - i);
            i = j;
            continue;
        }
        if(text[i] == '"') {
            int j = i + 1;
            while(j < n && text[j] != '"') {
                if(text[j] == '\\' && j + 1 < n) j += 2; else j++;
            }
            j = (j < n) ? j + 1 : n;
            sb_putn(&out, text + i, j - i);
            i = j;
            continue;
        }

        sb_putc(&out, text[i]);
        i++;
        next_char: ;
    }
    return out.buf;
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
    /* 条件编译过滤（主文件；死分支中的 import 在 transform 前已被移除） */
    {
        int cerr = 0;
        char* filtered = lm_cond_filter_text(content, real, &cerr);
        if(cerr) { free(content); *had_mod_out = -1; return NULL; }
        if(filtered) { free(content); content = filtered; }
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

    /* 收集 alias → module_id 和 rename → (module_id, old_name) 映射 */
    AliasEntry* alias_map = NULL;  int nalias = 0, cap_alias = 0;
    RenameEntry* rename_map = NULL; int nrename = 0, cap_rename = 0;

    while(i < tl) {
        if(strncmp(txt + i, "__LMIMP_", 8) == 0) {
            int k = i + 8;
            int idx = 0;
            while(k < tl && txt[k] >= '0' && txt[k] <= '9') { idx = idx * 10 + (txt[k] - '0'); k++; }
            while(k < tl && txt[k] != ';') k++;
            if(k < tl) k++;

            if(idx < 0 || idx >= tr->nimports) { ok = 0; break; }
            ImportSpec* isp = &tr->imports[idx];

            /* 解析模块路径 */
            char joined[PATH_MAX];
            if(isp->is_framework) {
                if(!resolve_framework(isp->path, joined, sizeof joined)) {
                    LOG_ERROR("[module] 无法解析框架路径: <%s> (in %s)\n", isp->path, real);
                    ok = 0; break;
                }
            } else if(isp->path[0] == '/') {
                snprintf(joined, sizeof joined, "%s", isp->path);
            } else {
                snprintf(joined, sizeof joined, "%s/%s", dir, isp->path);
            }
            char mreal[PATH_MAX];
            if(!realpath(joined, mreal)) {
                LOG_ERROR("[module] 无法解析导入路径: %s (in %s)\n", isp->path, real);
                ok = 0; break;
            }
            /* 循环检测 */
            const char* active[2];
            active[0] = real;
            active[1] = mreal;
            SB child_body; sb_init(&child_body);
            char* expvar = expand_file(mreal, &child_body, active, 2,
                                       isp->is_no_alias,
                                       (const char* const*)isp->sel_old,
                                       (const char* const*)isp->sel_new,
                                       isp->nselective);
            if(!expvar) { ok = 0; free(child_body.buf); break; }

            sb_puts(&out, child_body.buf);
            free(child_body.buf);

            /* alias 赋值（仅 alias 模式 / 混合模式） */
            if(isp->alias) {
                ProcessedMod* apm = find_processed(mreal);
                lm_register_alias(isp->alias,
                                  apm ? (char* const*)apm->export_names : NULL,
                                  apm ? apm->nexport_names : 0);
                sb_putc(&out, '\n');
                sb_puts(&out, isp->alias);
                sb_puts(&out, " = ");
                sb_puts(&out, expvar);
                sb_puts(&out, ";\n");

                /* 记录 alias → module_id + class_names */
                ProcessedMod* pm = find_processed(mreal);
                if(pm) {
                    if(nalias >= cap_alias) {
                        cap_alias = cap_alias ? cap_alias * 2 : 8;
                        alias_map = (AliasEntry*)realloc(alias_map, (size_t)cap_alias * sizeof(AliasEntry));
                    }
                    alias_map[nalias].alias = strdup(isp->alias);
                    alias_map[nalias].module_id = pm->module_id;
                    /* 复制 class_names（用于 alias.ClassName( 构造调用重写） */
                    alias_map[nalias].nclass_names = pm->nclass_names;
                    if(pm->nclass_names > 0) {
                        alias_map[nalias].class_names = (char**)malloc((size_t)pm->nclass_names * sizeof(char*));
                        for(int c = 0; c < pm->nclass_names; c++)
                            alias_map[nalias].class_names[c] = strdup(pm->class_names[c]);
                    } else {
                        alias_map[nalias].class_names = NULL;
                    }
                    nalias++;
                }
            }

            /* 记录 rename → (module_id, old_name) */
            ProcessedMod* pm = find_processed(mreal);
            if(pm) {
                for(int s = 0; s < isp->nselective; s++) {
                    const char* new_name = isp->sel_new[s] ? isp->sel_new[s] : isp->sel_old[s];
                    if(nrename >= cap_rename) {
                        cap_rename = cap_rename ? cap_rename * 2 : 8;
                        rename_map = (RenameEntry*)realloc(rename_map, (size_t)cap_rename * sizeof(RenameEntry));
                    }
                    rename_map[nrename].new_name = strdup(new_name);
                    rename_map[nrename].module_id = pm->module_id;
                    rename_map[nrename].old_name = strdup(isp->sel_old[s]);
                    nrename++;
                }
                /* no-alias 模式：被 mangle 的非导出 class/interface 名需重写
                 * （否则主文件 extends/implements 引用原名，但模块声明的是 mangled 名）
                 * 只对 class_names 中且确实被 mangle 的名字建映射 */
                if(isp->is_no_alias) {
                    for(int c = 0; c < pm->nclass_names; c++) {
                        const char* cn = pm->class_names[c];
                        if(pm_was_mangled(pm, cn)) {
                            if(nrename >= cap_rename) {
                                cap_rename = cap_rename ? cap_rename * 2 : 8;
                                rename_map = (RenameEntry*)realloc(rename_map, (size_t)cap_rename * sizeof(RenameEntry));
                            }
                            rename_map[nrename].new_name = strdup(cn);
                            rename_map[nrename].module_id = pm->module_id;
                            rename_map[nrename].old_name = strdup(cn);
                            nrename++;
                        }
                    }
                }
            }

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
        for(int a = 0; a < nalias; a++) { free(alias_map[a].alias); for(int c=0;c<alias_map[a].nclass_names;c++) free(alias_map[a].class_names[c]); free(alias_map[a].class_names); }
        free(alias_map);
        for(int r = 0; r < nrename; r++) { free(rename_map[r].new_name); free(rename_map[r].old_name); }
        free(rename_map);
        *had_mod_out = -1;
        return NULL;
    }

    /* 所有 import 展开后：重写主文件的 extends/implements 引用 */
    if(nalias > 0 || nrename > 0) {
        char* rewritten = rewrite_class_refs(out.buf, alias_map, nalias, rename_map, nrename);
        free(out.buf);
        out.buf = rewritten;
    }

    /* 释放映射表 */
    for(int a = 0; a < nalias; a++) { free(alias_map[a].alias); for(int c=0;c<alias_map[a].nclass_names;c++) free(alias_map[a].class_names[c]); free(alias_map[a].class_names); }
    free(alias_map);
    for(int r = 0; r < nrename; r++) { free(rename_map[r].new_name); free(rename_map[r].old_name); }
    free(rename_map);

    if(!has_any) {
        free(out.buf);
        *had_mod_out = 0;
        return NULL;
    }
    *had_mod_out = 1;
    return out.buf;
}
