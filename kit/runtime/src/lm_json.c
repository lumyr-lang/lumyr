// lm_json.c —— json(s) / stringify(v) 内置函数
// 递归下降 JSON 解析器 + 值序列化器
// 双通道共享（VM 与 C 编译通道都调用本模块）
#include "lm_value.h"
#include "lm_charset.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ========== 解析 ========== */

typedef struct {
    const char* p;
    const char* end;
} JP;

static void jp_ws(JP* j) { while(j->p < j->end && isspace((unsigned char)*j->p)) j->p++; }

static Value jp_parse_value(JP* j);

// UTF-8 编码一个码点（含代理对合并后的完整码点）
static void utf8_enc(unsigned cp, char* out, int* n)
{
    if(cp < 0x80)      { out[0] = (char)cp; *n = 1; }
    else if(cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        *n = 2;
    } else if(cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        *n = 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        *n = 4;
    }
}

static int hex4(const char* s)
{
    int v = 0;
    for(int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if(c >= '0' && c <= '9')      v |= c - '0';
        else if(c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if(c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

// 解析 JSON 字符串字面量（已吃掉开引号，直到闭合引号）
static Value jp_parse_string(JP* j)
{
    j->p++; /* 跳过 " */
    size_t cap = 16, len = 0;
    char* buf = (char*)malloc(cap);
    while(j->p < j->end) {
        unsigned char c = (unsigned char)*j->p;
        if(c == '"') { j->p++; break; }
        if(c == '\\') {
            j->p++;
            if(j->p >= j->end) break;
            char e = *j->p;
            switch(e) {
                case '"':  buf[len++] = '"';  j->p++; break;
                case '\\': buf[len++] = '\\'; j->p++; break;
                case '/':  buf[len++] = '/';  j->p++; break;
                case 'b':  buf[len++] = '\b'; j->p++; break;
                case 'f':  buf[len++] = '\f'; j->p++; break;
                case 'n':  buf[len++] = '\n'; j->p++; break;
                case 'r':  buf[len++] = '\r'; j->p++; break;
                case 't':  buf[len++] = '\t'; j->p++; break;
                case 'u': {
                    if(j->p + 4 < j->end) {
                        int cp = hex4(j->p + 1);
                        if(cp >= 0) {
                            // 高代理位，尝试合并低代理位 \uD800-\uDBFF + \uDC00-\uDFFF
                            if(cp >= 0xD800 && cp <= 0xDBFF && j->p + 10 < j->end &&
                               j->p[5] == '\\' && j->p[6] == 'u') {
                                int lo = hex4(j->p + 7);
                                if(lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                    j->p += 6;
                                } else cp = 0xFFFD;
                            } else if(cp >= 0xDC00 && cp <= 0xDFFF) {
                                cp = 0xFFFD; /* 孤立低代理 */
                            }
                            char ub[4]; int un;
                            utf8_enc((unsigned)cp, ub, &un);
                            for(int k = 0; k < un; k++) buf[len++] = ub[k];
                            j->p += 5;
                            break;
                        }
                    }
                    buf[len++] = 'u';
                    j->p++;
                    break;
                }
                default: buf[len++] = e; j->p++; break;
            }
        } else {
            buf[len++] = (char)c;
            j->p++;
        }
        if(len + 8 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
    }
    buf[len] = '\0';
    Value v = lumyr_make_string(buf);  // 转为 gc_alloc 字符串
    free(buf);                          // 释放普通 malloc 缓冲区
    return v;
}

// 解析数字：纯整数（无小数点/指数）→ VAL_INT，否则 VAL_DOUBLE
static Value jp_parse_number(JP* j)
{
    const char* start = j->p;
    int is_int = 1;
    if(j->p < j->end && *j->p == '-') j->p++;
    while(j->p < j->end && isdigit((unsigned char)*j->p)) j->p++;
    if(j->p < j->end && *j->p == '.') { is_int = 0; j->p++; while(j->p < j->end && isdigit((unsigned char)*j->p)) j->p++; }
    if(j->p < j->end && (*j->p == 'e' || *j->p == 'E')) { is_int = 0; j->p++; if(j->p < j->end && (*j->p == '+' || *j->p == '-')) j->p++; while(j->p < j->end && isdigit((unsigned char)*j->p)) j->p++; }
    char* tmp = (char*)malloc((size_t)(j->p - start) + 1);
    memcpy(tmp, start, (size_t)(j->p - start));
    tmp[j->p - start] = '\0';
    Value v;
    if(is_int) {
        char* endp = NULL;
        long long ll = strtoll(tmp, &endp, 10);
        if(*endp == '\0' && endp != tmp) {
            v = lumyr_make_int(ll);
        } else {
            v = lumyr_make_double(strtod(tmp, NULL));
        }
    } else {
        v = lumyr_make_double(strtod(tmp, NULL));
    }
    free(tmp);
    return v;
}

static Value jp_parse_value(JP* j)
{
    jp_ws(j);
    if(j->p >= j->end) { runtime_error("json parse error: unexpected end"); return val_none(); }
    char c = *j->p;
    if(c == '{') {
        j->p++;
        Value m = val_map();
        jp_ws(j);
        if(j->p < j->end && *j->p == '}') { j->p++; return m; }
        for(;;) {
            jp_ws(j);
            if(j->p >= j->end || *j->p != '"') { runtime_error("json parse error: expect string key"); return m; }
            Value k = jp_parse_string(j);
            jp_ws(j);
            if(j->p >= j->end || *j->p != ':') { runtime_error("json parse error: expect ':'"); return m; }
            j->p++;
            Value val = jp_parse_value(j);
            lumyr_map_set(&m, k, val);
            jp_ws(j);
            if(j->p < j->end && *j->p == ',') { j->p++; continue; }
            if(j->p < j->end && *j->p == '}') { j->p++; break; }
            runtime_error("json parse error: expect ',' or '}'");
            return m;
        }
        return m;
    }
    if(c == '[') {
        j->p++;
        size_t cap = 8, len = 0;
        Value* items = (Value*)malloc(cap * sizeof(Value));
        jp_ws(j);
        if(j->p < j->end && *j->p == ']') { j->p++; Value r = val_array(len); for(size_t i = 0; i < len; i++) { gc_write_barrier(items[i]); r.v.array->items[i] = items[i]; } free(items); return r; }
        for(;;) {
            Value val = jp_parse_value(j);
            if(len >= cap) { cap *= 2; items = (Value*)realloc(items, cap * sizeof(Value)); }
            items[len++] = val;
            jp_ws(j);
            if(j->p < j->end && *j->p == ',') { j->p++; continue; }
            if(j->p < j->end && *j->p == ']') { j->p++; break; }
            runtime_error("json parse error: expect ',' or ']'");
            break;
        }
        Value r = val_array(len);
        for(size_t i = 0; i < len; i++) { gc_write_barrier(items[i]); r.v.array->items[i] = items[i]; }
        free(items);
        return r;
    }
    if(c == '"') return jp_parse_string(j);
    if(c == 't') { if(j->end - j->p >= 4 && strncmp(j->p, "true", 4) == 0) { j->p += 4; return lumyr_make_bool(1); } runtime_error("json parse error: bad literal"); return val_none(); }
    if(c == 'f') { if(j->end - j->p >= 5 && strncmp(j->p, "false", 5) == 0) { j->p += 5; return lumyr_make_bool(0); } runtime_error("json parse error: bad literal"); return val_none(); }
    if(c == 'n') { if(j->end - j->p >= 4 && strncmp(j->p, "null", 4) == 0) { j->p += 4; return val_none(); } runtime_error("json parse error: bad literal"); return val_none(); }
    if(c == '-' || isdigit((unsigned char)c)) return jp_parse_number(j);
    runtime_error("json parse error: unexpected character");
    return val_none();
}

Value lumyr_json_parse_enc(const char* s, Value enc)
{
    if(!s) { runtime_error("json(): input is null"); return val_none(); }
    char* conv = lumyr_text_to_utf8(s, strlen(s), enc);
    if(!conv) { runtime_error("json() 字符编码转换失败"); return val_none(); }
    JP j;
    j.p = conv;
    j.end = conv + strlen(conv);
    Value v = jp_parse_value(&j);
    jp_ws(&j);
    if(j.p != j.end) { runtime_error("json parse error: trailing data"); return val_none(); }
    if(conv != s) free(conv);
    return v;
}

Value lumyr_json_parse(const char* s)
{
    return lumyr_json_parse_enc(s, val_none());
}

/* ========== 序列化 ========== */

typedef struct {
    char* buf;
    size_t len;
    size_t cap;
} SB;

static void sb_grow(SB* b, size_t need)
{
    if(b->len + need + 1 > b->cap) {
        size_t nc = b->cap > 0 ? b->cap : 64;
        while(b->len + need + 1 > nc) nc *= 2;
        b->buf = (char*)realloc(b->buf, nc);
        b->cap = nc;
    }
}

static void sb_putc(SB* b, char c) { sb_grow(b, 1); b->buf[b->len++] = c; }
static void sb_puts(SB* b, const char* s) { size_t n = strlen(s); sb_grow(b, n); memcpy(b->buf + b->len, s, n); b->len += n; }

// 输出 JSON 字符串字面量（含转义）
static void sb_json_string(SB* b, const char* s)
{
    sb_putc(b, '"');
    for(const unsigned char* p = (const unsigned char*)s; *p; p++) {
        unsigned char c = *p;
        switch(c) {
            case '"':  sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            case '\n': sb_puts(b, "\\n"); break;
            case '\r': sb_puts(b, "\\r"); break;
            case '\t': sb_puts(b, "\\t"); break;
            case '\b': sb_puts(b, "\\b"); break;
            case '\f': sb_puts(b, "\\f"); break;
            default:
                if(c < 0x20) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    sb_puts(b, tmp);
                } else {
                    sb_putc(b, (char)c);
                }
        }
    }
    sb_putc(b, '"');
}

static void jq_stringify(SB* b, Value v, Value enc)
{
    switch(v.type) {
        case VAL_NONE: sb_puts(b, "null"); break;
        case VAL_BOOL: sb_puts(b, v.v.b ? "true" : "false"); break;
        /* 有符号整数类型：每个类型独立 case，直接读对应字段 */
        case VAL_INT: {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%d", v.v.i); sb_puts(b, tmp); break;
        }
        case VAL_INT8: {
            char tmp[16]; snprintf(tmp, sizeof(tmp), "%d", (int)v.v.i8); sb_puts(b, tmp); break;
        }
        case VAL_INT16: {
            char tmp[16]; snprintf(tmp, sizeof(tmp), "%d", (int)v.v.i16); sb_puts(b, tmp); break;
        }
        case VAL_SHORT: {
            char tmp[16]; snprintf(tmp, sizeof(tmp), "%d", (int)v.v.sh); sb_puts(b, tmp); break;
        }
        case VAL_INT32: {
            char tmp[16]; snprintf(tmp, sizeof(tmp), "%d", (int)v.v.i32); sb_puts(b, tmp); break;
        }
        case VAL_INT64: {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%lld", (long long)v.v.i64); sb_puts(b, tmp); break;
        }
        case VAL_LONG_LONG: {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%lld", v.v.ll); sb_puts(b, tmp); break;
        }
        case VAL_LONG: {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%ld", v.v.l); sb_puts(b, tmp); break;
        }
        /* 无符号整数类型：每个类型独立 case，直接读对应字段 */
        case VAL_BYTE: {
            char tmp[8]; snprintf(tmp, sizeof(tmp), "%u", (unsigned int)v.v.by); sb_puts(b, tmp); break;
        }
        case VAL_UINT8: {
            char tmp[8]; snprintf(tmp, sizeof(tmp), "%u", (unsigned int)v.v.u8); sb_puts(b, tmp); break;
        }
        case VAL_UCHAR: {
            char tmp[8]; snprintf(tmp, sizeof(tmp), "%u", (unsigned int)v.v.uc); sb_puts(b, tmp); break;
        }
        case VAL_UINT16: {
            char tmp[8]; snprintf(tmp, sizeof(tmp), "%u", (unsigned int)v.v.u16); sb_puts(b, tmp); break;
        }
        case VAL_USHORT: {
            char tmp[8]; snprintf(tmp, sizeof(tmp), "%u", (unsigned int)v.v.us); sb_puts(b, tmp); break;
        }
        case VAL_UINT32: {
            char tmp[16]; snprintf(tmp, sizeof(tmp), "%u", (unsigned int)v.v.u32); sb_puts(b, tmp); break;
        }
        case VAL_UINT: {
            char tmp[16]; snprintf(tmp, sizeof(tmp), "%u", (unsigned int)v.v.ui); sb_puts(b, tmp); break;
        }
        case VAL_UINT64: {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v.v.u64); sb_puts(b, tmp); break;
        }
        case VAL_ULONG: {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%lu", v.v.ul); sb_puts(b, tmp); break;
        }
        case VAL_SIZE_T: {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%zu", v.v.st); sb_puts(b, tmp); break;
        }
        case VAL_SSIZE_T: {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%zd", v.v.sst); sb_puts(b, tmp); break;
        }
        /* 浮点类型：每个类型独立 case */
        case VAL_FLOAT: {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "%g", (double)v.v.f); sb_puts(b, tmp); break;
        }
        case VAL_DOUBLE: {
            char tmp[64]; snprintf(tmp, sizeof(tmp), "%g", v.v.d); sb_puts(b, tmp); break;
        }
        case VAL_LONG_DOUBLE: {
            char tmp[64]; snprintf(tmp, sizeof(tmp), "%Lg", v.v.ld); sb_puts(b, tmp); break;
        }
        case VAL_CHAR: {
            char one[2] = { v.v.c, '\0' };
            if(enc.type == VAL_NONE || (enc.type == VAL_STRING && (!lumyr_str_cstr(&enc) || !*lumyr_str_cstr(&enc))))
                sb_json_string(b, one);
            else { char* t = lumyr_utf8_to_text(one, enc); sb_json_string(b, t ? t : one); free(t); }
            break;
        }
        case VAL_STRING: {
            if(enc.type == VAL_NONE || (enc.type == VAL_STRING && (!lumyr_str_cstr(&enc) || !*lumyr_str_cstr(&enc))))
                sb_json_string(b, lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "");
            else { char* t = lumyr_utf8_to_text(lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "", enc); sb_json_string(b, t ? t : ""); free(t); }
            break;
        }
        case VAL_ARRAY: {
            sb_putc(b, '[');
            for(int i = 0; i < v.v.array->len; i++) {
                if(i > 0) sb_putc(b, ',');
                jq_stringify(b, v.v.array->items[i], enc);
            }
            sb_putc(b, ']');
            break;
        }
        case VAL_MAP: {
            sb_putc(b, '{');
            MapIter it; map_iter_init(&it, v.v.map);
            Value k, vv; int first = 1;
            while(map_iter_next(&it, &k, &vv)) {
                if(!first) sb_putc(b, ',');
                first = 0;
                char* kstr = value_to_str(k);
                if(enc.type == VAL_NONE || (enc.type == VAL_STRING && (!lumyr_str_cstr(&enc) || !*lumyr_str_cstr(&enc))))
                    sb_json_string(b, kstr);
                else { char* t = lumyr_utf8_to_text(kstr, enc); sb_json_string(b, t ? t : kstr); free(t); }
                free(kstr);
                sb_putc(b, ':');
                jq_stringify(b, vv, enc);
            }
            sb_putc(b, '}');
            break;
        }
        default: sb_puts(b, "null"); break;
    }
}

char* lumyr_json_stringify_enc(Value v, Value enc)
{
    SB b;
    b.buf = NULL;
    b.len = 0;
    b.cap = 0;
    jq_stringify(&b, v, enc);
    sb_putc(&b, '\0');
    return b.buf;
}

char* lumyr_json_stringify(Value v)
{
    return lumyr_json_stringify_enc(v, val_none());
}
