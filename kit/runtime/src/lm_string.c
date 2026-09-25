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

// === f-string format spec 格式化（兼容子集） ===
// spec 语法：[[fill]align][sign][#][0][width][,][.prec][type]
//   fill    任意单字节字符（默认空格）
//   align   < > ^ = （= 仅数值：符号后填充）
//   sign    + - 空格 （+ 正数显式+，空格 正数前加空格，- 默认仅负数）
//   #       进制前缀 0x/0o/0b（仅 x/X/o/b）
//   0       数值零填充（等价 fill=0 align==，与显式 align 互斥）
//   width   最小宽度
//   ,       千分位（整数部分每 3 位）
//   .prec   精度（浮点小数位 / 字符串最大长度）
//   type    d x X o b c f F e E g G % s （空=s）
typedef struct {
    char fill;
    char align;     // 0=未设
    char sign;      // 默认 '-'
    int  alt;       // # 0/1
    int  zero;      // 0 标志 0/1
    int  width;
    int  comma;
    int  prec;      // -1=未设
    char type;      // 0=未设（默认 s）
} FmtSpec;

// 解析 spec 串 → FmtSpec；成功 1，失败 0
static int parse_spec(const char* s, FmtSpec* o) {
    memset(o, 0, sizeof(*o));
    o->fill = ' ';
    o->sign = '-';
    o->prec = -1;
    const char* p = s;
    // [[fill]align]：后跟 align 字符则前一字节为 fill
    if(p[0] && p[1] && (p[1]=='<'||p[1]=='>'||p[1]=='^'||p[1]=='=')) {
        o->fill = p[0];
        o->align = p[1];
        p += 2;
    } else if(p[0]=='<'||p[0]=='>'||p[0]=='^'||p[0]=='=') {
        o->align = p[0];
        p++;
    }
    // sign
    if(*p=='+'||*p=='-'||*p==' ') { o->sign = *p; p++; }
    // #
    if(*p=='#') { o->alt = 1; p++; }
    // 0 标志
    if(*p=='0') { o->zero = 1; p++; }
    // width
    while(*p>='0'&&*p<='9') { o->width = o->width*10 + (*p-'0'); p++; }
    // ,
    if(*p==',') { o->comma = 1; p++; }
    // .prec
    if(*p=='.') {
        p++;
        int pr = 0;
        while(*p>='0'&&*p<='9') { pr = pr*10 + (*p-'0'); p++; }
        o->prec = pr;
    }
    // type
    if(*p) {
        o->type = *p;
        p++;
        if(*p) return 0;  // 多余字符
    }
    return 1;
}

// 值转 double（高精度数值经字符串中转，精度可能损失）
static double value_to_double(Value v) {
    if(v.type == VAL_DOUBLE) return v.v.d;
    if(v.type == VAL_FLOAT)  return (double)v.v.f;
    if(v.type == VAL_LONG_DOUBLE) return (double)v.v.ld;
    if(v.type == VAL_BIGINT || v.type == VAL_DECIMAL || v.type == VAL_BITDECIMAL) {
        char* s = value_to_str(v);
        double d = s ? strtod(s, NULL) : 0.0;
        free(s);
        return d;
    }
    return value_as_number(v);
}

// 二进制串（malloc）：整数按位模式输出（负数按二补码位模式）
static char* int_to_bin(long long n) {
    if(n == 0) { char* r = (char*)malloc(2); r[0] = '0'; r[1] = '\0'; return r; }
    unsigned long long u = (unsigned long long)n;
    char tmp[80];
    int i = 0;
    while(u) { tmp[i++] = (char)('0' + (u & 1)); u >>= 1; }
    char* r = (char*)malloc(i + 1);
    for(int k = 0; k < i; k++) r[k] = tmp[i-1-k];
    r[i] = '\0';
    return r;
}

// 千分位：对数值串的整数部分（符号后到小数点/e/E/% 前）每 3 位插 ','
// 规则：从右到左每 3 位一组，组间插 ','（第一组位数 = ilen % 3，若 0 则 3）
static char* apply_comma(const char* numstr) {
    const char* p = numstr;
    if(*p=='+'||*p=='-'||*p==' ') p++;
    const char* end = p;
    while(*end && *end!='.' && *end!='e' && *end!='E' && *end!='%') end++;
    int ilen = (int)(end - p);
    int ncomma = ilen > 0 ? (ilen - 1) / 3 : 0;
    int suffix_len = (int)strlen(end);
    /* 根因修复：带符号（+/-/空格）时符号位也写入 out（下方 w++），
     * newlen 必须包含它——否则负数+千分位 memcpy 溢出 1 字节（ASAN 堆溢出） */
    int sign_len = (p != numstr) ? 1 : 0;
    int newlen = sign_len + ilen + ncomma + suffix_len + 1;
    char* out = (char*)malloc(newlen);
    int w = 0;
    if(p != numstr) out[w++] = numstr[0];  // 符号
    // 第一组位数（从右数剩余）
    int first_grp = ilen % 3;
    if(first_grp == 0) first_grp = 3;
    const char* q = p;
    int i = 0;
    for(; i < first_grp && q < end; i++) out[w++] = *q++;
    // 之后每组 3 位，组前插 ','
    while(q < end) {
        out[w++] = ',';
        for(int k = 0; k < 3 && q < end; k++) out[w++] = *q++;
    }
    memcpy(out + w, end, suffix_len + 1);
    return out;
}

// 按 align/width 填充。core 已含 sign/进制前缀/千分位。
// is_numeric：数值类（默认右对齐），字符串类（默认左对齐）
static char* apply_align(const char* core, FmtSpec* fs, int is_numeric) {
    int len = (int)strlen(core);
    if(fs->width <= len) return strdup(core);
    int pad = fs->width - len;
    char fill = fs->fill;
    char align = fs->align;
    if(!align) align = is_numeric ? '>' : '<';
    // zero 标志：数值且无显式 align 时，用 0 填充且 align==
    if(fs->zero && !fs->align && is_numeric) { fill = '0'; align = '='; }
    char* out = (char*)malloc(fs->width + 1);
    if(!out) { perror("format"); exit(EXIT_FAILURE); }
    if(align == '<') {
        memcpy(out, core, len);
        for(int i = 0; i < pad; i++) out[len + i] = fill;
        out[fs->width] = '\0';
    } else if(align == '>') {
        for(int i = 0; i < pad; i++) out[i] = fill;
        memcpy(out + pad, core, len);
        out[fs->width] = '\0';
    } else if(align == '^') {
        int left = pad / 2;
        int right = pad - left;
        for(int i = 0; i < left; i++) out[i] = fill;
        memcpy(out + left, core, len);
        for(int i = 0; i < right; i++) out[left + len + i] = fill;
        out[fs->width] = '\0';
    } else { // '='
        int sl = 0;
        if(core[0]=='+'||core[0]=='-'||core[0]==' ') sl = 1;
        memcpy(out, core, sl);
        for(int i = 0; i < pad; i++) out[sl + i] = fill;
        memcpy(out + sl + pad, core + sl, len - sl);
        out[fs->width] = '\0';
    }
    return out;
}

// 主格式化：按 spec 格式化单个值，返回 malloc 串（调用方 free）
static char* format_value_with_spec(Value v, const char* spec) {
    FmtSpec fs;
    if(!parse_spec(spec, &fs)) runtime_error("format() 无效的格式说明符");
    char t = fs.type ? fs.type : 's';
    int prec = fs.prec;

    size_t cap = 8192;
    char* core = (char*)malloc(cap);
    if(!core) { perror("format"); exit(EXIT_FAILURE); }
    core[0] = '\0';
    int is_numeric = 0;
    int is_int_type = 0;

    #define FS_ENSURE(need) do { \
        size_t _cur = strlen(core); \
        if(_cur + (size_t)(need) >= cap) { \
            while(_cur + (size_t)(need) >= cap) cap *= 2; \
            char* _nw = (char*)realloc(core, cap); \
            if(!_nw) { perror("format"); exit(EXIT_FAILURE); } \
            core = _nw; \
        } \
    } while(0)

    if(t == 's') {
        char* s = value_to_str(v);
        if(prec >= 0 && (int)strlen(s) > prec) s[prec] = '\0';
        FS_ENSURE(strlen(s) + 1);
        strcpy(core, s);
        free(s);
    } else if(t == 'c') {
        long long iv = lumyr_extract_ll(v);
        FS_ENSURE(2);
        core[0] = (char)(unsigned char)iv;
        core[1] = '\0';
    } else if(t == 'd') {
        is_numeric = 1; is_int_type = 1;
        if(v.type == VAL_BIGINT) {
            char* bs = value_to_str(v);
            FS_ENSURE(strlen(bs) + 1);
            strcpy(core, bs);
            free(bs);
        } else {
            long long iv = lumyr_extract_ll(v);
            FS_ENSURE(32);
            snprintf(core, cap, "%lld", iv);
        }
    } else if(t == 'x' || t == 'X' || t == 'o' || t == 'b') {
        is_numeric = 1; is_int_type = 1;
        if(v.type == VAL_BIGINT)
            runtime_error("format() bigint 不支持 x/X/o/b 进制格式说明符");
        long long iv = lumyr_extract_ll(v);
        if(t == 'b') {
            char* bs = int_to_bin(iv);
            FS_ENSURE(strlen(bs) + 1);
            strcpy(core, bs);
            free(bs);
        } else {
            FS_ENSURE(32);
            if(t == 'x') snprintf(core, cap, "%llx", (unsigned long long)iv);
            else if(t == 'X') snprintf(core, cap, "%llX", (unsigned long long)iv);
            else snprintf(core, cap, "%llo", (unsigned long long)iv);
        }
    } else if(t == 'f' || t == 'F' || t == 'e' || t == 'E' ||
              t == 'g' || t == 'G' || t == '%') {
        is_numeric = 1;
        if(prec < 0) prec = 6;
        double dv = value_to_double(v);
        if(t == '%') dv *= 100.0;
        FS_ENSURE(64);
        if(t == 'f' || t == 'F') snprintf(core, cap, "%.*f", prec, dv);
        else if(t == 'e') snprintf(core, cap, "%.*e", prec, dv);
        else if(t == 'E') snprintf(core, cap, "%.*E", prec, dv);
        else if(t == 'g') snprintf(core, cap, "%.*g", prec, dv);
        else if(t == 'G') snprintf(core, cap, "%.*G", prec, dv);
        else { // %
            snprintf(core, cap, "%.*f", prec, dv);
            FS_ENSURE(strlen(core) + 2);
            size_t l = strlen(core);
            core[l] = '%';
            core[l + 1] = '\0';
        }
    } else {
        free(core);
        runtime_error("format() 不支持的格式类型字符");
    }

    // sign：正数补符号（数值类，非 c）
    if(is_numeric && t != 'c' && (fs.sign == '+' || fs.sign == ' ')) {
        if(core[0] != '-') {
            size_t l = strlen(core);
            FS_ENSURE(l + 2);
            memmove(core + 1, core, l + 1);
            core[0] = fs.sign;
        }
    }
    // 进制前缀 #
    if(fs.alt && is_int_type && (t == 'x' || t == 'X' || t == 'o' || t == 'b')) {
        const char* px = (t == 'x') ? "0x" : (t == 'X') ? "0X" : (t == 'o') ? "0o" : "0b";
        size_t l = strlen(core);
        int has_sign = (core[0] == '-' || core[0] == '+' || core[0] == ' ');
        FS_ENSURE(l + 3);
        if(has_sign) {
            char sc = core[0];
            memmove(core + 3, core + 1, l);  // 数值部分后移（含末尾 \0）
            core[0] = sc; core[1] = px[0]; core[2] = px[1];
        } else {
            memmove(core + 2, core, l + 1);
            core[0] = px[0]; core[1] = px[1];
        }
    }
    // 千分位
    char* after_comma;
    if(fs.comma && is_numeric && t != 'c') {
        after_comma = apply_comma(core);
    } else {
        after_comma = strdup(core);
    }
    // 对齐填充
    char* result = apply_align(after_comma, &fs, is_numeric);
    free(after_comma);
    free(core);
    #undef FS_ENSURE
    return result;
}

// 格式化（模板字符串 f"{x:spec}" 编译目标 + 全局 format(fmt, args...)）
// 占位语法：{spec}（spec 可空；spec 内不能含 '{'；{{/}} 转义字面花括号）
Value lumyr_format(Value* args, int n) {
    if(n < 1 || args[0].type != VAL_STRING) runtime_error("format() 第一个参数必须是格式串");
    const char* fmt = lumyr_str_cstr(&args[0]);
    int nargs = n - 1;
    // 第一遍：扫描占位计数并校验配对
    int placeholders = 0;
    const char* scan = fmt;
    while(*scan) {
        if(scan[0] == '{' && scan[1] == '{') { scan += 2; continue; }
        if(scan[0] == '}' && scan[1] == '}') { scan += 2; continue; }
        if(scan[0] == '}') runtime_error("format() 格式串含未配对的 '}'");
        if(scan[0] == '{') {
            // spec 内不能含 '{'，找下一个 '}'
            const char* q = scan + 1;
            while(*q && *q != '}' && *q != '{') q++;
            if(*q == '{') runtime_error("format() 格式串占位内出现 '{'");
            if(*q != '}') runtime_error("format() 格式串含未配对的 '{'");
            placeholders++;
            scan = q + 1;
            continue;
        }
        scan++;
    }
    if(placeholders != nargs) {
        char b[128];
        snprintf(b, sizeof b, "format() 占位符 %d 个（给了 %d 个实参）", placeholders, nargs);
        runtime_error(b);
    }
    // 预估容量
    size_t cap = strlen(fmt) + 64;
    for(int i = 0; i < nargs; i++) {
        char* t = value_to_str(args[i + 1]);
        cap += strlen(t) + 32;
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
        if(p[0] == '{') {
            // 提取 spec：[p+1, close)；兼容前导 ':'（f-string 经 tmpl 产出的 spec 无冒号，
            // 用户直接 format("{:spec}", x) 写的占位含冒号，统一跳过前导冒号）
            const char* q = p + 1;
            while(*q && *q != '}') q++;
            // *q 一定是 '}'（第一遍已校验）
            const char* spec_src = p + 1;
            size_t slen = (size_t)(q - spec_src);
            if(slen > 0 && spec_src[0] == ':') { spec_src++; slen--; }
            char spec[256];
            if(slen >= sizeof(spec)) runtime_error("format() 格式说明符过长");
            memcpy(spec, spec_src, slen);
            spec[slen] = '\0';
            char* r = format_value_with_spec(args[ai + 1], spec);
            size_t rl = strlen(r);
            if(w + rl > cap) {
                while(w + rl > cap) cap *= 2;
                out = (char*)realloc(out, cap + 1);
            }
            memcpy(out + w, r, rl);
            w += rl;
            free(r);
            ai++;
            p = q + 1;
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
