// lm_file.c —— 文件对象（VAL_FILE）与目录对象（VAL_FOLDER）
// 不持有 FILE* 句柄：每次方法调用 fopen/fclose，避免 GC 回收时的资源泄漏
#include "lm_file.h"
#include "lm_array.h"
#include "lm_container.h"
#include "gc_runtime.h"
#include "lm_blocking_pool.h" /* Phase 8.5：阻塞 syscall 流放 blocking 池，调度线程不阻塞 */
#include "lm_scheduler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <fnmatch.h>
#include <libgen.h>

/* 把 err 对应的系统错误文本写入调用方栈缓冲（可重入）。
 * 兼容两种 strerror_r 签名：GNU 版可能返回独立静态串（需拷入 buf），
 * XSI 版（macOS/BSD）直接写入 buf。多 worker/blocking 池线程共用，
 * 不得使用非线程安全保证的 strerror()。 */
static void file_errno_text(int err, char* buf, size_t n) {
#if defined(__GLIBC__) && (_GNU_SOURCE)
    char* r = strerror_r(err, buf, n);
    if(r && r != buf) snprintf(buf, n, "%s", r);
#else
    (void)strerror_r(err, buf, n);
#endif
}

/* ============================================================
 * 路由约定（本文件所有阻塞操作统一遵循）
 * ------------------------------------------------------------
 * 每个阻塞操作提供 *_impl 纯实现 + 路由包装：
 *   - 协程/调度器上下文：lm_co_await_blocking 提交 blocking 池并 yield，
 *     操作在独立池线程执行，完成回投续行——调度线程不冻结；
 *   - 非协程（主线程启动）或提交失败：直接执行 impl，功能不降级。
 * 池线程无协程/调度器上下文，故递归树操作内部调用路由包装时自动走
 * impl（直接执行），整棵树只占一个池任务、只 yield 一次。
 * Value/GC 只在协程线程触碰；池任务只做纯 C 操作，结果经 malloc
 * 缓冲/C 字符串列表（strList）带回。
 * ============================================================ */

// C 字符串动态列表：阻塞任务内收集路径（不触碰 Value/GC），
// 回协程后再转换为 VAL_ARRAY
typedef struct {
    char** items;
    int    len;
    int    cap;
} strList;

static void strList_init(strList* l) {
    l->items = NULL; l->len = 0; l->cap = 0;
}

static int strList_add(strList* l, const char* s) {
    if (l->len == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 16;
        char** ni = (char**)realloc(l->items, (size_t)ncap * sizeof(char*));
        if (!ni) return -1;
        l->items = ni; l->cap = ncap;
    }
    l->items[l->len] = strdup(s);
    if (!l->items[l->len]) return -1;
    l->len++;
    return 0;
}

static void strList_free(strList* l) {
    for (int i = 0; i < l->len; i++) free(l->items[i]);
    free(l->items);
}

// ===== 内部辅助 =====

// 标准化模式字符串：返回 fopen 兼容的模式（"rb"/"wb"/"ab"）
static const char* norm_mode(const char* mode) {
    if (!mode || !*mode) return "rb";
    if (mode[0] == 'w' || mode[0] == 'W') return "wb";
    if (mode[0] == 'a' || mode[0] == 'A') return "ab";
    return "rb";  // 默认读
}

/* ---- stat 系列：path_exists / path_is_dir / file_size ---- */

static int stat_impl(const char* path, struct stat* st) {
    return stat(path, st) == 0;
}

typedef struct {
    const char*   path;
    struct stat   st;
    int           ok;
} statCtx;

static void* stat_blocking(void* arg) {
    statCtx* c = (statCtx*)arg;
    c->ok = stat_impl(c->path, &c->st);
    return c;
}

/* 路由版 stat：返回 1 成功 / 0 失败；成功时可选拷贝 stat 结构。 */
static int run_stat(const char* path, struct stat* out) {
    statCtx c;
    c.path = path; c.ok = 0; memset(&c.st, 0, sizeof(c.st));
    if (lm_co_await_blocking(stat_blocking, &c, NULL) == 0) {
        if (c.ok && out) memcpy(out, &c.st, sizeof(*out));
        return c.ok;
    }
    struct stat st;
    int ok = stat_impl(path, &st);
    if (ok && out) memcpy(out, &st, sizeof(*out));
    return ok;
}

// 文件是否存在
static int path_exists(const char* path) {
    return run_stat(path, NULL);
}

// 是否为目录
static int path_is_dir(const char* path) {
    struct stat st;
    if (!run_stat(path, &st)) return 0;
    return S_ISDIR(st.st_mode);
}

// 获取文件大小（字节）
static long file_size(const char* path) {
    struct stat st;
    if (!run_stat(path, &st)) return -1;
    return (long)st.st_size;
}

/* ---- 读文件 ---- */

// 读取整个文件到 malloc 缓冲（调用方 free）——纯实现
static char* read_whole_impl(const char* path, long* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    if (out_len) *out_len = (long)rd;
    return buf;
}

typedef struct {
    const char* path;
    long        len;
    char*       buf;
} readCtx;

static void* read_blocking(void* arg) {
    readCtx* c = (readCtx*)arg;
    c->buf = read_whole_impl(c->path, &c->len);
    return c;
}

// 读取整个文件到 malloc 缓冲（调用方 free）——路由版
static char* read_whole_file(const char* path, long* out_len) {
    readCtx c;
    c.path = path; c.len = 0; c.buf = NULL;
    if (lm_co_await_blocking(read_blocking, &c, NULL) == 0) {
        if (out_len) *out_len = c.len;
        return c.buf;
    }
    return read_whole_impl(path, out_len);
}

/* ---- 写文件 ---- */

// 将整块缓冲以指定 fopen 模式写入——纯实现，返回 0 成功 / -1 失败（打不开或写不完整）
static int write_buf_impl(const char* path, const char* mode,
                          const void* data, size_t len) {
    FILE* f = fopen(path, mode);
    if (!f) return -1;
    size_t wr = fwrite(data ? data : "", 1, len, f);
    fclose(f);
    return wr == len ? 0 : -1;
}

typedef struct {
    const char* path;
    const char* mode;
    const void* data;
    size_t      len;
    int         rc;
} writeCtx;

static void* write_blocking(void* arg) {
    writeCtx* c = (writeCtx*)arg;
    c->rc = write_buf_impl(c->path, c->mode, c->data, c->len);
    return c;
}

// 路由版整块写入
static int file_write_buf(const char* path, const char* mode,
                          const void* data, size_t len) {
    writeCtx c;
    c.path = path; c.mode = mode; c.data = data; c.len = len; c.rc = -1;
    if (lm_co_await_blocking(write_blocking, &c, NULL) == 0) return c.rc;
    return write_buf_impl(path, mode, data, len);
}

/* ---- unlink / rename / truncate ---- */

typedef struct {
    const char* a;
    const char* b;
    int64_t     sz;
    int         rc;
    int         errNo;   /* 失败时的 errno（池线程 errno 不跨线程可见，显式带回） */
} fsSysCtx;

static void* unlink_blocking(void* arg) {
    fsSysCtx* c = (fsSysCtx*)arg;
    if (unlink(c->a) == 0) { c->rc = 0; }
    else { c->rc = -1; c->errNo = errno; }
    return c;
}

static void file_unlink_route(const char* path, int* rc, int* errNo) {
    fsSysCtx c;
    c.a = path; c.b = NULL; c.sz = 0; c.rc = -1; c.errNo = 0;
    if (lm_co_await_blocking(unlink_blocking, &c, NULL) == 0) {
        *rc = c.rc; *errNo = c.errNo; return;
    }
    if (unlink(path) == 0) { *rc = 0; *errNo = 0; }
    else { *rc = -1; *errNo = errno; }
}

static void* rename_blocking(void* arg) {
    fsSysCtx* c = (fsSysCtx*)arg;
    if (rename(c->a, c->b) == 0) { c->rc = 0; }
    else { c->rc = -1; c->errNo = errno; }
    return c;
}

static int file_rename_route(const char* a, const char* b, int* errNo) {
    fsSysCtx c;
    c.a = a; c.b = b; c.sz = 0; c.rc = -1; c.errNo = 0;
    if (lm_co_await_blocking(rename_blocking, &c, NULL) == 0) {
        if (errNo) *errNo = c.errNo;
        return c.rc;
    }
    if (rename(a, b) == 0) { if (errNo) *errNo = 0; return 0; }
    if (errNo) *errNo = errno;
    return -1;
}

static void* truncate_blocking(void* arg) {
    fsSysCtx* c = (fsSysCtx*)arg;
    if (truncate(c->a, (off_t)c->sz) == 0) { c->rc = 0; }
    else { c->rc = -1; c->errNo = errno; }
    return c;
}

static int file_truncate_route(const char* path, int64_t sz, int* errNo) {
    fsSysCtx c;
    c.a = path; c.b = NULL; c.sz = sz; c.rc = -1; c.errNo = 0;
    if (lm_co_await_blocking(truncate_blocking, &c, NULL) == 0) {
        if (errNo) *errNo = c.errNo;
        return c.rc;
    }
    if (truncate(path, (off_t)sz) == 0) { if (errNo) *errNo = 0; return 0; }
    if (errNo) *errNo = errno;
    return -1;
}

/* ---- 单文件复制（read+write 合成一个池任务） ---- */

static int copy_file_impl(const char* src, const char* dest) {
    long sz = 0;
    char* content = read_whole_impl(src, &sz);
    if (!content) return -1;
    int rc = write_buf_impl(dest, "wb", content, (size_t)sz);
    free(content);
    return rc;
}

typedef struct {
    const char* src;
    const char* dest;
    int         rc;
} copyFileCtx;

static void* copy_file_blocking(void* arg) {
    copyFileCtx* c = (copyFileCtx*)arg;
    c->rc = copy_file_impl(c->src, c->dest);
    return c;
}

static int file_copy_route(const char* src, const char* dest) {
    copyFileCtx c;
    c.src = src; c.dest = dest; c.rc = -1;
    if (lm_co_await_blocking(copy_file_blocking, &c, NULL) == 0) return c.rc;
    return copy_file_impl(src, dest);
}

// 统一获取文件内容（磁盘 fopen 或内存文件拷贝）到 malloc 缓冲（调用方 free）
static char* file_get_content(FileObj* o, long* out_len) {
    if (o->content) {
        char* buf = (char*)malloc((size_t)o->contentLen + 1);
        if (!buf) return NULL;
        memcpy(buf, o->content, o->contentLen);
        buf[o->contentLen] = '\0';
        if (out_len) *out_len = (long)o->contentLen;
        return buf;
    }
    return read_whole_file(o->path, out_len);
}

// 计算字符串中的行数（与 split_lines 一致：末尾 \n 不增加空行）
static int count_lines(const char* s) {
    if (!s || !*s) return 0;
    int n = 0;
    const char* p = s;
    while (*p) {
        if (*p == '\n') n++;
        p++;
    }
    // 末尾非 \n 则最后一行未计入
    if (p > s && *(p - 1) != '\n') n++;
    return n;
}

// 将文本按行拆分到 Value 数组（VAL_STRING 元素，\n 不保留）
// 行数与 count_lines 一致：末尾 \n 不增加空行
static Value split_lines(const char* s) {
    if (!s) s = "";
    int total = count_lines(s);
    Value arr = val_array(total);
    if (total == 0) return arr;
    const char* start = s;
    int idx = 0;
    const char* p = s;
    while (*p) {
        if (*p == '\n') {
            int len = (int)(p - start);
            char* line = (char*)malloc((size_t)len + 1);
            if (line) {
                memcpy(line, start, (size_t)len);
                line[len] = '\0';
                arr.v.array->items[idx++] = lumyr_make_string(line);
                free(line);
            }
            start = p + 1;
        }
        p++;
    }
    // 处理末尾非 \n 的最后一行
    if (start < p) {
        int len = (int)(p - start);
        char* line = (char*)malloc((size_t)len + 1);
        if (line) {
            memcpy(line, start, (size_t)len);
            line[len] = '\0';
            arr.v.array->items[idx++] = lumyr_make_string(line);
            free(line);
        }
    }
    return arr;
}

// 规整行号：负数表示从末尾倒数（-1 = 最后一行），返回 0-based 索引；越界返回 -1
static int norm_line_no(int64_t line_no, int total) {
    if (total <= 0) return -1;
    if (line_no < 0) line_no += total;  // -1 → total-1
    if (line_no < 0 || line_no >= total) return -1;
    return (int)line_no;
}

// 将字符串数组拼成"每行以 \n 结尾"的 malloc 缓冲（调用方 free）。
// 必须在协程线程调用（触碰 Value）；拼装结果再整块流放 blocking 池写入。
static char* join_lines(Value lines, size_t* outLen) {
    int n = lines.v.array ? lines.v.array->len : 0;
    size_t total = 0;
    const char** cstrs = (const char**)calloc((size_t)n, sizeof(char*));
    if (!cstrs) return NULL;
    for (int i = 0; i < n; i++) {
        cstrs[i] = lumyr_str_cstr(&lines.v.array->items[i]);
        if (!cstrs[i]) cstrs[i] = "";
        total += strlen(cstrs[i]) + 1;   // +1 给 '\n'
    }
    /* total 为全部字节数；+1 给末尾 '\0'（空数组时 total=0 仍需一字节）。 */
    char* buf = (char*)malloc(total + 1);
    if (!buf) { free(cstrs); return NULL; }
    size_t off = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(cstrs[i]);
        memcpy(buf + off, cstrs[i], l);
        off += l;
        buf[off++] = '\n';
    }
    free(cstrs);
    *(buf + off) = '\0';   /* 多分配的一字节（0 行时 buf 长度 1） */
    if (outLen) *outLen = off;
    return buf;
}

// 递归删除目录（rmdir 非递归只删空目录）——纯实现（池线程调用）
static int remove_dir_impl(const char* path) {
    DIR* d = opendir(path);
    if (!d) return -1;
    struct dirent* ent;
    int rc = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (path_is_dir(child)) {
            if (remove_dir_impl(child) != 0) { rc = -1; }
        } else {
            if (unlink(child) != 0) { rc = -1; }
        }
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}

typedef struct {
    const char* path;
    int         rc;
} dirOpCtx;

static void* remove_dir_blocking(void* arg) {
    dirOpCtx* c = (dirOpCtx*)arg;
    c->rc = remove_dir_impl(c->path);
    return c;
}

// 递归删除目录——路由版（整棵树一个池任务）
static int remove_dir_recursive(const char* path) {
    dirOpCtx c;
    c.path = path; c.rc = -1;
    if (lm_co_await_blocking(remove_dir_blocking, &c, NULL) == 0) return c.rc;
    return remove_dir_impl(path);
}

// 递归复制目录到 dest——纯实现
static int copy_dir_impl(const char* src, const char* dest) {
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    // 创建目标目录
    mkdir(dest, 0755);
    DIR* d = opendir(src);
    if (!d) return -1;
    struct dirent* ent;
    int rc = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char src_child[4096];
        char dst_child[4096];
        snprintf(src_child, sizeof(src_child), "%s/%s", src, ent->d_name);
        snprintf(dst_child, sizeof(dst_child), "%s/%s", dest, ent->d_name);
        if (path_is_dir(src_child)) {
            if (copy_dir_impl(src_child, dst_child) != 0) rc = -1;
        } else {
            // 复制单个文件（read+write 直接走纯实现，本处已在池线程）
            if (copy_file_impl(src_child, dst_child) != 0) rc = -1;
        }
    }
    closedir(d);
    return rc;
}

typedef struct {
    const char* src;
    const char* dest;
    int         rc;
} copyDirCtx;

static void* copy_dir_blocking(void* arg) {
    copyDirCtx* c = (copyDirCtx*)arg;
    c->rc = copy_dir_impl(c->src, c->dest);
    return c;
}

// 递归复制目录——路由版
static int copy_dir_recursive(const char* src, const char* dest) {
    copyDirCtx c;
    c.src = src; c.dest = dest; c.rc = -1;
    if (lm_co_await_blocking(copy_dir_blocking, &c, NULL) == 0) return c.rc;
    return copy_dir_impl(src, dest);
}

/* 递归遍历目录收集文件路径——纯实现，结果写入 strList（不触碰 Value/GC）。 */
static void walk_impl(const char* path, strList* out) {
    DIR* d = opendir(path);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (path_is_dir(child)) {
            walk_impl(child, out);
        } else {
            strList_add(out, child);
        }
    }
    closedir(d);
}

typedef struct {
    const char* path;
    strList*    out;
} walkCtx;

static void* walk_blocking(void* arg) {
    walkCtx* c = (walkCtx*)arg;
    walk_impl(c->path, c->out);
    return c;
}

// 递归遍历目录，将所有文件路径追加到 arr（Value 拼装在协程线程）
static void walk_dir_append(const char* path, Value arr) {
    strList out;
    strList_init(&out);
    walkCtx c;
    c.path = path; c.out = &out;
    if (lm_co_await_blocking(walk_blocking, &c, NULL) != 0) {
        walk_impl(path, &out);
    }
    for (int i = 0; i < out.len; i++) {
        lumyr_array_add(&arr, lumyr_make_string(out.items[i]));
    }
    strList_free(&out);
}

/* 列出目录条目——纯实现。filter：0=全部，1=只文件，2=只目录。 */
static void list_impl(const char* path, int filter, strList* out) {
    DIR* d = opendir(path);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        int is_d = path_is_dir(child);
        if (filter == 1 && is_d) continue;
        if (filter == 2 && !is_d) continue;
        strList_add(out, ent->d_name);
    }
    closedir(d);
}

typedef struct {
    const char* path;
    int         filter;
    strList*    out;
} listCtx;

static void* list_blocking(void* arg) {
    listCtx* c = (listCtx*)arg;
    list_impl(c->path, c->filter, c->out);
    return c;
}

// 列出目录条目，filter：0=全部，1=只文件，2=只目录
static Value list_entries(const char* path, int filter) {
    strList out;
    strList_init(&out);
    listCtx c;
    c.path = path; c.filter = filter; c.out = &out;
    if (lm_co_await_blocking(list_blocking, &c, NULL) != 0) {
        list_impl(path, filter, &out);
    }
    Value arr = val_array(out.len);
    for (int i = 0; i < out.len; i++) {
        arr.v.array->items[i] = lumyr_make_string(out.items[i]);
    }
    strList_free(&out);
    return arr;
}

/* 目录直接子条目数——纯实现 */
static int dir_count_impl(const char* path) {
    DIR* d = opendir(path);
    if (!d) return 0;
    struct dirent* ent;
    int n = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        n++;
    }
    closedir(d);
    return n;
}

static void* dir_count_blocking(void* arg) {
    dirOpCtx* c = (dirOpCtx*)arg;
    c->rc = dir_count_impl(c->path);
    return c;
}

// 计算目录直接子条目数
static int dir_count(const char* path) {
    dirOpCtx c;
    c.path = path; c.rc = 0;
    if (lm_co_await_blocking(dir_count_blocking, &c, NULL) == 0) return c.rc;
    return dir_count_impl(path);
}

/* 递归计算目录总大小——纯实现（所有文件字节数之和） */
static int64_t dir_total_impl(const char* path) {
    DIR* d = opendir(path);
    if (!d) return 0;
    struct dirent* ent;
    int64_t total = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (path_is_dir(child)) {
            total += dir_total_impl(child);
        } else {
            total += file_size(child);
        }
    }
    closedir(d);
    return total;
}

typedef struct {
    const char* path;
    int64_t     total;
} dirSizeCtx;

static void* dir_total_blocking(void* arg) {
    dirSizeCtx* c = (dirSizeCtx*)arg;
    c->total = dir_total_impl(c->path);
    return c;
}

static int64_t dir_total_size(const char* path) {
    if (!path_exists(path)) return 0;
    dirSizeCtx c;
    c.path = path; c.total = 0;
    if (lm_co_await_blocking(dir_total_blocking, &c, NULL) == 0) return c.total;
    return dir_total_impl(path);
}

// ===== file 公共 API =====

Value lumyr_file_make(const char* path, const char* mode) {
    FileObj* o = (FileObj*)gc_alloc(sizeof(FileObj), VAL_FILE);
    if (!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    // 复制路径和模式字符串到 GC 堆
    size_t plen = path ? strlen(path) : 0;
    o->path = (char*)gc_alloc(plen + 1, VAL_STRING);
    if (o->path) {
        memcpy(o->path, path ? path : "", plen + 1);
    }
    const char* m = norm_mode(mode);
    // 简化存储：存原始字符 r/w/a，对外显示时补全
    char mc[2] = { (char)(m[0] == 'w' ? 'w' : (m[0] == 'a' ? 'a' : 'r')), '\0' };
    o->mode = (char*)gc_alloc(2, VAL_STRING);
    if (o->mode) {
        o->mode[0] = mc[0];
        o->mode[1] = '\0';
    }
    o->stack_alloc = 0;
    Value r;
    r.type = VAL_FILE;
    r.str_inline = 0;
    r.v.file_obj = o;
    return r;
}

// 内存文件：file(name, bytes(...))
Value lumyr_file_from_bytes(const char* name, Value b) {
    FileObj* o = (FileObj*)gc_alloc(sizeof(FileObj), VAL_FILE);
    if (!o) { Value z; z.type = VAL_NONE; z.str_inline = 0; return z; }

    size_t nlen = name ? strlen(name) : 0;
    o->path = (char*)gc_alloc(nlen + 1, VAL_STRING);
    if (o->path) memcpy(o->path, name ? name : "", nlen + 1);

    o->mode = (char*)gc_alloc(2, VAL_STRING);
    if (o->mode) { o->mode[0] = 'r'; o->mode[1] = '\0'; }

    int blen = 0;
    const uint8_t* bdata = NULL;
    if (b.type == VAL_BYTES && b.v.bytes_obj) {
        BytesObj* bo = (BytesObj*)b.v.bytes_obj;
        blen = bo->len;
        bdata = bo->data;
    }
    o->contentLen = blen;
    if (blen > 0) {
        o->content = (uint8_t*)gc_alloc(sizeof(uint8_t) * blen, VAL_FILE);
        if (o->content) memcpy(o->content, bdata, blen);
        else o->contentLen = 0;
    } else {
        o->content = NULL;
    }
    o->stack_alloc = 0;

    Value r;
    r.type = VAL_FILE;
    r.str_inline = 0;
    r.v.file_obj = o;
    return r;
}

Value lumyr_file_field(Value v, const char* name) {
    if (!name) return lumyr_make_int(0);
    if (v.type != VAL_FILE) return lumyr_make_int(0);
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) return lumyr_make_int(0);

    if (strcmp(name, "path") == 0)   return lumyr_make_string(o->path);
    if (strcmp(name, "mode") == 0)  return lumyr_make_string(o->mode ? o->mode : "r");
    if (strcmp(name, "exists") == 0)
        return lumyr_make_bool(o->content ? 1 : path_exists(o->path));
    if (strcmp(name, "size") == 0)
        return lumyr_make_int(o->content ? o->contentLen : file_size(o->path));
    if (strcmp(name, "isOpen") == 0) return lumyr_make_bool(0);  // 无持久句柄
    if (strcmp(name, "lines") == 0) {
        long sz = 0;
        char* content = file_get_content(o, &sz);
        int n = content ? count_lines(content) : 0;
        free(content);
        return lumyr_make_int(n);
    }
    if (strcmp(name, "name") == 0) {
        /* 文件名（不含目录） */
        char* dup = strdup(o->path);
        char* bn = basename(dup);
        Value r = lumyr_make_string(bn);
        free(dup);
        return r;
    }
    if (strcmp(name, "ext") == 0) {
        /* 扩展名（不含 .，无扩展返回空串） */
        char* dup = strdup(o->path);
        char* bn = basename(dup);
        char* dot = strrchr(bn, '.');
        Value r = (dot && dot != bn) ? lumyr_make_string(dot + 1) : lumyr_make_string("");
        free(dup);
        return r;
    }
    if (strcmp(name, "mtime") == 0) {
        /* 最后修改时间（epoch 秒）；run_stat 协程内自动流放 blocking 池 */
        struct stat st;
        if (!run_stat(o->path, &st)) {
            char buf[256];
            snprintf(buf, sizeof buf,
                     "file.mtime：无法读取文件状态 \"%s\" / file.mtime: cannot stat \"%s\"",
                     o->path, o->path);
            runtime_error(buf);
            return lumyr_make_int(0);
        }
        return lumyr_make_int64((int64_t)st.st_mtime);
    }
    /* 无兜底：未知字段/方法名 → AttributeError（方法解析已先完成），
     * 禁止静默返回 0 */
    {
        char buf[256];
        snprintf(buf, sizeof buf,
                 "file 没有字段或方法 \"%s\" / file has no field or method \"%s\"",
                 name, name);
        runtime_error(buf);
    }
    return lumyr_make_int(0);   /* 不可达 */
}

char* lumyr_file_to_str(Value v) {
    if (v.type != VAL_FILE) return strdup("");
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) return strdup("");
    char buf[512];
    snprintf(buf, sizeof(buf), "<file %s>", o->path);
    return strdup(buf);
}

Value lumyr_file_read_all(Value v) {
    if (v.type != VAL_FILE) { runtime_error("readAll() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("readAll() 文件对象无效"); return val_none(); }
    long sz = 0;
    char* content = file_get_content(o, &sz);
    if (!content) {
        char buf[512];
        snprintf(buf, sizeof(buf), "readAll() 无法读取文件: %s", o->path);
        runtime_error(buf);
        return val_none();
    }
    Value r = lumyr_make_string(content);
    free(content);
    return r;
}

Value lumyr_file_read_lines(Value v) {
    if (v.type != VAL_FILE) { runtime_error("readLines() 仅适用于 file 对象"); return val_array(0); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("readLines() 文件对象无效"); return val_array(0); }
    long sz = 0;
    char* content = read_whole_file(o->path, &sz);
    if (!content) {
        char buf[512];
        snprintf(buf, sizeof(buf), "readLines() 无法读取文件: %s", o->path);
        runtime_error(buf);
        return val_array(0);
    }
    Value r = split_lines(content);
    free(content);
    return r;
}

Value lumyr_file_read_line(Value v, int64_t line_no) {
    if (v.type != VAL_FILE) { runtime_error("readLine() 仅适用于 file 对象"); return lumyr_make_string(""); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("readLine() 文件对象无效"); return lumyr_make_string(""); }
    long sz = 0;
    char* content = read_whole_file(o->path, &sz);
    if (!content) {
        char buf[512];
        snprintf(buf, sizeof(buf), "readLine() 无法读取文件: %s", o->path);
        runtime_error(buf);
        return lumyr_make_string("");
    }
    int total = count_lines(content);
    int idx = norm_line_no(line_no, total);
    if (idx < 0) {
        free(content);
        char buf[256];
        snprintf(buf, sizeof(buf), "readLine() 行号越界: %lld（共 %d 行）", (long long)line_no, total);
        runtime_error(buf);
        return lumyr_make_string("");
    }
    Value lines = split_lines(content);
    free(content);
    if (idx >= lines.v.array->len) return lumyr_make_string("");
    Value r = lines.v.array->items[idx];
    return r;
}

Value lumyr_file_read_lines_range(Value v, int64_t from, int64_t to) {
    if (v.type != VAL_FILE) { runtime_error("readLines(from,to) 仅适用于 file 对象"); return val_array(0); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("readLines(from,to) 文件对象无效"); return val_array(0); }
    long sz = 0;
    char* content = file_get_content(o, &sz);
    if (!content) {
        char buf[512];
        snprintf(buf, sizeof(buf), "readLines(from,to) 无法读取文件: %s", o->path);
        runtime_error(buf);
        return val_array(0);
    }
    int total = count_lines(content);
    int i_from = norm_line_no(from, total);
    int i_to = norm_line_no(to, total);
    if (i_from < 0 || i_to < 0 || i_from > i_to) {
        free(content);
        char buf[256];
        snprintf(buf, sizeof(buf), "readLines(from,to) 行号范围无效: %lld..%lld（共 %d 行）",
                 (long long)from, (long long)to, total);
        runtime_error(buf);
        return val_array(0);
    }
    Value all = split_lines(content);
    free(content);
    int n = i_to - i_from + 1;
    Value r = val_array(n);
    for (int i = 0; i < n; i++) {
        r.v.array->items[i] = all.v.array->items[i_from + i];
    }
    return r;
}

Value lumyr_file_write_all(Value v, const char* content) {
    if (v.type != VAL_FILE) { runtime_error("writeAll() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("writeAll() 文件对象无效"); return val_none(); }
    const char* m = norm_mode(o->mode);
    // writeAll 始终覆盖写；协程内流放 blocking 池，调度线程不阻塞
    size_t len = content ? strlen(content) : 0;
    if (file_write_buf(o->path, "wb", content ? content : "", len) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "writeAll() 无法打开文件（写入）或写入不完整: %s", o->path);
        runtime_error(buf);
        return val_none();
    }
    (void)m;
    return val_none();
}

Value lumyr_file_write_line(Value v, int64_t line_no, const char* content) {
    if (v.type != VAL_FILE) { runtime_error("writeLine() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("writeLine() 文件对象无效"); return val_none(); }
    long sz = 0;
    char* old = read_whole_file(o->path, &sz);
    int total = old ? count_lines(old) : 0;
    int idx = norm_line_no(line_no, total);
    Value lines = old ? split_lines(old) : val_array(0);
    free(old);
    if (idx < 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "writeLine() 行号越界: %lld（共 %d 行）", (long long)line_no, total);
        runtime_error(buf);
        return val_none();
    }
    // 替换第 idx 行
    lines.v.array->items[idx] = lumyr_make_string(content ? content : "");
    // 协程线程拼成整块，再流放 blocking 池写入（调度线程不阻塞）
    size_t bufLen = 0;
    char* buf = join_lines(lines, &bufLen);
    if (!buf || file_write_buf(o->path, "wb", buf, bufLen) != 0) {
        free(buf);
        char buf2[512];
        snprintf(buf2, sizeof(buf2), "writeLine() 无法打开文件（写回）或写入失败: %s", o->path);
        runtime_error(buf2);
        return val_none();
    }
    free(buf);
    return val_none();
}

Value lumyr_file_insert_line(Value v, int64_t line_no, const char* content) {
    if (v.type != VAL_FILE) { runtime_error("insertLine() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("insertLine() 文件对象无效"); return val_none(); }
    long sz = 0;
    char* old = read_whole_file(o->path, &sz);
    int total = old ? count_lines(old) : 0;
    Value lines = old ? split_lines(old) : val_array(0);
    free(old);
    /* 规整行号：负数倒数，越界则追加到末尾 */
    int idx;
    if (line_no < 0) {
        idx = (int)(total + line_no);  /* -1 → 最后一行之前插入 */
        if (idx < 0) idx = 0;
    } else {
        idx = (int)line_no;
        if (idx > total) idx = total;  /* 超界追加到末尾 */
    }
    /* 构造新数组：idx 之前 + 新行 + idx 之后 */
    int new_total = total + 1;
    Value result = val_array(new_total);
    int j = 0;
    for (int i = 0; i < idx; i++) result.v.array->items[j++] = lines.v.array->items[i];
    result.v.array->items[j++] = lumyr_make_string(content ? content : "");
    for (int i = idx; i < total; i++) result.v.array->items[j++] = lines.v.array->items[i];
    /* 协程线程拼成整块，流放 blocking 池写回 */
    size_t bufLen = 0;
    char* buf = join_lines(result, &bufLen);
    if (!buf || file_write_buf(o->path, "wb", buf, bufLen) != 0) {
        free(buf);
        char buf2[512];
        snprintf(buf2, sizeof(buf2), "insertLine() 无法打开文件（写回）或写入失败: %s", o->path);
        runtime_error(buf2);
        return val_none();
    }
    free(buf);
    return val_none();
}

Value lumyr_file_write_lines(Value v, Value arr) {
    if (v.type != VAL_FILE) { runtime_error("writeLines() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("writeLines() 文件对象无效"); return val_none(); }
    if (arr.type != VAL_ARRAY) { runtime_error("writeLines() 参数必须是字符串数组"); return val_none(); }
    size_t bufLen = 0;
    char* buf = join_lines(arr, &bufLen);
    if (!buf || file_write_buf(o->path, "wb", buf, bufLen) != 0) {
        free(buf);
        char buf2[512];
        snprintf(buf2, sizeof(buf2), "writeLines() 无法打开文件（写入）或写入失败: %s", o->path);
        runtime_error(buf2);
        return val_none();
    }
    free(buf);
    return val_none();
}

Value lumyr_file_append(Value v, const char* content) {
    if (v.type != VAL_FILE) { runtime_error("append() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("append() 文件对象无效"); return val_none(); }
    size_t len = content ? strlen(content) : 0;
    if (file_write_buf(o->path, "ab", content ? content : "", len) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "append() 无法打开文件（追加）或写入不完整: %s", o->path);
        runtime_error(buf);
        return val_none();
    }
    return val_none();
}

Value lumyr_file_append_line(Value v, const char* content) {
    if (v.type != VAL_FILE) { runtime_error("appendLine() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("appendLine() 文件对象无效"); return val_none(); }
    size_t len = content ? strlen(content) : 0;
    /* 协程线程拼出 content+'\n'，整块流放 blocking 池追加 */
    char* buf = (char*)malloc(len + 1);
    if (!buf) { runtime_error("appendLine() 内存不足"); return val_none(); }
    if (content) memcpy(buf, content, len);
    buf[len] = '\n';
    if (file_write_buf(o->path, "ab", buf, len + 1) != 0) {
        free(buf);
        char buf2[512];
        snprintf(buf2, sizeof(buf2), "appendLine() 无法打开文件（追加）或写入失败: %s", o->path);
        runtime_error(buf2);
        return val_none();
    }
    free(buf);
    return val_none();
}

Value lumyr_file_flush(Value v) {
    // 无持久句柄，flush 为 no-op
    (void)v;
    return val_none();
}

Value lumyr_file_delete(Value v) {
    if (v.type != VAL_FILE) { runtime_error("delete() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("delete() 文件对象无效"); return val_none(); }
    int rc = 0, errNo = 0;
    file_unlink_route(o->path, &rc, &errNo);
    if (rc != 0) {
        char buf[512], why[160];
        file_errno_text(errNo, why, sizeof(why));
        snprintf(buf, sizeof(buf), "delete() 无法删除文件: %s (%s)", o->path, why);
        runtime_error(buf);
        return val_none();
    }
    return val_none();
}

/* ===== file 二进制 I/O + 文件管理 ===== */

Value lumyr_file_read_bytes(Value v) {
    if (v.type != VAL_FILE) { runtime_error("readBytes() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("readBytes() 文件对象无效"); return val_none(); }
    long sz = 0;
    char* content = file_get_content(o, &sz);
    if (!content) {
        char buf[512];
        snprintf(buf, sizeof(buf), "readBytes() 无法读取文件: %s", o->path);
        runtime_error(buf);
        return val_none();
    }
    /* 构造 BytesObj */
    BytesObj* bo = (BytesObj*)gc_alloc(sizeof(BytesObj), VAL_BYTES);
    if (!bo) { free(content); return val_none(); }
    bo->len = (int)sz;
    bo->data = (uint8_t*)gc_alloc((size_t)sz + 1, VAL_BYTES);
    if (bo->data) {
        memcpy(bo->data, content, (size_t)sz);
    }
    bo->stack_alloc = 0;
    free(content);
    Value r;
    r.type = VAL_BYTES;
    r.str_inline = 0;
    r.v.bytes_obj = bo;
    return r;
}

Value lumyr_file_write_bytes(Value v, Value b) {
    if (v.type != VAL_FILE) { runtime_error("writeBytes() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("writeBytes() 文件对象无效"); return val_none(); }
    if (b.type != VAL_BYTES) { runtime_error("writeBytes() 参数必须是 bytes 对象"); return val_none(); }
    BytesObj* bo = (BytesObj*)b.v.bytes_obj;
    if (!bo) { runtime_error("writeBytes() bytes 对象无效"); return val_none(); }
    if (file_write_buf(o->path, "wb", bo->data, (size_t)bo->len) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "writeBytes() 无法打开文件或写入不完整: %s", o->path);
        runtime_error(buf);
        return val_none();
    }
    return val_none();
}

/* ============================================================
 * 流式分块 I/O
 * ------------------------------------------------------------
 * readChunk(n)：从 readPos 游标顺序读取最多 n 字节为 bytes，游标按
 *   实际读取字节数前进；到文件尾返回空 bytes。磁盘读走 blocking 池
 *   （fopen/fseek/fread/fclose 均为阻塞 syscall），内存文件直接拷贝。
 * appendBytes(b)：以 "ab" 二进制追加一块 bytes，不做全量缓冲。
 * 二者配合使大文件上传/落盘的内存占用保持 O(块大小)，与文件大小无关。
 * ============================================================ */

typedef struct {
    const char* path;
    int64_t     offset;
    int64_t     maxLen;
    uint8_t*    buf;      /* 输出：malloc 缓冲，调用方 free；EOF/失败为 NULL */
    int64_t     got;      /* 实际读取字节数，EOF=0，失败=-1 */
} chunkReadCtx;

static void* chunk_read_blocking(void* arg) {
    chunkReadCtx* c = (chunkReadCtx*)arg;
    c->buf = NULL;
    c->got = -1;
    FILE* f = fopen(c->path, "rb");
    if (!f) return c;
    if (c->offset > 0 && fseek(f, (long)c->offset, SEEK_SET) != 0) { fclose(f); return c; }
    if (c->maxLen <= 0) { fclose(f); c->got = 0; return c; }
    c->buf = (uint8_t*)malloc((size_t)c->maxLen);
    if (!c->buf) { fclose(f); return c; }
    size_t rd = fread(c->buf, 1, (size_t)c->maxLen, f);
    fclose(f);
    c->got = (int64_t)rd;
    if (rd == 0) { free(c->buf); c->buf = NULL; }
    return c;
}

Value lumyr_file_read_chunk(Value v, int64_t maxLen) {
    if (v.type != VAL_FILE) { runtime_error("readChunk() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("readChunk() 文件对象无效"); return val_none(); }
    if (maxLen <= 0) maxLen = 65536;

    /* 内存文件：直接从 content 游标拷贝，无 syscall */
    if (o->content) {
        int64_t remain = (int64_t)o->contentLen - o->readPos;
        if (remain <= 0) return lumyr_bytes_from_buf(NULL, 0);
        int64_t n = remain < maxLen ? remain : maxLen;
        Value r = lumyr_bytes_from_buf(o->content + o->readPos, (int)n);
        o->readPos += n;
        return r;
    }

    chunkReadCtx c;
    c.path = o->path; c.offset = o->readPos; c.maxLen = maxLen;
    c.buf = NULL; c.got = -1;
    if (lm_co_await_blocking(chunk_read_blocking, &c, NULL) != 0) {
        chunk_read_blocking(&c);
    }
    if (c.got < 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "readChunk() 无法读取文件: %s", o->path);
        runtime_error(buf);
        return val_none();
    }
    Value r = lumyr_bytes_from_buf(c.buf, (int)c.got);
    if (c.buf) free(c.buf);
    o->readPos += c.got;
    return r;
}

typedef struct {
    const char* path;
    const uint8_t* data;
    size_t      len;
    int         ok;       /* 0 成功 / -1 失败 */
} chunkWriteCtx;

static void* chunk_append_blocking(void* arg) {
    chunkWriteCtx* c = (chunkWriteCtx*)arg;
    c->ok = -1;
    FILE* f = fopen(c->path, "ab");
    if (!f) return c;
    size_t wr = c->len ? fwrite(c->data, 1, c->len, f) : 0;
    int rc = fclose(f);
    c->ok = (wr == c->len && rc == 0) ? 0 : -1;
    return c;
}

/* ============================================================
 * readInto：零分配流式读
 * fread 直接写入复用缓冲（GC 非移动式，data 指针跨 blocking 调用稳定；
 * 缓冲由调用栈保活），循环内不再 gc_alloc 新 bytes，从根上避免
 * 保守 C 栈扫描残留指针导致的流式垃圾驻留。
 * ============================================================ */

typedef struct {
    const char* path;
    int64_t     offset;
    uint8_t*    dst;      /* 调用方 bytes 缓冲 data（容量 cap） */
    int         cap;
    int64_t     got;      /* 实际读取字节数，EOF=0，失败=-1 */
} readIntoCtx;

static void* read_into_blocking(void* arg) {
    readIntoCtx* c = (readIntoCtx*)arg;
    c->got = -1;
    FILE* f = fopen(c->path, "rb");
    if (!f) return c;
    if (c->offset > 0 && fseek(f, (long)c->offset, SEEK_SET) != 0) { fclose(f); return c; }
    size_t rd = c->cap > 0 ? fread(c->dst, 1, (size_t)c->cap, f) : 0;
    fclose(f);
    c->got = (int64_t)rd;
    return c;
}

Value lumyr_file_read_into(Value v, Value buf, int64_t maxLen) {
    if (v.type != VAL_FILE) { runtime_error("readInto() 仅适用于 file 对象"); return lumyr_make_int(-1); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("readInto() 文件对象无效"); return lumyr_make_int(-1); }
    if (buf.type != VAL_BYTES) { runtime_error("readInto(buf) 参数必须是 bytes 定长缓冲 bytes(n)"); return lumyr_make_int(-1); }
    BytesObj* bo = (BytesObj*)buf.v.bytes_obj;
    if (!bo || !bo->data || bo->cap <= 0) { runtime_error("readInto(buf) 需要非空定长缓冲 bytes(n>0)"); return lumyr_make_int(-1); }
    int readCap = bo->cap;
    if (maxLen > 0 && maxLen < readCap) readCap = (int)maxLen;

    int64_t n;
    /* 内存文件：直接拷贝到复用缓冲 */
    if (o->content) {
        int64_t remain = (int64_t)o->contentLen - o->readPos;
        n = remain < (int64_t)readCap ? remain : (int64_t)readCap;
        if (n < 0) n = 0;
        if (n > 0) memcpy(bo->data, o->content + o->readPos, (size_t)n);
    } else {
        readIntoCtx c;
        c.path = o->path; c.offset = o->readPos; c.dst = bo->data; c.cap = readCap; c.got = -1;
        if (lm_co_await_blocking(read_into_blocking, &c, NULL) != 0) {
            read_into_blocking(&c);
        }
        if (c.got < 0) {
            char eb[512];
            snprintf(eb, sizeof(eb), "readInto() 无法读取文件: %s", o->path);
            runtime_error(eb);
            return lumyr_make_int(-1);
        }
        n = c.got;
    }
    bo->len = (int)n;   /* 末块可能不足 cap，按实际长度发送/落盘 */
    o->readPos += n;
    return lumyr_make_int((int)n);
}

Value lumyr_file_append_bytes(Value v, Value b) {
    if (v.type != VAL_FILE) { runtime_error("appendBytes() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("appendBytes() 文件对象无效"); return val_none(); }
    if (b.type != VAL_BYTES) { runtime_error("appendBytes() 参数必须是 bytes 对象"); return val_none(); }
    if (o->content) {
        runtime_error("appendBytes() 不支持内存文件，请用磁盘路径 file(path, \"a\")");
        return val_none();
    }
    BytesObj* bo = (BytesObj*)b.v.bytes_obj;
    chunkWriteCtx c;
    c.path = o->path;
    c.data = bo ? bo->data : NULL;
    c.len = bo ? (size_t)bo->len : 0;
    c.ok = -1;
    if (lm_co_await_blocking(chunk_append_blocking, &c, NULL) != 0) {
        chunk_append_blocking(&c);
    }
    if (c.ok != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "appendBytes() 追加写入失败: %s", o->path);
        runtime_error(buf);
        return val_none();
    }
    return val_none();
}

Value lumyr_file_copy_to(Value v, const char* dest) {
    if (v.type != VAL_FILE) { runtime_error("copyTo() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("copyTo() 文件对象无效"); return val_none(); }
    if (!dest) { runtime_error("copyTo() 目标路径为空"); return val_none(); }
    /* read+write 合成一个 blocking 池任务，协程只 yield 一次 */
    if (file_copy_route(o->path, dest) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "copyTo() 无法读取源文件或无法打开目标文件: %s -> %s",
                 o->path, dest);
        runtime_error(buf);
        return val_none();
    }
    return val_none();
}

Value lumyr_file_rename_to(Value v, const char* newPath) {
    if (v.type != VAL_FILE) { runtime_error("renameTo() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("renameTo() 文件对象无效"); return val_none(); }
    if (!newPath) { runtime_error("renameTo() 新路径为空"); return val_none(); }
    int errNo = 0;
    if (file_rename_route(o->path, newPath, &errNo) != 0) {
        char buf[512], why[160];
        file_errno_text(errNo, why, sizeof(why));
        snprintf(buf, sizeof(buf), "renameTo() 重命名失败: %s -> %s (%s)",
                 o->path, newPath, why);
        runtime_error(buf);
        return val_none();
    }
    /* 更新内部路径 */
    size_t plen = strlen(newPath);
    /* 释放旧路径（GC 管理，不手动 free） */
    o->path = (char*)gc_alloc(plen + 1, VAL_STRING);
    if (o->path) memcpy(o->path, newPath, plen + 1);
    return val_none();
}

Value lumyr_file_truncate(Value v, int64_t size) {
    if (v.type != VAL_FILE) { runtime_error("truncate() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("truncate() 文件对象无效"); return val_none(); }
    int errNo = 0;
    if (file_truncate_route(o->path, size, &errNo) != 0) {
        char buf[512], why[160];
        file_errno_text(errNo, why, sizeof(why));
        snprintf(buf, sizeof(buf), "truncate() 截断失败: %s (%s)", o->path, why);
        runtime_error(buf);
        return val_none();
    }
    return val_none();
}

/* ============================================================
 * folder 专用阻塞操作（纯实现 + 池任务，供公共 API 路由）
 * ============================================================ */

/* mkdir -p：递归创建目录。返回 0 成功；路径已存在（EEXIST）视为成功。 */
static int mkdir_p_impl(const char* path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

typedef struct {
    const char* path;
    int         rc;
    int         errNo;
} folderOpCtx;

static void* mkdir_p_blocking(void* arg) {
    folderOpCtx* c = (folderOpCtx*)arg;
    c->rc = mkdir_p_impl(c->path);
    if (c->rc != 0) c->errNo = errno;
    return c;
}

/* 整目录删除：不存在幂等成功。返回 0 成功 / -1 失败。 */
static int folder_remove_impl(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) return 0;
        return -1;
    }
    return remove_dir_impl(path);
}

static void* folder_remove_blocking(void* arg) {
    folderOpCtx* c = (folderOpCtx*)arg;
    c->rc = folder_remove_impl(c->path);
    if (c->rc != 0) c->errNo = errno;
    return c;
}

/* glob：直接子条目名与 pattern 匹配，结果写入 strList。 */
static void glob_impl(const char* path, const char* pattern, strList* out) {
    DIR* d = opendir(path);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (fnmatch(pattern, ent->d_name, 0) == 0) {
            strList_add(out, ent->d_name);
        }
    }
    closedir(d);
}

typedef struct {
    const char* path;
    const char* pattern;
    strList*    out;
} globCtx;

static void* glob_blocking(void* arg) {
    globCtx* c = (globCtx*)arg;
    glob_impl(c->path, c->pattern, c->out);
    return c;
}

/* 整目录移动：先 rename（同文件系统快），失败（跨文件系统）则复制+删除。
 * 返回 0 成功 / -1 失败。纯实现（池线程调用）。 */
static int folder_move_impl(const char* src, const char* dest) {
    if (rename(src, dest) == 0) return 0;
    if (copy_dir_impl(src, dest) != 0) return -1;
    return remove_dir_impl(src);
}

typedef struct {
    const char* src;
    const char* dest;
    int         rc;
    int         errNo;
} folderMoveCtx;

static void* folder_move_blocking(void* arg) {
    folderMoveCtx* c = (folderMoveCtx*)arg;
    c->rc = folder_move_impl(c->src, c->dest);
    if (c->rc != 0) c->errNo = errno;
    return c;
}

// ===== folder 公共 API =====

Value lumyr_folder_make(const char* path) {
    FolderObj* o = (FolderObj*)gc_alloc(sizeof(FolderObj), VAL_FOLDER);
    if (!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    size_t plen = path ? strlen(path) : 0;
    o->path = (char*)gc_alloc(plen + 1, VAL_STRING);
    if (o->path) {
        memcpy(o->path, path ? path : "", plen + 1);
    }
    o->stack_alloc = 0;
    Value r;
    r.type = VAL_FOLDER;
    r.str_inline = 0;
    r.v.folder_obj = o;
    return r;
}

Value lumyr_folder_field(Value v, const char* name) {
    if (!name) return lumyr_make_int(0);
    if (v.type != VAL_FOLDER) return lumyr_make_int(0);
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) return lumyr_make_int(0);

    if (strcmp(name, "path") == 0)    return lumyr_make_string(o->path);
    if (strcmp(name, "exists") == 0)  return lumyr_make_bool(path_exists(o->path) && path_is_dir(o->path));
    if (strcmp(name, "count") == 0)  return lumyr_make_int(dir_count(o->path));
    if (strcmp(name, "size") == 0) {
        /* 目录总大小（递归所有文件字节数） */
        return lumyr_make_int64(dir_total_size(o->path));
    }
    /* 无兜底：未知字段/方法名 → AttributeError（方法解析已先完成） */
    {
        char buf[256];
        snprintf(buf, sizeof buf,
                 "folder 没有字段或方法 \"%s\" / folder has no field or method \"%s\"",
                 name, name);
        runtime_error(buf);
    }
    return lumyr_make_int(0);   /* 不可达 */
}

char* lumyr_folder_to_str(Value v) {
    if (v.type != VAL_FOLDER) return strdup("");
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) return strdup("");
    char buf[512];
    snprintf(buf, sizeof(buf), "<folder %s>", o->path);
    return strdup(buf);
}

Value lumyr_folder_list(Value v) {
    if (v.type != VAL_FOLDER) { runtime_error("list() 仅适用于 folder 对象"); return val_array(0); }
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) { runtime_error("list() 目录对象无效"); return val_array(0); }
    return list_entries(o->path, 0);
}

Value lumyr_folder_files(Value v) {
    if (v.type != VAL_FOLDER) { runtime_error("files() 仅适用于 folder 对象"); return val_array(0); }
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) { runtime_error("files() 目录对象无效"); return val_array(0); }
    return list_entries(o->path, 1);
}

Value lumyr_folder_dirs(Value v) {
    if (v.type != VAL_FOLDER) { runtime_error("dirs() 仅适用于 folder 对象"); return val_array(0); }
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) { runtime_error("dirs() 目录对象无效"); return val_array(0); }
    return list_entries(o->path, 2);
}

Value lumyr_folder_create(Value v) {
    if (v.type != VAL_FOLDER) { runtime_error("create() 仅适用于 folder 对象"); return val_none(); }
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) { runtime_error("create() 目录对象无效"); return val_none(); }
    folderOpCtx c;
    c.path = o->path; c.rc = -1; c.errNo = 0;
    if (lm_co_await_blocking(mkdir_p_blocking, &c, NULL) == 0) {
        if (c.rc == 0) return val_none();
    } else if (mkdir_p_impl(o->path) == 0) {
        return val_none();
    }
    char buf[512], why[160];
    file_errno_text(c.errNo, why, sizeof(why));
    snprintf(buf, sizeof(buf), "create() 无法创建目录: %s (%s)", o->path, why);
    runtime_error(buf);
    return val_none();
}

Value lumyr_folder_remove(Value v) {
    if (v.type != VAL_FOLDER) { runtime_error("remove() 仅适用于 folder 对象"); return val_none(); }
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) { runtime_error("remove() 目录对象无效"); return val_none(); }
    folderOpCtx c;
    c.path = o->path; c.rc = -1; c.errNo = 0;
    int rc;
    if (lm_co_await_blocking(folder_remove_blocking, &c, NULL) == 0) {
        rc = c.rc;
    } else {
        rc = folder_remove_impl(o->path);
        if (rc != 0) c.errNo = errno;
    }
    if (rc != 0) {
        char buf[512], why[160];
        file_errno_text(c.errNo, why, sizeof(why));
        snprintf(buf, sizeof(buf), "remove() 无法删除目录: %s (%s)", o->path, why);
        runtime_error(buf);
        return val_none();
    }
    return val_none();
}

Value lumyr_folder_walk(Value v) {
    if (v.type != VAL_FOLDER) { runtime_error("walk() 仅适用于 folder 对象"); return val_array(0); }
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) { runtime_error("walk() 目录对象无效"); return val_array(0); }
    Value arr = val_array(0);
    walk_dir_append(o->path, arr);
    return arr;
}

Value lumyr_folder_copy_to(Value v, const char* dest) {
    if (v.type != VAL_FOLDER) { runtime_error("copyTo() 仅适用于 folder 对象"); return val_none(); }
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) { runtime_error("copyTo() 目录对象无效"); return val_none(); }
    if (!dest) { runtime_error("copyTo() 目标路径为空"); return val_none(); }
    if (copy_dir_recursive(o->path, dest) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "copyTo() 复制失败: %s -> %s", o->path, dest);
        runtime_error(buf);
        return val_none();
    }
    return val_none();
}

Value lumyr_folder_move_to(Value v, const char* dest) {
    if (v.type != VAL_FOLDER) { runtime_error("moveTo() 仅适用于 folder 对象"); return val_none(); }
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) { runtime_error("moveTo() 目录对象无效"); return val_none(); }
    if (!dest) { runtime_error("moveTo() 目标路径为空"); return val_none(); }
    /* rename/复制+删除 合为一个池任务，协程只 yield 一次 */
    folderMoveCtx c;
    c.src = o->path; c.dest = dest; c.rc = -1; c.errNo = 0;
    int rc;
    if (lm_co_await_blocking(folder_move_blocking, &c, NULL) == 0) {
        rc = c.rc;
    } else {
        rc = folder_move_impl(o->path, dest);
        if (rc != 0) c.errNo = errno;
    }
    if (rc != 0) {
        char buf[512], why[160];
        file_errno_text(c.errNo, why, sizeof(why));
        snprintf(buf, sizeof(buf), "moveTo() 移动失败: %s -> %s (%s)",
                 o->path, dest, why);
        runtime_error(buf);
        return val_none();
    }
    /* 更新内部路径 */
    size_t plen = strlen(dest);
    o->path = (char*)gc_alloc(plen + 1, VAL_STRING);
    if (o->path) memcpy(o->path, dest, plen + 1);
    return val_none();
}

Value lumyr_folder_rename_to(Value v, const char* newPath) {
    if (v.type != VAL_FOLDER) { runtime_error("renameTo() 仅适用于 folder 对象"); return val_none(); }
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) { runtime_error("renameTo() 目录对象无效"); return val_none(); }
    if (!newPath) { runtime_error("renameTo() 新路径为空"); return val_none(); }
    int errNo = 0;
    if (file_rename_route(o->path, newPath, &errNo) != 0) {
        char buf[512], why[160];
        file_errno_text(errNo, why, sizeof(why));
        snprintf(buf, sizeof(buf), "renameTo() 重命名失败: %s -> %s (%s)",
                 o->path, newPath, why);
        runtime_error(buf);
        return val_none();
    }
    /* 更新内部路径 */
    size_t plen = strlen(newPath);
    o->path = (char*)gc_alloc(plen + 1, VAL_STRING);
    if (o->path) memcpy(o->path, newPath, plen + 1);
    return val_none();
}

Value lumyr_folder_glob(Value v, const char* pattern) {
    if (v.type != VAL_FOLDER) { runtime_error("glob() 仅适用于 folder 对象"); return val_array(0); }
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) { runtime_error("glob() 目录对象无效"); return val_array(0); }
    if (!pattern) { runtime_error("glob() 模式为空"); return val_array(0); }
    strList out;
    strList_init(&out);
    globCtx c;
    c.path = o->path; c.pattern = pattern; c.out = &out;
    if (lm_co_await_blocking(glob_blocking, &c, NULL) != 0) {
        glob_impl(o->path, pattern, &out);
    }
    Value arr = val_array(out.len);
    for (int i = 0; i < out.len; i++) {
        arr.v.array->items[i] = lumyr_make_string(out.items[i]);
    }
    strList_free(&out);
    return arr;
}
