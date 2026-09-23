// lm_http.c —— HTTP 客户端（libcurl 实现；requests.get/post/put/delete/head/patch 的运行时支撑）
//
// 签名：requests.<method>(url [, config])
// config 键：
//   params         map/string/bytes/[[k,v],...] URL 查询参数（query string）
//   headers        map                          请求头
//   body           任意值                       string/bytes 原样发送；map/array/tuple/set/数字/bool 自动 JSON；
//                                              file 对象流式发送（磁盘 fread 或内存文件内容）
//   form           map                          application/x-www-form-urlencoded 表单
//   files          map                          multipart/form-data：字段名 → file/路径/bytes/描述map，可同名字段多文件
//   output         string/file                  响应体直接写入文件（不进内存，下载文件流）
//   responseType   "auto"/"text"/"json"/"bytes" 响应体解析方式（默认 auto：按 Content-Type）
//   timeout        数字                         超时秒数
//   allowRedirects bool                         是否跟随 3xx（默认 true）
// 返回 map：status(int) / headers(map) / body(自动类型) / bytes(原始字节) / json(解析值)
#include "lm_http.h"
#include "lm_map.h"
#include "lm_value.h"
#include "lm_json.h"
#include "lm_container.h"
#include "lm_formdata.h"
#include "gc_runtime.h"
#include <curl/curl.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <libgen.h>

// ---------------- 动态字节缓冲 ----------------
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} Buf;

// 返回 0=分配失败（b 已复位）
static int buf_append(Buf* b, const char* s, size_t n)
{
    if(b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while(nc < b->len + n + 1) nc *= 2;
        char* nd = (char*)realloc(b->data, nc);
        if(!nd) { free(b->data); b->data = NULL; b->len = 0; b->cap = 0; return 0; }
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

// ---------------- 响应收集（内存 / 文件） ----------------
typedef struct {
    Buf* mem;     // 无 output：收集到内存
    FILE* fp;     // 有 output：直接写文件
} OutSink;

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* ud)
{
    OutSink* s = (OutSink*)ud;
    size_t total = size * nmemb;
    if(s->fp) return fwrite(ptr, size, nmemb, s->fp);
    if(!buf_append(s->mem, ptr, total)) return 0;
    return total;
}

// 响应头收集回调：解析 "Name: value\r\n" 进响应头 map（重复键后者覆盖）
static size_t hdr_cb(char* ptr, size_t size, size_t nmemb, void* ud)
{
    Value* hm = (Value*)ud;
    size_t n = size * nmemb;
    char* line = (char*)malloc(n + 1);
    if(!line) return 0;
    memcpy(line, ptr, n);
    line[n] = '\0';
    char* colon = strchr(line, ':');
    if(colon) {
        *colon = '\0';
        char* k = line;
        char* v = colon + 1;
        while(*k == ' ' || *k == '\t') k++;
        char* ke = k + strlen(k);
        while(ke > k && (ke[-1] == ' ' || ke[-1] == '\t' || ke[-1] == '\r' || ke[-1] == '\n')) *--ke = '\0';
        while(*v == ' ' || *v == '\t') v++;
        char* ve = v + strlen(v);
        while(ve > v && (ve[-1] == ' ' || ve[-1] == '\t' || ve[-1] == '\r' || ve[-1] == '\n')) *--ve = '\0';
        if(k[0] && v[0])
            lumyr_map_set(hm, lumyr_make_string(k), lumyr_make_string(v));
    }
    free(line);
    return n;
}

// ---------------- 上传：磁盘文件流读取 ----------------
static size_t upload_cb(char* buf, size_t size, size_t nmemb, void* ud)
{
    return fread(buf, size, nmemb, (FILE*)ud);
}

// ---------------- 通用辅助 ----------------

// map 键大小写不敏感查找（HTTP 头字段大小写不统一）
static Value map_get_ci(Value m, const char* key)
{
    MapIter it; map_iter_init(&it, m.v.map);
    Value k, v;
    while(map_iter_next(&it, &k, &v)) {
        char* ks = value_to_str(k);
        int eq = ks && strcasecmp(ks, key) == 0;
        free(ks);
        if(eq) return v;
    }
    return val_none();
}

// map 是否含某键（大小写不敏感）
static int map_has_ci(Value m, const char* key)
{
    MapIter it; map_iter_init(&it, m.v.map);
    Value k, v;
    while(map_iter_next(&it, &k, &v)) {
        char* ks = value_to_str(k);
        int eq = ks && strcasecmp(ks, key) == 0;
        free(ks);
        if(eq) return 1;
    }
    return 0;
}

// ASCII 子串大小写不敏感
static int ascii_contains_ci(const char* hay, const char* needle)
{
    if(!hay || !needle) return 0;
    size_t nl = strlen(needle);
    for(const char* p = hay; *p; p++) {
        size_t i = 0;
        while(i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if(i == nl) return 1;
    }
    return 0;
}

// 构造 VAL_BYTES（与 lm_container.c bytes_alloc 同模式；裸字节不经过字符串编码）
static Value make_bytes_value(const uint8_t* data, int len)
{
    BytesObj* o = (BytesObj*)gc_alloc(sizeof(BytesObj), VAL_BYTES);
    Value z; memset(&z, 0, sizeof(z));
    if(!o) return z;
    o->len = len;
    o->stack_alloc = 0;
    if(len > 0) {
        o->data = (uint8_t*)gc_alloc(sizeof(uint8_t) * len, VAL_BYTES);
        if(o->data) memcpy(o->data, data, len);
        else o->len = 0;
    } else {
        o->data = NULL;
    }
    z.type = VAL_BYTES;
    z.str_inline = 0;
    z.v.bytes_obj = o;
    return z;
}

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static void curl_global_init_once(void) { curl_global_init(CURL_GLOBAL_DEFAULT); }

/* 构造 VAL_ERROR（与 vm_except.c ensure_error 约定一致：strdup 字符串）。
 * 不直接 abort：VM 异常是协作式展开（无 setjmp/longjmp），错误由调用方 throw */
static Value http_error(const char* fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    Value e;
    memset(&e, 0, sizeof(e));
    e.type = VAL_ERROR;
    e.v.err.type = strdup("RuntimeError");
    e.v.err.message = strdup(msg);
    e.v.err.stack = NULL;
    return e;
}

// URL escape 后追加 "k=v"（分隔符由调用方处理）；失败返回 0
static int append_escaped_kv(Buf* out, CURL* h, Value k, Value v)
{
    char* kraw = value_to_str(k);
    char* ek = curl_easy_escape(h, kraw, 0);
    free(kraw);
    char* vraw = value_to_str(v);
    char* ev = curl_easy_escape(h, vraw, 0);
    free(vraw);
    int ok = ek && ev && buf_append(out, ek, strlen(ek))
            && buf_append(out, "=", 1)
            && buf_append(out, ev, strlen(ev));
    if(ek) curl_free(ek);
    if(ev) curl_free(ev);
    return ok;
}

// map → urlencoded 文本（query/form 通用）；失败返回 0
static int build_urlencoded(CURL* h, Value m, Buf* out)
{
    MapIter it; map_iter_init(&it, m.v.map);
    Value k, v; int first = 1;
    while(map_iter_next(&it, &k, &v)) {
        if(!first) { if(!buf_append(out, "&", 1)) return 0; }
        first = 0;
        if(!append_escaped_kv(out, h, k, v)) return 0;
    }
    return 1;
}

// ================= multipart/form-data =================

// 单个文件描述 → 已命名 part；失败 *err 赋值返回 0
static int mime_single(curl_mime* mime, const char* field, Value d, Value* err)
{
    curl_mimepart* p = curl_mime_addpart(mime);
    if(!p) { *err = http_error("requests: 内存不足"); return 0; }
    curl_mime_name(p, field);

    switch(d.type) {
    case VAL_FILE: {
        FileObj* fo = (FileObj*)d.v.file_obj;
        if(fo->content) {
            // 内存文件：内容直接作为 part 数据，文件名取 basename
            curl_mime_data(p, (const char*)fo->content, fo->contentLen);
            char* dup = strdup(fo->path);
            const char* bn = basename(dup);
            curl_mime_filename(p, bn);
            free(dup);
            curl_mime_type(p, "application/octet-stream");
        } else {
            CURLcode rc = curl_mime_filedata(p, fo->path);   // curl 内部流式 fread
            if(rc != CURLE_OK) { *err = http_error("requests: 读取文件失败 %s: %s", fo->path, curl_easy_strerror(rc)); return 0; }
        }
        break;
    }
    case VAL_STRING: {
        // 字符串视为本地文件路径
        const char* path = lumyr_str_cstr(&d);
        CURLcode rc = curl_mime_filedata(p, path);
        if(rc != CURLE_OK) { *err = http_error("requests: 读取文件失败 %s: %s", path, curl_easy_strerror(rc)); return 0; }
        break;
    }
    case VAL_BYTES: {
        BytesObj* bo = (BytesObj*)d.v.bytes_obj;
        curl_mime_data(p, (const char*)bo->data, bo->len);
        curl_mime_filename(p, field);
        curl_mime_type(p, "application/octet-stream");
        break;
    }
    case VAL_MAP: {
        Value path = lumyr_map_get(d, lumyr_make_string("path"));
        Value bv = lumyr_map_get(d, lumyr_make_string("bytes"));
        if(path.type == VAL_STRING) {
            CURLcode rc = curl_mime_filedata(p, lumyr_str_cstr(&path));
            if(rc != CURLE_OK) { *err = http_error("requests: 读取文件失败 %s: %s", lumyr_str_cstr(&path), curl_easy_strerror(rc)); return 0; }
        } else if(bv.type == VAL_BYTES) {
            BytesObj* bo = (BytesObj*)bv.v.bytes_obj;
            curl_mime_data(p, (const char*)bo->data, bo->len);
        } else {
            *err = http_error("requests: files 描述 map 必须包含 path 或 bytes");
            return 0;
        }
        Value fn = lumyr_map_get(d, lumyr_make_string("filename"));
        if(fn.type == VAL_STRING) curl_mime_filename(p, lumyr_str_cstr(&fn));
        Value ct = lumyr_map_get(d, lumyr_make_string("contentType"));
        if(ct.type == VAL_STRING) curl_mime_type(p, lumyr_str_cstr(&ct));
        break;
    }
    default:
        *err = http_error("requests: files 的值必须是 file 对象/路径字符串/bytes/描述map");
        return 0;
    }
    return 1;
}

// 字段 → 描述；数组表示同名字段多文件
static int mime_add_field(curl_mime* mime, const char* field, Value d, Value* err)
{
    if(d.type == VAL_ARRAY) {
        for(int i = 0; i < d.v.array->len; i++)
            if(!mime_single(mime, field, d.v.array->items[i], err)) return 0;
        return 1;
    }
    return mime_single(mime, field, d, err);
}

// 构建 multipart：files 全部加；form 字段作为普通文本 part
static Value build_mime(CURL* h, Value files, Value form, curl_mime** out)
{
    Value err; memset(&err, 0, sizeof(err));
    curl_mime* mime = curl_mime_init(h);
    if(!mime) return http_error("requests: 内存不足");

    if(form.type == VAL_MAP) {
        MapIter it; map_iter_init(&it, form.v.map);
        Value k, v;
        while(map_iter_next(&it, &k, &v)) {
            char* kr = value_to_str(k);
            char* vr = value_to_str(v);
            curl_mimepart* p = curl_mime_addpart(mime);
            curl_mime_name(p, kr);
            curl_mime_data(p, vr ? vr : "", CURL_ZERO_TERMINATED);
            free(kr); free(vr);
        }
    }

    MapIter it; map_iter_init(&it, files.v.map);
    Value k, v;
    while(map_iter_next(&it, &k, &v)) {
        char* kr = value_to_str(k);
        int ok = mime_add_field(mime, kr, v, &err);
        free(kr);
        if(!ok) { curl_mime_free(mime); return err; }
    }

    *out = mime;
    err.type = VAL_NONE;
    return err;
}

// ---------- formdata → multipart ----------

// formdata 单个值 → part；数组展开为同名字段（多文件/多值）
static int mime_formdata_value(curl_mime* mime, const char* field, Value d, Value* err)
{
    if(d.type == VAL_ARRAY) {
        for(int i = 0; i < d.v.array->len; i++)
            if(!mime_formdata_value(mime, field, d.v.array->items[i], err)) return 0;
        return 1;
    }
    if(d.type == VAL_FILE || d.type == VAL_BYTES)
        return mime_single(mime, field, d, err);
    if(d.type == VAL_MAP) {
        // 含 path/bytes 的描述 map → 文件 part；普通 map → JSON 文本 part
        Value path = lumyr_map_get(d, lumyr_make_string("path"));
        Value bv = lumyr_map_get(d, lumyr_make_string("bytes"));
        if(path.type == VAL_STRING || bv.type == VAL_BYTES)
            return mime_single(mime, field, d, err);
        char* js = lumyr_json_stringify(d);
        if(!js) { *err = http_error("requests: 内存不足"); return 0; }
        curl_mimepart* p = curl_mime_addpart(mime);
        curl_mime_name(p, field);
        curl_mime_data(p, js, CURL_ZERO_TERMINATED);
        free(js);
        return 1;
    }
    // 其余类型（string/int/double/bool/char/高精度/date 族…）：文本 part
    curl_mimepart* p = curl_mime_addpart(mime);
    if(!p) { *err = http_error("requests: 内存不足"); return 0; }
    curl_mime_name(p, field);
    char* vr = value_to_str(d);
    curl_mime_data(p, vr ? vr : "", CURL_ZERO_TERMINATED);
    free(vr);
    return 1;
}

// formdata 对象 → 完整 multipart
static Value build_mime_formdata(CURL* h, Value fd, curl_mime** out)
{
    curl_mime* mime = curl_mime_init(h);
    if(!mime) return http_error("requests: 内存不足");
    int n = lumyr_formdata_len(fd);
    for(int i = 0; i < n; i++) {
        Value err;
        memset(&err, 0, sizeof(err));
        if(!mime_formdata_value(mime, lumyr_formdata_name(fd, i),
                                lumyr_formdata_get(fd, i), &err)) {
            curl_mime_free(mime);
            return err;
        }
    }
    *out = mime;
    Value ok;
    ok.type = VAL_NONE;
    return ok;
}

// body 为容器/数值/bool/none/char/高精度/date 族等可 JSON 序列化类型时按 JSON 发送
static int body_is_json_type(ValueType t)
{
    switch(t) {
    case VAL_MAP: case VAL_ARRAY: case VAL_TUPLE: case VAL_SET:
    case VAL_BOOL: case VAL_NONE: case VAL_CHAR:
    case VAL_INT: case VAL_INT8: case VAL_INT16: case VAL_INT32: case VAL_INT64:
    case VAL_LONG: case VAL_LONG_LONG: case VAL_SHORT:
    case VAL_BYTE: case VAL_UINT8: case VAL_UINT16: case VAL_UINT32:
    case VAL_UINT64: case VAL_UINT: case VAL_UCHAR: case VAL_ULONG: case VAL_USHORT:
    case VAL_SIZE_T: case VAL_SSIZE_T:
    case VAL_DOUBLE: case VAL_FLOAT: case VAL_LONG_DOUBLE:
    case VAL_BIGINT: case VAL_DECIMAL: case VAL_BITDECIMAL:
    case VAL_DATE: case VAL_DATETIME: case VAL_TIME: case VAL_TIMEDELTA:
    case VAL_COMPLEX: case VAL_CALENDAR: case VAL_TYPED_ARRAY:
        return 1;
    default:
        return 0;
    }
}

// ================= 主入口 =================
Value lumyr_http_request(const char* method, Value url, Value config)
{
    pthread_once(&g_curl_once, curl_global_init_once);

    if(url.type != VAL_STRING)
        return http_error("requests: url 必须是字符串");
    if(config.type != VAL_NONE && config.type != VAL_MAP)
        return http_error("requests: 配置 config 必须是字典");

    // ---------- 1. 解析 config ----------
    Value params = val_none(), headers = val_none();
    Value body = val_none(), form = val_none(), files = val_none();
    Value output = val_none(), respType = val_none();
    int hasParams = 0, hasBody = 0, hasForm = 0, hasFiles = 0, hasOutput = 0;
    long timeoutS = 30;
    int follow = 1;

    if(config.type == VAL_MAP) {
        hasParams = lumyr_map_has(config, lumyr_make_string("params"));
        if(hasParams) {
            params = lumyr_map_get(config, lumyr_make_string("params"));
            int pt = params.type;
            if(pt != VAL_STRING && pt != VAL_BYTES && pt != VAL_MAP && pt != VAL_ARRAY)
                return http_error("requests: config.params 必须是字符串/bytes/字典/pair 数组");
        }
        if(lumyr_map_has(config, lumyr_make_string("headers"))) {
            headers = lumyr_map_get(config, lumyr_make_string("headers"));
            if(headers.type != VAL_MAP) return http_error("requests: config.headers 必须是字典");
        }
        hasBody = lumyr_map_has(config, lumyr_make_string("body"));
        if(hasBody) body = lumyr_map_get(config, lumyr_make_string("body"));
        hasForm = lumyr_map_has(config, lumyr_make_string("form"));
        if(hasForm) {
            form = lumyr_map_get(config, lumyr_make_string("form"));
            if(form.type != VAL_MAP) return http_error("requests: config.form 必须是字典");
        }
        hasFiles = lumyr_map_has(config, lumyr_make_string("files"));
        if(hasFiles) {
            files = lumyr_map_get(config, lumyr_make_string("files"));
            if(files.type != VAL_MAP) return http_error("requests: config.files 必须是字典");
        }
        hasOutput = lumyr_map_has(config, lumyr_make_string("output"));
        if(hasOutput) {
            output = lumyr_map_get(config, lumyr_make_string("output"));
            if(output.type != VAL_STRING && output.type != VAL_FILE)
                return http_error("requests: config.output 必须是路径字符串或 file 对象");
        }
        if(lumyr_map_has(config, lumyr_make_string("responseType"))) {
            respType = lumyr_map_get(config, lumyr_make_string("responseType"));
            if(respType.type != VAL_STRING) return http_error("requests: config.responseType 必须是字符串");
        }
        if(lumyr_map_has(config, lumyr_make_string("timeout"))) {
            Value tv = lumyr_map_get(config, lumyr_make_string("timeout"));
            if(body_is_json_type(tv.type) || tv.type == VAL_CHAR) timeoutS = (long)lumyr_extract_ll(tv);
        }
        if(lumyr_map_has(config, lumyr_make_string("allowRedirects")))
            follow = lumyr_to_bool(lumyr_map_get(config, lumyr_make_string("allowRedirects")));
    }

    if(hasBody && (hasForm || hasFiles)) return http_error("requests: body 不能与 form/files 同时使用");

    const char* rtype = respType.type == VAL_STRING ? lumyr_str_cstr(&respType) : "auto";
    if(strcmp(rtype, "auto") && strcmp(rtype, "text") && strcmp(rtype, "json") && strcmp(rtype, "bytes"))
        return http_error("requests: responseType 必须是 auto/text/json/bytes");

    CURL* h = curl_easy_init();
    if(!h) return http_error("requests: curl 初始化失败");
    struct curl_slist* reqHdrs = NULL;
    curl_mime* mime = NULL;
    FILE* upFile = NULL;   // body=磁盘 file 时的上传句柄
    FILE* outFile = NULL;  // output 下载句柄
    Buf fullUrl = {0};
    Value rv; memset(&rv, 0, sizeof(rv));

    // ---------- 2. URL + query 参数 ----------
    const char* urlstr = lumyr_str_cstr(&url);
    if(!buf_append(&fullUrl, urlstr, strlen(urlstr))) goto oom;

    Buf qs = {0};
    int hasQs = 0;
    if(params.type == VAL_MAP) {
        if(!build_urlencoded(h, params, &qs)) goto oom;
        hasQs = qs.len > 0;
    } else if(params.type == VAL_ARRAY) {
        // [[k,v], ...] pair 列表（元素也可是 tuple）
        ValueArray* pa = params.v.array;
        int first = 1;
        for(int i = 0; pa && i < pa->len; i++) {
            Value pair = pa->items[i];
            Value k, v;
            if(pair.type == VAL_ARRAY && pair.v.array && pair.v.array->len == 2) {
                k = pair.v.array->items[0]; v = pair.v.array->items[1];
            } else if(pair.type == VAL_TUPLE && lumyr_tuple_len(pair) == 2) {
                k = lumyr_tuple_get(pair, 0); v = lumyr_tuple_get(pair, 1);
            } else {
                rv = http_error("requests: params 数组元素必须是 [key, value]"); goto fail;
            }
            if(!first) { if(!buf_append(&qs, "&", 1)) goto oom; }
            first = 0;
            if(!append_escaped_kv(&qs, h, k, v)) goto oom;
        }
        hasQs = qs.len > 0;
    } else if(params.type == VAL_STRING || params.type == VAL_BYTES) {
        const char* ps = params.type == VAL_STRING ? lumyr_str_cstr(&params)
                                                   : (const char*)((BytesObj*)params.v.bytes_obj)->data;
        if(ps && ps[0]) { if(!buf_append(&qs, ps, strlen(ps))) goto oom; hasQs = 1; }
    }
    if(hasQs) {
        if(strchr(fullUrl.data, '?') == NULL) { if(!buf_append(&fullUrl, "?", 1)) goto oom; }
        else { if(!buf_append(&fullUrl, "&", 1)) goto oom; }
        if(!buf_append(&fullUrl, qs.data, qs.len)) goto oom;
    }
    free(qs.data); qs.data = NULL;

    // ---------- 3. 请求头 ----------
    if(headers.type == VAL_MAP) {
        MapIter hit; map_iter_init(&hit, headers.v.map);
        Value hk, hv2;
        while(map_iter_next(&hit, &hk, &hv2)) {
            char* kraw = value_to_str(hk);
            char* vraw = value_to_str(hv2);
            size_t kl = strlen(kraw), vl = strlen(vraw);
            char* entry = (char*)malloc(kl + vl + 3);
            if(!entry) { free(kraw); free(vraw); goto oom; }
            memcpy(entry, kraw, kl);
            entry[kl] = ':'; entry[kl + 1] = ' ';
            memcpy(entry + kl + 2, vraw, vl + 1);
            reqHdrs = curl_slist_append(reqHdrs, entry);
            free(entry); free(kraw); free(vraw);
        }
    }

    // 用户是否显式指定 Content-Type（大小写不敏感）——决定是否自动补
    int userCT = headers.type == VAL_MAP && map_has_ci(headers, "content-type");

    // ---------- 4. 请求体编码 ----------
    const char* autoCT = NULL;   // 需要自动补的 Content-Type
    if(hasFiles) {
        Value merr = build_mime(h, files, form, &mime);
        if(merr.type == VAL_ERROR) { rv = merr; goto fail; }
        // multipart 的 Content-Type（含 boundary）由 curl 随 mime 自动生成，切勿手设
    } else if(hasForm) {
        Buf fb = {0};
        if(!build_urlencoded(h, form, &fb)) { free(fb.data); goto oom; }
        curl_easy_setopt(h, CURLOPT_COPYPOSTFIELDS, fb.data ? fb.data : "");
        free(fb.data);
        autoCT = "application/x-www-form-urlencoded";
    } else if(hasBody) {
        if(body.type == VAL_STRING) {
            curl_easy_setopt(h, CURLOPT_COPYPOSTFIELDS, lumyr_str_cstr(&body));
        } else if(body.type == VAL_BYTES) {
            BytesObj* bo = (BytesObj*)body.v.bytes_obj;
            curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)bo->len);
            curl_easy_setopt(h, CURLOPT_COPYPOSTFIELDS, bo->data);
            autoCT = "application/octet-stream";
        } else if(body.type == VAL_FILE) {
            FileObj* fo = (FileObj*)body.v.file_obj;
            if(fo->content) {
                // 内存文件：内容直接发送（curl 拷贝），长度精确
                curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)fo->contentLen);
                curl_easy_setopt(h, CURLOPT_COPYPOSTFIELDS, (const char*)fo->content);
                autoCT = "application/octet-stream";
            } else {
                upFile = fopen(fo->path, "rb");
                if(!upFile) { rv = http_error("requests: 无法打开上传文件 %s", fo->path); goto fail; }
                fseek(upFile, 0, SEEK_END);
                long fsize = ftell(upFile);
                rewind(upFile);
                curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
                curl_easy_setopt(h, CURLOPT_READFUNCTION, upload_cb);
                curl_easy_setopt(h, CURLOPT_READDATA, upFile);
                curl_easy_setopt(h, CURLOPT_INFILESIZE, fsize);
                autoCT = "application/octet-stream";
            }
        } else if(body.type == VAL_FORMDATA) {
            Value merr = build_mime_formdata(h, body, &mime);
            if(merr.type == VAL_ERROR) { rv = merr; goto fail; }
            // multipart 的 Content-Type（含 boundary）由 curl 随 mime 自动生成
        } else if(body_is_json_type(body.type)) {
            char* js = lumyr_json_stringify(body);
            if(!js) goto oom;
            curl_easy_setopt(h, CURLOPT_COPYPOSTFIELDS, js);
            free(js);
            autoCT = "application/json";
        } else {
            rv = http_error("requests: 不支持的 body 类型"); goto fail;
        }
    }
    if(autoCT && !userCT) {
        char entry[300];
        snprintf(entry, sizeof(entry), "Content-Type: %s", autoCT);
        reqHdrs = curl_slist_append(reqHdrs, entry);
    }

    // ---------- 5. 下载输出目标 ----------
    Buf respBody = {0};
    OutSink sink;
    sink.mem = &respBody; sink.fp = NULL;
    if(hasOutput) {
        const char* path = output.type == VAL_STRING ? lumyr_str_cstr(&output)
                                                     : ((FileObj*)output.v.file_obj)->path;
        outFile = fopen(path, "wb");
        if(!outFile) { rv = http_error("requests: 无法写入输出文件 %s", path); goto fail; }
        sink.fp = outFile;
        sink.mem = NULL;
    }

    // ---------- 6. 执行 ----------
    Value respHeaders = val_map();
    curl_easy_setopt(h, CURLOPT_URL, fullUrl.data);
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method);
    if(mime) curl_easy_setopt(h, CURLOPT_MIMEPOST, mime);
    if(reqHdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, reqHdrs);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeoutS);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, follow ? 1L : 0L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, hdr_cb);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &respHeaders);
    if(strcmp(method, "HEAD") == 0) curl_easy_setopt(h, CURLOPT_NOBODY, 1L);

    CURLcode rc = curl_easy_perform(h);
    long code = 0;
    if(rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);

    if(outFile) { fclose(outFile); outFile = NULL; }
    if(rc != CURLE_OK) {
        rv = http_error("requests.%s: %s", method, curl_easy_strerror(rc));
        goto fail;
    }

    // ---------- 7. 组装响应 ----------
    Value rawBytes = make_bytes_value((const uint8_t*)respBody.data, (int)respBody.len);

    Value jsonVal = val_none();
    if(!hasOutput && respBody.data)
        lumyr_json_try_parse(respBody.data, &jsonVal);

    Value bodyVal;
    if(hasOutput) {
        bodyVal = lumyr_make_string("");
    } else if(strcmp(rtype, "bytes") == 0) {
        bodyVal = rawBytes;
    } else if(strcmp(rtype, "text") == 0) {
        bodyVal = lumyr_make_string(respBody.data ? respBody.data : "");
    } else if(strcmp(rtype, "json") == 0) {
        if(jsonVal.type == VAL_NONE && (respBody.len == 0 || !ascii_contains_ci(respBody.data, "null"))) {
            rv = http_error("requests: 响应不是合法 JSON"); goto fail_resp;
        }
        bodyVal = jsonVal;
    } else { /* auto：Content-Type 含 json 且成功解析则给解析值，否则文本 */
        Value ctVal = map_get_ci(respHeaders, "content-type");
        char* ctStr = ctVal.type == VAL_NONE ? NULL : value_to_str(ctVal);
        if(ctStr && ascii_contains_ci(ctStr, "json") && jsonVal.type != VAL_NONE)
            bodyVal = jsonVal;
        else
            bodyVal = lumyr_make_string(respBody.data ? respBody.data : "");
        free(ctStr);
    }

    Value r = val_map();
    lumyr_map_set(&r, lumyr_make_string("status"), lumyr_make_int(code));
    lumyr_map_set(&r, lumyr_make_string("headers"), respHeaders);
    lumyr_map_set(&r, lumyr_make_string("body"), bodyVal);
    lumyr_map_set(&r, lumyr_make_string("bytes"), hasOutput ? make_bytes_value(NULL, 0) : rawBytes);
    lumyr_map_set(&r, lumyr_make_string("json"), jsonVal);
    free(respBody.data);
    rv = r;
    goto cleanup;

fail_resp:
    free(respBody.data);
    goto cleanup;
fail:
    goto cleanup;
oom:
    rv = http_error("requests: 内存不足");
cleanup:
    if(outFile) fclose(outFile);
    if(upFile) fclose(upFile);
    if(mime) curl_mime_free(mime);
    if(reqHdrs) curl_slist_free_all(reqHdrs);
    curl_easy_cleanup(h);
    free(fullUrl.data);
    free(qs.data);
    return rv;
}
