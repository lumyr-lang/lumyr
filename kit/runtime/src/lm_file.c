// lm_file.c —— 文件对象（VAL_FILE）与目录对象（VAL_FOLDER）
// 不持有 FILE* 句柄：每次方法调用 fopen/fclose，避免 GC 回收时的资源泄漏
#include "lm_file.h"
#include "lm_array.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>

// ===== 内部辅助 =====

// 标准化模式字符串：返回 fopen 兼容的模式（"rb"/"wb"/"ab"）
static const char* norm_mode(const char* mode) {
    if (!mode || !*mode) return "rb";
    if (mode[0] == 'w' || mode[0] == 'W') return "wb";
    if (mode[0] == 'a' || mode[0] == 'A') return "ab";
    return "rb";  // 默认读
}

// 文件是否存在
static int path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// 是否为目录
static int path_is_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

// 获取文件大小（字节）
static long file_size(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

// 读取整个文件到 malloc 缓冲（调用方 free）
static char* read_whole_file(const char* path, long* out_len) {
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

// 递归删除目录（rmdir 非递归只删空目录）
static int remove_dir_recursive(const char* path) {
    DIR* d = opendir(path);
    if (!d) return -1;
    struct dirent* ent;
    int rc = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (path_is_dir(child)) {
            if (remove_dir_recursive(child) != 0) { rc = -1; }
        } else {
            if (unlink(child) != 0) { rc = -1; }
        }
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}

// 递归复制目录到 dest
static int copy_dir_recursive(const char* src, const char* dest) {
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
            if (copy_dir_recursive(src_child, dst_child) != 0) rc = -1;
        } else {
            // 复制单个文件
            long sz = 0;
            char* content = read_whole_file(src_child, &sz);
            if (content) {
                FILE* f = fopen(dst_child, "wb");
                if (f) {
                    fwrite(content, 1, (size_t)sz, f);
                    fclose(f);
                } else rc = -1;
                free(content);
            } else rc = -1;
        }
    }
    closedir(d);
    return rc;
}

// 递归遍历目录，将所有文件路径追加到 arr
static void walk_dir_append(const char* path, Value arr) {
    DIR* d = opendir(path);
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (path_is_dir(child)) {
            walk_dir_append(child, arr);
        } else {
            Value s = lumyr_make_string(child);
            // arr 是 VAL_ARRAY，使用 lumyr_array_add 原地追加（接受 Value* 指针）
            lumyr_array_add(&arr, s);
        }
    }
    closedir(d);
}

// 列出目录条目，filter：0=全部，1=只文件，2=只目录
static Value list_entries(const char* path, int filter) {
    Value arr = val_array(0);
    DIR* d = opendir(path);
    if (!d) return arr;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        int is_d = path_is_dir(child);
        if (filter == 1 && is_d) continue;
        if (filter == 2 && !is_d) continue;
        Value s = lumyr_make_string(ent->d_name);
        lumyr_array_add(&arr, s);
    }
    closedir(d);
    return arr;
}

// 计算目录直接子条目数
static int dir_count(const char* path) {
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

Value lumyr_file_field(Value v, const char* name) {
    if (!name) return lumyr_make_int(0);
    if (v.type != VAL_FILE) return lumyr_make_int(0);
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) return lumyr_make_int(0);

    if (strcmp(name, "path") == 0)   return lumyr_make_string(o->path);
    if (strcmp(name, "mode") == 0)  return lumyr_make_string(o->mode ? o->mode : "r");
    if (strcmp(name, "exists") == 0) return lumyr_make_bool(path_exists(o->path));
    if (strcmp(name, "size") == 0)  return lumyr_make_int(file_size(o->path));
    if (strcmp(name, "isOpen") == 0) return lumyr_make_bool(0);  // 无持久句柄
    if (strcmp(name, "lines") == 0) {
        long sz = 0;
        char* content = read_whole_file(o->path, &sz);
        int n = content ? count_lines(content) : 0;
        free(content);
        return lumyr_make_int(n);
    }
    return lumyr_make_int(0);
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
    char* content = read_whole_file(o->path, &sz);
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
    char* content = read_whole_file(o->path, &sz);
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
    // writeAll 始终覆盖写
    FILE* f = fopen(o->path, "wb");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "writeAll() 无法打开文件（写入）: %s", o->path);
        runtime_error(buf);
        return val_none();
    }
    size_t len = content ? strlen(content) : 0;
    size_t wr = fwrite(content ? content : "", 1, len, f);
    fclose(f);
    if (wr != len) { runtime_error("writeAll() 写入不完整"); return val_none(); }
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
    // 写回
    FILE* f = fopen(o->path, "wb");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "writeLine() 无法打开文件（写回）: %s", o->path);
        runtime_error(buf);
        return val_none();
    }
    for (int i = 0; i < lines.v.array->len; i++) {
        const char* s = lumyr_str_cstr(&lines.v.array->items[i]);
        fputs(s ? s : "", f);
        fputc('\n', f);
    }
    fclose(f);
    return val_none();
}

Value lumyr_file_write_lines(Value v, Value arr) {
    if (v.type != VAL_FILE) { runtime_error("writeLines() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("writeLines() 文件对象无效"); return val_none(); }
    if (arr.type != VAL_ARRAY) { runtime_error("writeLines() 参数必须是字符串数组"); return val_none(); }
    FILE* f = fopen(o->path, "wb");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "writeLines() 无法打开文件（写入）: %s", o->path);
        runtime_error(buf);
        return val_none();
    }
    for (int i = 0; i < arr.v.array->len; i++) {
        const char* s = lumyr_str_cstr(&arr.v.array->items[i]);
        fputs(s ? s : "", f);
        fputc('\n', f);
    }
    fclose(f);
    return val_none();
}

Value lumyr_file_append(Value v, const char* content) {
    if (v.type != VAL_FILE) { runtime_error("append() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("append() 文件对象无效"); return val_none(); }
    FILE* f = fopen(o->path, "ab");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "append() 无法打开文件（追加）: %s", o->path);
        runtime_error(buf);
        return val_none();
    }
    size_t len = content ? strlen(content) : 0;
    size_t wr = fwrite(content ? content : "", 1, len, f);
    fclose(f);
    if (wr != len) { runtime_error("append() 写入不完整"); return val_none(); }
    return val_none();
}

Value lumyr_file_append_line(Value v, const char* content) {
    if (v.type != VAL_FILE) { runtime_error("appendLine() 仅适用于 file 对象"); return val_none(); }
    FileObj* o = (FileObj*)v.v.file_obj;
    if (!o || !o->path) { runtime_error("appendLine() 文件对象无效"); return val_none(); }
    FILE* f = fopen(o->path, "ab");
    if (!f) {
        char buf[512];
        snprintf(buf, sizeof(buf), "appendLine() 无法打开文件（追加）: %s", o->path);
        runtime_error(buf);
        return val_none();
    }
    size_t len = content ? strlen(content) : 0;
    fwrite(content ? content : "", 1, len, f);
    fputc('\n', f);
    fclose(f);
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
    if (unlink(o->path) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "delete() 无法删除文件: %s (%s)", o->path, strerror(errno));
        runtime_error(buf);
        return val_none();
    }
    return val_none();
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
    return lumyr_make_int(0);
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
    // 递归创建（类似 mkdir -p）
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", o->path);
    size_t len = strlen(tmp);
    if (len == 0) { runtime_error("create() 路径为空"); return val_none(); }
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        char buf[512];
        snprintf(buf, sizeof(buf), "create() 无法创建目录: %s (%s)", o->path, strerror(errno));
        runtime_error(buf);
        return val_none();
    }
    return val_none();
}

Value lumyr_folder_remove(Value v) {
    if (v.type != VAL_FOLDER) { runtime_error("remove() 仅适用于 folder 对象"); return val_none(); }
    FolderObj* o = (FolderObj*)v.v.folder_obj;
    if (!o || !o->path) { runtime_error("remove() 目录对象无效"); return val_none(); }
    /* 目录不存在时幂等返回（不报错） */
    if (!path_exists(o->path)) return val_none();
    if (remove_dir_recursive(o->path) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "remove() 无法删除目录: %s (%s)", o->path, strerror(errno));
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
    // 先尝试 rename（同文件系统快），失败则复制+删除
    if (rename(o->path, dest) == 0) return val_none();
    if (copy_dir_recursive(o->path, dest) != 0) {
        char buf[512];
        snprintf(buf, sizeof(buf), "moveTo() 移动失败: %s -> %s", o->path, dest);
        runtime_error(buf);
        return val_none();
    }
    remove_dir_recursive(o->path);
    return val_none();
}
