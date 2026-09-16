// lm_string.c —— 字符串操作内置函数
#include "lm_string.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>



Value lumyr_substr(Value s, Value start, Value n) {
    if(s.type != VAL_STRING) runtime_error("substr() 第一个参数必须是字符串");
    long long slen = (long long)strlen(lumyr_str_cstr(&s));
    long long i = array_index_of(start);
    long long cnt = array_index_of(n);
    if(i < 0 || i > slen) runtime_error("substr() 起始越界");
    if(cnt < 0) runtime_error("substr() 长度不能为负数");
    if(i + cnt > slen) cnt = slen - i;
    char* out = (char*)malloc(cnt + 1);
    if(!out) { perror("lumyr_substr"); exit(EXIT_FAILURE); }
    memcpy(out, lumyr_str_cstr(&s) + i, cnt);
    out[cnt] = '\0';
    Value r = lumyr_make_string(out);
    free(out);
    return r;
}

// 写回数组元素（深拷贝），返回 val 作为表达式值

static Value str_case(Value s, int upper)
{
    if(s.type != VAL_STRING) runtime_error("参数必须是字符串");
    char* out = (char*)malloc(strlen(lumyr_str_cstr(&s)) + 1);
    if(!out) { perror("str_case"); exit(EXIT_FAILURE); }
    const unsigned char* p = (const unsigned char*)lumyr_str_cstr(&s);
    char* q = out;
    while(*p) {
        if(upper && *p >= 'a' && *p <= 'z') *q = *p - 'a' + 'A';
        else if(!upper && *p >= 'A' && *p <= 'Z') *q = *p - 'A' + 'a';
        else *q = (char)*p;
        p++; q++;
    }
    *q = '\0';
    Value r = lumyr_make_string(out);
    free(out);
    return r;
}

Value lumyr_toupper(Value s) { return str_case(s, 1); }

Value lumyr_tolower(Value s) { return str_case(s, 0); }

// split(s, sep)：按分隔符拆成字符串数组

Value lumyr_split(Value s, Value sep)
{
    if(s.type != VAL_STRING || sep.type != VAL_STRING)
        runtime_error("split() 参数必须是字符串");
    if(lumyr_str_cstr(&sep)[0] == '\0') runtime_error("split() 分隔符不能为空");
    const char* p = lumyr_str_cstr(&s);
    const char* sp = lumyr_str_cstr(&sep);
    size_t splen = strlen(sp);
    int count = 1;
    for(const char* t = p; (t = strstr(t, sp)) != NULL; t += splen) count++;
    Value arr = val_array(count);
    int idx = 0;
    const char* start = p;
    const char* hit = strstr(start, sp);
    while(hit) {
        size_t len = (size_t)(hit - start);
        char* piece = (char*)malloc(len + 1);
        memcpy(piece, start, len);
        piece[len] = '\0';
        Value item = lumyr_make_string(piece);
        free(piece);
        gc_write_barrier(item);
        arr.v.array->items[idx++] = item;
        start = hit + splen;
        hit = strstr(start, sp);
    }
    Value item = lumyr_make_string(start);
    gc_write_barrier(item);
    arr.v.array->items[idx++] = item;
    return arr;
}

// del(arr, idx)：返回删除第 idx 个元素后的新数组（值语义，原数组不变）

Value lumyr_join(Value arr, Value sep)
{
    if(arr.type != VAL_ARRAY) runtime_error("join() 第一个参数必须是数组");
    if(sep.type != VAL_STRING) runtime_error("join() 分隔符必须是字符串");
    size_t total = 1;
    for(int i = 0; i < arr.v.array->len; i++) {
        char* t = value_to_str(arr.v.array->items[i]);
        total += strlen(t);
        if(i < arr.v.array->len - 1) total += strlen(lumyr_str_cstr(&sep));
        free(t);
    }
    char* out = (char*)malloc(total);
    if(!out) { perror("join"); exit(EXIT_FAILURE); }
    out[0] = '\0';
    for(int i = 0; i < arr.v.array->len; i++) {
        if(i > 0) strcat(out, lumyr_str_cstr(&sep));
        char* t = value_to_str(arr.v.array->items[i]);
        strcat(out, t);
        free(t);
    }
    Value r = lumyr_make_string(out);
    free(out);
    return r;
}

// contains：字符串子串 / 数组元素相等

Value lumyr_contains(Value hay, Value needle)
{
    if(hay.type == VAL_STRING) {
        if(needle.type != VAL_STRING) runtime_error("contains() 字符串查找需要字符串参数");
        return lumyr_make_bool(strstr(lumyr_str_cstr(&hay), lumyr_str_cstr(&needle)) != NULL);
    }
    if(hay.type == VAL_ARRAY) {
        for(int i = 0; i < hay.v.array->len; i++) {
            Value eq = lumyr_eq(hay.v.array->items[i], needle);
            if(eq.v.b) return lumyr_make_bool(1);
        }
        return lumyr_make_bool(0);
    }
    if(hay.type == VAL_MAP) {
        if(needle.type != VAL_STRING) runtime_error("contains() 字典键必须是字符串");
        return lumyr_make_bool(lumyr_map_has(hay, needle));
    }
    runtime_error("contains() 第一个参数必须是字符串、数组或字典");
    return val_none();
}

// repeat(s, n)：字符串重复 n 次

Value lumyr_repeat(Value s, Value n)
{
    if(s.type != VAL_STRING) runtime_error("repeat() 第一个参数必须是字符串");
    /* 支持所有整数类型作为次数参数 */
    if(n.type != VAL_INT && n.type != VAL_INT8 && n.type != VAL_INT16 && n.type != VAL_INT32 && n.type != VAL_INT64 &&
       n.type != VAL_BYTE && n.type != VAL_UINT8 && n.type != VAL_UINT16 && n.type != VAL_UINT32 && n.type != VAL_UINT64 &&
       n.type != VAL_LONG && n.type != VAL_ULONG && n.type != VAL_SIZE_T && n.type != VAL_SSIZE_T &&
       n.type != VAL_BOOL && n.type != VAL_CHAR && n.type != VAL_DOUBLE)
        runtime_error("repeat() 次数必须是整数");
    long long k = lumyr_extract_ll(n);
    if(k < 0) runtime_error("repeat() 次数不能为负数");
    size_t len = strlen(lumyr_str_cstr(&s));
    if(k > 0 && len > (size_t)((1ULL << 40) / k)) runtime_error("repeat() 结果过大");
    size_t total = len * (size_t)k;
    char* out = (char*)malloc(total + 1);
    if(!out) { perror("repeat"); exit(EXIT_FAILURE); }
    for(long long i = 0; i < k; i++) memcpy(out + len * (size_t)i, lumyr_str_cstr(&s), len);
    out[total] = '\0';
    Value r = lumyr_make_string(out);
    free(out);
    return r;
}

// replace(s, from, to)：替换所有 from 为 to（from 空串报错）

Value lumyr_replace(Value s, Value from, Value to)
{
    if(s.type != VAL_STRING || from.type != VAL_STRING || to.type != VAL_STRING)
        runtime_error("replace() 三个参数都必须是字符串");
    if(lumyr_str_cstr(&from)[0] == '\0') runtime_error("replace() 被替换串不能为空");
    const char* p = lumyr_str_cstr(&s);
    const char* f = lumyr_str_cstr(&from);
    const char* t = lumyr_str_cstr(&to);
    size_t flen = strlen(f), tlen = strlen(t), slen = strlen(p);
    int count = 0;
    for(const char* q = p; (q = strstr(q, f)) != NULL; q += flen) count++;
    if(count == 0) return lumyr_make_string(p);  // 无匹配，原样返回
    size_t outlen = slen + (size_t)count * (tlen > flen ? tlen - flen : 0);
    char* out = (char*)malloc(outlen + 1);
    if(!out) { perror("replace"); exit(EXIT_FAILURE); }
    char* w = out;
    const char* start = p;
    const char* hit = strstr(start, f);
    while(hit) {
        size_t pre = (size_t)(hit - start);
        memcpy(w, start, pre); w += pre;
        memcpy(w, t, tlen); w += tlen;
        start = hit + flen;
        hit = strstr(start, f);
    }
    size_t rest = strlen(start);
    memcpy(w, start, rest); w += rest;
    *w = '\0';
    Value r = lumyr_make_string(out);
    free(out);
    return r;
}

// sum/avg：数字数组聚合（只允许 int/double 元素）

Value lumyr_format(Value* args, int n) {
    if(n < 1 || args[0].type != VAL_STRING) runtime_error("format() 第一个参数必须是格式串");
    const char* fmt = lumyr_str_cstr(&args[0]);
    int nargs = n - 1;
    int placeholders = 0;
    const char* scan = fmt;
    while(*scan) {
        if(scan[0] == '{' && scan[1] == '{') { scan += 2; continue; }
        if(scan[0] == '}' && scan[1] == '}') { scan += 2; continue; }
        if(scan[0] == '{' && scan[1] == '}') { placeholders++; scan += 2; continue; }
        if(scan[0] == '{') runtime_error("format() 格式串含未配对的 '{'");
        if(scan[0] == '}') runtime_error("format() 格式串含未配对的 '}'");
        scan++;
    }
    if(placeholders != nargs) {
        char b[128];
        snprintf(b, sizeof b, "format() 占位符 %d 个（给了 %d 个实参）", placeholders, nargs);
        runtime_error(b);
    }
    size_t cap = strlen(fmt) + 64;
    for(int i = 0; i < nargs; i++) {
        char* t = value_to_str(args[i + 1]);
        cap += strlen(t);
        free(t);
    }
    char* out = (char*)malloc(cap + 1);
    if(!out) { perror("format"); exit(EXIT_FAILURE); }
    size_t w = 0;
    int ai = 0;
    const char* p = fmt;
    while(*p) {
        if(p[0] == '{' && p[1] == '{') { out[w++] = '{'; p += 2; continue; }
        if(p[0] == '}' && p[1] == '}') { out[w++] = '}'; p += 2; continue; }
        if(p[0] == '{' && p[1] == '}') {
            char* t = value_to_str(args[ai + 1]);
            size_t tl = strlen(t);
            memcpy(out + w, t, tl); w += tl;
            free(t);
            ai++;
            p += 2;
            continue;
        }
        out[w++] = *p++;
    }
    out[w] = '\0';
    Value r = lumyr_make_string(out);
    free(out);
    return r;
}

Value lumyr_strip(Value s)
{
    if(s.type != VAL_STRING) runtime_error("strip() 参数必须是字符串");
    const char* p = lumyr_str_cstr(&s);
    while(*p && isspace((unsigned char)*p)) p++;
    size_t len = strlen(p);
    while(len > 0 && isspace((unsigned char)p[len - 1])) len--;
    char* out = (char*)malloc(len + 1);
    if(!out) { perror("strip"); exit(EXIT_FAILURE); }
    memcpy(out, p, len);
    out[len] = '\0';
    Value r = lumyr_make_string(out);
    free(out);
    return r;
}

// startswith / endswith：前缀/后缀判断

Value lumyr_startswith(Value s, Value prefix)
{
    if(s.type != VAL_STRING || prefix.type != VAL_STRING)
        runtime_error("startswith() 两个参数都必须是字符串");
    size_t sl = strlen(lumyr_str_cstr(&s)), pl = strlen(lumyr_str_cstr(&prefix));
    return lumyr_make_bool(pl <= sl && strncmp(lumyr_str_cstr(&s), lumyr_str_cstr(&prefix), pl) == 0);
}

Value lumyr_endswith(Value s, Value suffix)
{
    if(s.type != VAL_STRING || suffix.type != VAL_STRING)
        runtime_error("endswith() 两个参数都必须是字符串");
    size_t sl = strlen(lumyr_str_cstr(&s)), fl = strlen(lumyr_str_cstr(&suffix));
    return lumyr_make_bool(fl <= sl && strcmp(lumyr_str_cstr(&s) + sl - fl, lumyr_str_cstr(&suffix)) == 0);
}


// ---------------- 文件 IO 内置（read "path" / write "path" value / file_exists） ----------------

// read_file(path) → 文件全部内容（字符串）；失败 → runtime_error
