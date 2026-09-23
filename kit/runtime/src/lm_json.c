// lm_json.c —— json(s) / stringify(v) 内置函数
// 递归下降 JSON 解析器 + 值序列化器
// 双通道共享（VM 与 C 编译通道都调用本模块）
#include "lm_value.h"
#include "lm_charset.h"
#include "lm_container.h"
#include "lm_bigint.h"
#include "lm_decimal.h"
#include "lm_bitdecimal.h"
#include "lm_time.h"
#include "lm_calendar.h"
#include "lumyr_typed_arrays.h"
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

/* 静默模式（try_parse 用）：解析失败不调 runtime_error（HTTP 场景下那会直接退出），
 * 只置 jp_error；正常 json() 调用行为不变 */
static _Thread_local int jp_silent = 0;
static _Thread_local int jp_error = 0;
static void jp_fail(const char* msg) {
    jp_error = 1;
    if(!jp_silent) runtime_error(msg);
}

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
            if(j->p >= j->end || *j->p != '"') { jp_fail("json parse error: expect string key"); return m; }
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
    if(c == 't') { if(j->end - j->p >= 4 && strncmp(j->p, "true", 4) == 0) { j->p += 4; return lumyr_make_bool(1); } jp_fail("json parse error: bad literal"); return val_none(); }
    if(c == 'f') { if(j->end - j->p >= 5 && strncmp(j->p, "false", 5) == 0) { j->p += 5; return lumyr_make_bool(0); } jp_fail("json parse error: bad literal"); return val_none(); }
    if(c == 'n') { if(j->end - j->p >= 4 && strncmp(j->p, "null", 4) == 0) { j->p += 4; return val_none(); } jp_fail("json parse error: bad literal"); return val_none(); }
    if(c == '-' || isdigit((unsigned char)c)) return jp_parse_number(j);
    jp_fail("json parse error: unexpected character");
    return val_none();
}

/* 解析核心：silent 状态由调用方设置。成功 *out=v 返回 1；失败返回 0 */
static int json_core(const char* s, Value enc, Value* out)
{
    if(!s) { jp_fail("json(): input is null"); return 0; }
    char* conv = lumyr_text_to_utf8(s, strlen(s), enc);
    if(!conv) { jp_fail("json() 字符编码转换失败"); return 0; }
    JP j;
    j.p = conv;
    j.end = conv + strlen(conv);
    Value v = jp_parse_value(&j);
    jp_ws(&j);
    if(j.p != j.end) jp_fail("json parse error: trailing data");
    if(conv != s) free(conv);
    if(jp_error) return 0;
    *out = v;
    return 1;
}

Value lumyr_json_parse_enc(const char* s, Value enc)
{
    jp_silent = 0; jp_error = 0;
    Value v = val_none();
    json_core(s, enc, &v);
    return v;
}

Value lumyr_json_parse(const char* s)
{
    return lumyr_json_parse_enc(s, val_none());
}

/* 静默解析：失败返回 0（不打印、不退出），供 HTTP 按 Content-Type 自动识别 */
int lumyr_json_try_parse(const char* s, Value* out)
{
    jp_silent = 1; jp_error = 0;
    int ok = json_core(s, val_none(), out);
    jp_silent = 0;
    return ok;
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

// TypedArray 元素装箱：按 elem_type 从裸数据缓冲读取并构造对应类型 Value
static Value jq_typed_elem(ValueType et, const void* items, int i)
{
    switch(et) {
        case VAL_INT:        return lumyr_make_int(((const int*)items)[i]);
        case VAL_INT8:       return lumyr_make_int8(((const int8_t*)items)[i]);
        case VAL_INT16:      return lumyr_make_int16(((const int16_t*)items)[i]);
        case VAL_SHORT:      return lumyr_make_short(((const int16_t*)items)[i]);
        case VAL_INT32:      return lumyr_make_int32(((const int32_t*)items)[i]);
        case VAL_INT64:      return lumyr_make_int64(((const int64_t*)items)[i]);
        case VAL_LONG_LONG:  return lumyr_make_long_long(((const long long*)items)[i]);
        case VAL_LONG:       return lumyr_make_long(((const long*)items)[i]);
        case VAL_BYTE:       return lumyr_make_byte(((const uint8_t*)items)[i]);
        case VAL_UINT8:      return lumyr_make_uint8(((const uint8_t*)items)[i]);
        case VAL_UCHAR:      return lumyr_make_uchar(((const unsigned char*)items)[i]);
        case VAL_UINT16:     return lumyr_make_uint16(((const uint16_t*)items)[i]);
        case VAL_USHORT:     return lumyr_make_ushort(((const unsigned short*)items)[i]);
        case VAL_UINT32:     return lumyr_make_uint32(((const uint32_t*)items)[i]);
        case VAL_UINT:       return lumyr_make_uint(((const unsigned int*)items)[i]);
        case VAL_UINT64:     return lumyr_make_uint64(((const uint64_t*)items)[i]);
        case VAL_ULONG:      return lumyr_make_ulong(((const unsigned long*)items)[i]);
        case VAL_SIZE_T:     return lumyr_make_size_t(((const size_t*)items)[i]);
        case VAL_SSIZE_T:    return lumyr_make_ssize_t(((const ssize_t*)items)[i]);
        case VAL_BOOL:       return lumyr_make_bool(((const _Bool*)items)[i]);
        case VAL_CHAR:       return lumyr_make_char(((const char*)items)[i]);
        case VAL_FLOAT:      return lumyr_make_float(((const float*)items)[i]);
        case VAL_DOUBLE:     return lumyr_make_double(((const double*)items)[i]);
        case VAL_LONG_DOUBLE: return lumyr_make_long_double(((const long double*)items)[i]);
        case VAL_STRING:     return lumyr_make_string(((char* const*)items)[i]);
        default:             return val_none();
    }
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
        case VAL_TUPLE: {
            int n = lumyr_tuple_len(v);
            sb_putc(b, '[');
            for(int i = 0; i < n; i++) {
                if(i > 0) sb_putc(b, ',');
                jq_stringify(b, lumyr_tuple_get(v, i), enc);
            }
            sb_putc(b, ']');
            break;
        }
        case VAL_SET: {
            Value a = lumyr_set_to_array(v);
            int n = a.v.array ? a.v.array->len : 0;
            sb_putc(b, '[');
            for(int i = 0; i < n; i++) {
                if(i > 0) sb_putc(b, ',');
                jq_stringify(b, a.v.array->items[i], enc);
            }
            sb_putc(b, ']');
            break;
        }
        /* 高精度数值：to_string 输出十进制文本，作为 JSON 数字字面量 */
        case VAL_BIGINT: {
            char* s = lumyr_bigint_to_string((BigInt*)v.v.bigint);
            sb_puts(b, s ? s : "null");
            free(s);
            break;
        }
        case VAL_DECIMAL: {
            char* s = lumyr_decimal_to_string((Decimal*)v.v.decimal);
            sb_puts(b, s ? s : "null");
            free(s);
            break;
        }
        case VAL_BITDECIMAL: {
            char* s = lumyr_bitdecimal_to_string((BitDecimal*)v.v.bitdecimal);
            sb_puts(b, s ? s : "null");
            free(s);
            break;
        }
        /* date 族：统一 ISO 字符串 */
        case VAL_DATE: case VAL_DATETIME: case VAL_TIME: case VAL_TIMEDELTA: {
            char* s = lumyr_date_to_iso(v);
            sb_json_string(b, s ? s : "");
            free(s);
            break;
        }
        /* complex：[real, imag] */
        case VAL_COMPLEX: {
            const ComplexObj* co = (const ComplexObj*)v.v.complex_obj;
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "[%g,%g]", co->real, co->imag);
            sb_puts(b, tmp);
            break;
        }
        /* calendar：描述字符串 */
        case VAL_CALENDAR: {
            char* s = lumyr_calendar_to_str(v);
            sb_json_string(b, s ? s : "");
            free(s);
            break;
        }
        /* 类型化数组：按 elem_type 展开为 JSON 数组 */
        case VAL_TYPED_ARRAY: {
            const TypedArray* ta = (const TypedArray*)v.v.typed_array;
            sb_putc(b, '[');
            for(int i = 0; i < ta->len; i++) {
                if(i > 0) sb_putc(b, ',');
                jq_stringify(b, jq_typed_elem(ta->elem_type, ta->items, i), enc);
            }
            sb_putc(b, ']');
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
