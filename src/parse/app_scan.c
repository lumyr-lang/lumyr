/*
 * lumyr 应用配置目录扫描（编译期）实现
 * 详见 app_scan.h。
 */
#include "app_scan.h"
#include "lumyr_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#define PATH_SEP '\\'
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define PATH_SEP '/'
#endif

/* import.c 提供：展开按绝对路径指定的模块，返回 module_id（-1 失败），body 写出 */
extern int lm_expand_scanned_module(const char* abs_path, const char* main_real,
                                    char** out_body);

/* 进程内保留扫描结果 */
static AppScanResult* g_last_scan = NULL;

AppScanResult* app_scan_last(void) { return g_last_scan; }

/* ---------------- 小工具 ---------------- */

/* 取路径所在目录（不含尾部分隔符）；无目录时写 "." */
static void scan_dir_of(const char* path, char* out, size_t outsz) {
    const char* sep = NULL;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') sep = p;
    }
    if (!sep) { snprintf(out, outsz, "."); return; }
    size_t n = (size_t)(sep - path);
    if (n == 0) n = 1;  /* 根目录 */
    if (n >= outsz) n = outsz - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

/* 拼接目录与文件名 */
static void join_path(char* out, size_t outsz, const char* dir, const char* name) {
    size_t dl = strlen(dir);
    if (dl > 0 && (dir[dl - 1] == '/' || dir[dl - 1] == '\\')) {
        snprintf(out, outsz, "%s%s", dir, name);
    } else {
        snprintf(out, outsz, "%s%c%s", dir, PATH_SEP, name);
    }
}

static int file_exists(const char* path) {
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

/* 字符串字面量反转义（仅处理路径可能出现的 \" \\ \/），返回 malloc'd */
static char* unescape_json_str(const char* s, int len) {
    char* out = (char*)malloc((size_t)len + 1);
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len) {
            char c = s[i + 1];
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            out[j++] = c;
            i++;
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* ---------------- 主文件中定位配置 ---------------- */

/* 已知的应用构造/配置入口标识符 */
static const char* g_app_ctors[] = {
    "Application", "WebApplication", "ServiceApplication", "loadConfig", NULL
};

static int is_app_ctor(const char* s, int len) {
    for (int i = 0; g_app_ctors[i]; i++) {
        if ((int)strlen(g_app_ctors[i]) == len &&
            strncmp(s, g_app_ctors[i], (size_t)len) == 0) return 1;
    }
    return 0;
}

/*
 * 扫描主文本：找「应用构造标识符( 后紧跟的 *.json 字符串字面量」。
 * 跳过注释与字符串内部；返回 malloc'd 字面量内容，未找到 NULL。
 */
static char* find_config_literal(const char* text) {
    int n = (int)strlen(text);
    int i = 0;
    while (i < n) {
        char c = text[i];
        /* 行注释 */
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            while (i < n && text[i] != '\n') i++;
            continue;
        }
        /* 块注释 */
        if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(text[i] == '*' && text[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        /* 标识符：检查是否应用构造入口 */
        if (isalpha((unsigned char)c) || c == '_') {
            int s = i;
            while (i < n && (isalnum((unsigned char)text[i]) || text[i] == '_')) i++;
            int wl = i - s;
            if (is_app_ctor(text + s, wl)) {
                int k = i;
                while (k < n && isspace((unsigned char)text[k])) k++;
                if (k < n && text[k] == '(') {
                    k++;
                    while (k < n && isspace((unsigned char)text[k])) k++;
                    if (k < n && text[k] == '"') {
                        int st = k + 1;
                        int e = st;
                        while (e < n && text[e] != '"') {
                            if (text[e] == '\\' && e + 1 < n) e += 2; else e++;
                        }
                        if (e < n) {
                            int slen = e - st;
                            /* 注意用 strncmp：字面量之后是引号而非 NUL，strcmp 永不相等 */
                            if (slen > 5 && strncmp(text + st + slen - 5, ".json", 5) == 0) {
                                return unescape_json_str(text + st, slen);
                            }
                        }
                    }
                }
            }
            continue;
        }
        i++;
    }
    return NULL;
}

/* ---------------- 配置 JSON 提取 ---------------- */

/* 定位 JSON 对象中 key 的冒号后位置；未找到 NULL */
static const char* json_find_key(const char* json, const char* key) {
    int n = (int)strlen(json);
    int i = 0;
    while (i < n) {
        if (json[i] == '"') {
            int s = i + 1;
            int e = s;
            while (e < n && json[e] != '"') {
                if (json[e] == '\\' && e + 1 < n) e += 2; else e++;
            }
            int klen = e - s;
            if (klen == (int)strlen(key) && strncmp(json + s, key, (size_t)klen) == 0) {
                int k = e + 1;
                while (k < n && isspace((unsigned char)json[k])) k++;
                if (k < n && json[k] == ':') {
                    k++;
                    while (k < n && isspace((unsigned char)json[k])) k++;
                    return json + k;
                }
            }
            i = e + 1;
            continue;
        }
        i++;
    }
    return NULL;
}

/* 取字符串数组；返回项数（0=无/空），数组写出（各项 malloc'd） */
static int json_string_array(const char* json, const char* key, char*** out) {
    *out = NULL;
    const char* p = json_find_key(json, key);
    if (!p || *p != '[') return 0;
    p++;
    int n = 0, cap = 0;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (*p == ']') break;
        if (*p != '"') break;
        p++;
        const char* s = p;
        while (*p && p[0] != '"') {
            if (p[0] == '\\' && p[1]) p += 2; else p++;
        }
        if (!*p) break;
        int slen = (int)(p - s);
        if (n >= cap) {
            cap = cap ? cap * 2 : 4;
            *out = (char**)realloc(*out, (size_t)cap * sizeof(char*));
        }
        (*out)[n++] = unescape_json_str(s, slen);
        p++;
    }
    return n;
}

/* 取布尔值；键缺失返回 def */
static int json_bool(const char* json, const char* key, int def) {
    const char* p = json_find_key(json, key);
    if (!p) return def;
    if (strncmp(p, "true", 4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return def;
}

/* ---------------- 目录枚举（*.lm，非递归） ---------------- */

static char** list_lm_files(const char* dir, int* nfiles) {
    *nfiles = 0;
    char** names = NULL;
    int cap = 0;
#ifdef _WIN32
    char pattern[PATH_MAX];
    snprintf(pattern, sizeof pattern, "%s\\*.lm", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        int nl = (int)strlen(fd.cFileName);
        if (nl <= 3 || strcmp(fd.cFileName + nl - 3, ".lm") != 0) continue;
        if (*nfiles >= cap) {
            cap = cap ? cap * 2 : 8;
            names = (char**)realloc(names, (size_t)cap * sizeof(char*));
        }
        names[(*nfiles)++] = strdup(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return NULL;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        int nl = (int)strlen(ent->d_name);
        if (nl <= 3 || strcmp(ent->d_name + nl - 3, ".lm") != 0) continue;
        if (*nfiles >= cap) {
            cap = cap ? cap * 2 : 8;
            names = (char**)realloc(names, (size_t)cap * sizeof(char*));
        }
        names[(*nfiles)++] = strdup(ent->d_name);
    }
    closedir(d);
#endif
    return names;
}

/* ---------------- 动态文本拼接 ---------------- */
typedef struct { char* buf; int len; } TextAccum;

static void accum_puts(TextAccum* a, const char* s) {
    int add = (int)strlen(s);
    a->buf = (char*)realloc(a->buf, (size_t)a->len + (size_t)add + 1);
    memcpy(a->buf + a->len, s, (size_t)add);
    a->len += add;
    a->buf[a->len] = '\0';
}

/* ---------------- 扫描主入口 ---------------- */

AppScanResult* app_scan_run(const char* main_text, const char* main_real,
                            char** scanned_bodies) {
    *scanned_bodies = NULL;
    if (g_last_scan) { app_scan_free(g_last_scan); g_last_scan = NULL; }

    AppScanResult* r = (AppScanResult*)calloc(1, sizeof(AppScanResult));
    r->auto_scan = 1;

    /* 1) 定位配置字面量；无则零影响返回 */
    char* lit = find_config_literal(main_text);
    if (!lit) { g_last_scan = r; return r; }

    /* 2) 解析配置路径：相对路径先按 CWD（与运行时 file() 语义一致），再按主文件目录 */
    char cfg_real[PATH_MAX];
    if (lit[0] == '/' || lit[0] == '\\' ||
        (lit[1] == ':' && lit[2] == '\\')) {
        snprintf(cfg_real, sizeof cfg_real, "%s", lit);
    } else {
        if (file_exists(lit)) {
            if (!realpath(lit, cfg_real)) snprintf(cfg_real, sizeof cfg_real, "%s", lit);
        } else {
            char main_dir[PATH_MAX];
            scan_dir_of(main_real, main_dir, sizeof main_dir);
            char cand[PATH_MAX];
            join_path(cand, sizeof cand, main_dir, lit);
            if (file_exists(cand)) {
                if (!realpath(cand, cfg_real)) snprintf(cfg_real, sizeof cfg_real, "%s", cand);
            } else {
                LOG_ERROR("[scan] 配置文件不存在 / config file not found: %s\n", lit);
                free(lit);
                free(r);
                return NULL;
            }
        }
    }
    free(lit);
    r->config_path = strdup(cfg_real);
    char cfg_dir[PATH_MAX];
    scan_dir_of(cfg_real, cfg_dir, sizeof cfg_dir);
    r->config_dir = strdup(cfg_dir);

    /* 3) 读配置文本 */
    FILE* f = fopen(cfg_real, "rb");
    if (!f) {
        LOG_ERROR("[scan] 无法打开配置 / cannot open config: %s\n", cfg_real);
        app_scan_free(r);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char* json = (char*)malloc((size_t)sz + 1);
    size_t rd = fread(json, 1, (size_t)sz, f);
    fclose(f);
    json[rd] = '\0';

    r->auto_scan = json_bool(json, "autoScan", 1);

    char** dirs = NULL;
    int ndirs = json_string_array(json, "scanDirs", &dirs);
    free(json);

    if (ndirs <= 0) {
        for (int i = 0; i < ndirs; i++) free(dirs[i]);
        free(dirs);
        g_last_scan = r;
        return r;
    }

    /* 4) 枚举各目录、跨目录文件名去重、展开模块 */
    char** seen_names = NULL;
    int nseen = 0;
    TextAccum bodies;
    bodies.buf = NULL;
    bodies.len = 0;

    for (int d = 0; d < ndirs; d++) {
        char dir_real[PATH_MAX];
        if (dirs[d][0] == '/' || dirs[d][0] == '\\' ||
            (dirs[d][1] == ':' && dirs[d][2] == '\\')) {
            snprintf(dir_real, sizeof dir_real, "%s", dirs[d]);
        } else {
            join_path(dir_real, sizeof dir_real, cfg_dir, dirs[d]);
        }

        int nfiles = 0;
        char** files = list_lm_files(dir_real, &nfiles);

        for (int q = 0; q < nfiles; q++) {
            /* 跨目录同名文件报错（模块文件名是唯一标识） */
            for (int s = 0; s < nseen; s++) {
                if (strcmp(seen_names[s], files[q]) == 0) {
                    LOG_ERROR("[scan] 扫描目录出现同名文件 / duplicate file name in scan dirs: %s\n",
                              files[q]);
                    for (int k = q; k < nfiles; k++) free(files[k]);
                    free(files);
                    for (int i = 0; i < nseen; i++) free(seen_names[i]);
                    free(seen_names);
                    for (int i = 0; i < ndirs; i++) free(dirs[i]);
                    free(dirs);
                    free(bodies.buf);
                    app_scan_free(r);
                    return NULL;
                }
            }
            char file_real[PATH_MAX];
            join_path(file_real, sizeof file_real, dir_real, files[q]);
#ifdef _WIN32
            char resolved[PATH_MAX];
            if (_fullpath(resolved, file_real, PATH_MAX)) {
                snprintf(file_real, sizeof file_real, "%s", resolved);
            }
#else
            char resolved[PATH_MAX];
            if (realpath(file_real, resolved)) {
                snprintf(file_real, sizeof file_real, "%s", resolved);
            }
#endif

            char* body = NULL;
            int mod_id = lm_expand_scanned_module(file_real, main_real, &body);
            if (mod_id < 0) {
                LOG_ERROR("[scan] 展开扫描模块失败 / failed to expand scanned module: %s\n",
                          file_real);
                for (int k = q; k < nfiles; k++) free(files[k]);
                free(files);
                for (int i = 0; i < nseen; i++) free(seen_names[i]);
                free(seen_names);
                for (int i = 0; i < ndirs; i++) free(dirs[i]);
                free(dirs);
                free(bodies.buf);
                app_scan_free(r);
                return NULL;
            }

            if (body) { accum_puts(&bodies, body); free(body); }

            seen_names = (char**)realloc(seen_names, (size_t)(nseen + 1) * sizeof(char*));
            seen_names[nseen++] = strdup(files[q]);

            if (r->count >= r->cap) {
                r->cap = r->cap ? r->cap * 2 : 8;
                r->mods = (ScannedModule*)realloc(r->mods,
                              (size_t)r->cap * sizeof(ScannedModule));
            }
            r->mods[r->count].abs_path = strdup(file_real);
            r->mods[r->count].module_id = mod_id;
            r->count++;
        }

        for (int q = 0; q < nfiles; q++) free(files[q]);
        free(files);
    }

    for (int i = 0; i < nseen; i++) free(seen_names[i]);
    free(seen_names);
    for (int i = 0; i < ndirs; i++) free(dirs[i]);
    free(dirs);

    *scanned_bodies = bodies.buf;
    g_last_scan = r;
    return r;
}

void app_scan_free(AppScanResult* r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) free(r->mods[i].abs_path);
    free(r->mods);
    free(r->config_path);
    free(r->config_dir);
    free(r);
}
