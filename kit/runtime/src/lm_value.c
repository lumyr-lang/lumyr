#include "lm_value.h"
#include "lm_json.h"
#include "lm_class.h"
#include "lm_struct.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>

// 字典辅助（VAL_MAP）前向声明：lumyr_eq 等在定义之前引用
Value lumyr_make_int(long long i) {
    Value v;
    v.type = VAL_INT;
    v.v.i = i;
    return v;
}

Value lumyr_make_double(double d) {
    Value v;
    v.type = VAL_DOUBLE;
    v.v.d = d;
    return v;
}

Value lumyr_make_bool(_Bool b) {
    Value v;
    v.type = VAL_BOOL;
    v.v.b = b;
    return v;
}

/* 创建 C 结构体指针对象（零拷贝传递，类型由外部标识） */
Value lumyr_make_struct_ptr(void* ptr) {
    Value v;
    v.type = VAL_STRUCT_PTR;
    v.v.struct_ptr = ptr;
    return v;
}

Value lumyr_make_string(const char* s) {
    Value v;
    v.type = VAL_STRING;
    v.str_inline = 0;
    if(s == NULL)
    {
        v.v.s = NULL;
        return v;
    }
    size_t len = strlen(s);
    if (len <= LUMYR_SSO_MAX) {
        v.str_inline = 1;
        v.v.sso.len = (uint8_t)len;
        memcpy(v.v.sso.data, s, len);
        v.v.sso.data[len] = '\0';
    } else {
        v.v.s = (char*)gc_alloc(len + 1, VAL_STRING);
        memcpy(v.v.s, s, len);
        v.v.s[len] = '\0';
    }
    return v;
}

Value lumyr_make_char(char ch) {
    Value v;
    v.type = VAL_CHAR;
    v.v.c = ch;
    return v;
}

Value lumyr_make_byte(unsigned char b) {
    Value v;
    v.type = VAL_BYTE;
    v.v.i = (long long)(b & 0xFF);
    return v;
}

Value lumyr_make_int8(int8_t i8) {
    Value v;
    v.type = VAL_INT8;
    v.v.i = (long long)i8;
    return v;
}

Value lumyr_make_int16(int16_t i16) {
    Value v;
    v.type = VAL_INT16;
    v.v.i = (long long)i16;
    return v;
}

Value lumyr_make_int32(int32_t i32) {
    Value v;
    v.type = VAL_INT32;
    v.v.i = (long long)i32;
    return v;
}

Value lumyr_make_int64(int64_t i64) {
    Value v;
    v.type = VAL_INT64;
    v.v.i = (long long)i64;
    return v;
}

Value lumyr_make_uint8(uint8_t u8) {
    Value v;
    v.type = VAL_UINT8;
    v.v.i = (long long)u8;
    return v;
}

Value lumyr_make_uint16(uint16_t u16) {
    Value v;
    v.type = VAL_UINT16;
    v.v.i = (long long)u16;
    return v;
}

Value lumyr_make_uint32(uint32_t u32) {
    Value v;
    v.type = VAL_UINT32;
    v.v.i = (long long)u32;
    return v;
}

Value lumyr_make_uint64(uint64_t u64) {
    Value v;
    v.type = VAL_UINT64;
    v.v.i = (long long)u64;
    return v;
}

Value lumyr_make_long(long lv) {
    Value v;
    v.type = VAL_LONG;
    v.v.i = (long long)lv;
    return v;
}

Value lumyr_make_ulong(unsigned long ulv) {
    Value v;
    v.type = VAL_ULONG;
    v.v.i = (long long)ulv;
    return v;
}

Value lumyr_make_size_t(size_t stv) {
    Value v;
    v.type = VAL_SIZE_T;
    v.v.i = (long long)stv;
    return v;
}

Value lumyr_make_ssize_t(ssize_t sstv) {
    Value v;
    v.type = VAL_SSIZE_T;
    v.v.i = (long long)sstv;
    return v;
}

Value lumyr_make_long_double(long double ldv) {
    Value v;
    v.type = VAL_LONG_DOUBLE;
    v.v.d = (double)ldv;
    return v;
}

// 获取value的数值，int转double
double value_as_number(Value x) {
    if(x.type == VAL_INT)
        return (double)x.v.i;
    else if(x.type == VAL_DOUBLE)
        return x.v.d;
    else if(x.type == VAL_CHAR)
        return (double)x.v.c;
    else if(x.type == VAL_BYTE)
        return (double)(x.v.i & 0xFF);
    return 0.0;
}

// 判断是否是字符串类型
static int is_string(Value a, Value b) {
    return (a.type == VAL_STRING) || (b.type == VAL_STRING);
}

// 把一个Value转堆字符串（用于字符串拼接、弱比较）
char* value_to_str(Value v) {
    char buf[256];
    switch(v.type)
    {
        case VAL_INT:
            snprintf(buf, sizeof(buf), "%lld", v.v.i);
            break;
        case VAL_DOUBLE:
            snprintf(buf, sizeof(buf), "%g", v.v.d);
            break;
        case VAL_BOOL:
            strcpy(buf, v.v.b ? "true" : "false");
            break;
        case VAL_STRING:
        {
            const char* cs = lumyr_str_cstr(&v);
            size_t l = cs ? strlen(cs) : 0;
            char* p = (char*)malloc(l+1);
            if (cs) memcpy(p, cs, l+1); else p[0] = '\0';
            return p;
        }
        case VAL_ERROR:
            return strdup(v.v.err.message ? v.v.err.message : "");
        case VAL_BYTE:
            snprintf(buf, sizeof(buf), "%lld", v.v.i & 0xFF);
            break;
        case VAL_CHAR:
        {
            char buf2[2];
            buf2[0] = v.v.c;
            buf2[1] = '\0';
            size_t n = strlen(buf2);
            char* res = (char*)malloc(n+1);
            memcpy(res, buf2, n+1);
            return res;
        }
        case VAL_MAP:
            return lumyr_json_stringify(v);
        default:
            strcpy(buf, "");
            break;
    }
    size_t n = strlen(buf);
    char* res = (char*)malloc(n+1);
    memcpy(res, buf, n+1);
    return res;
}

Value lumyr_unary_plus(Value v) {
    return v;
}

Value lumyr_unary_minus(Value v) {
    if(v.type == VAL_INT) {
        if(v.v.i == LLONG_MIN) return lumyr_make_double(-(double)v.v.i);  // 溢出保护
        return lumyr_make_int(-v.v.i);
    }
    double num = value_as_number(v);
    return lumyr_make_double(-num);
}

Value lumyr_add(Value a, Value b) {
    // 与解释器 ast_interp.c 语义对齐：
    // 1) 任一操作数为 string 或 bool → 字符串拼接（bool 转 "true"/"false"）
    // 2) int+int → int
    // 3) 其它（含 char 参与）→ double（char 按数值提升）
    if(is_string(a,b) || a.type == VAL_BOOL || b.type == VAL_BOOL)
    {
        /* 性能优化：已是字符串的操作数直接引用其数据，不调用 value_to_str() 做
         * 多余的 malloc+strlen+memcpy+free（strdup）。仅非字符串操作数需要转换。 */
        const char *sa, *sb;
        char *sa_alloc = NULL, *sb_alloc = NULL;
        size_t la, lb;
        if (a.type == VAL_STRING) {
            sa = a.str_inline ? a.v.sso.data : a.v.s;
            la = a.str_inline ? (size_t)a.v.sso.len : (sa ? strlen(sa) : 0);
        } else {
            sa_alloc = value_to_str(a);
            sa = sa_alloc;
            la = strlen(sa);
        }
        if (b.type == VAL_STRING) {
            sb = b.str_inline ? b.v.sso.data : b.v.s;
            lb = b.str_inline ? (size_t)b.v.sso.len : (sb ? strlen(sb) : 0);
        } else {
            sb_alloc = value_to_str(b);
            sb = sb_alloc;
            lb = strlen(sb);
        }
        size_t total = la + lb;
        Value res;
        res.type = VAL_STRING;
        if (total <= LUMYR_SSO_MAX) {
            res.str_inline = 1;
            res.v.sso.len = (uint8_t)total;
            memcpy(res.v.sso.data, sa, la);
            memcpy(res.v.sso.data + la, sb, lb);
            res.v.sso.data[total] = '\0';
        } else {
            res.str_inline = 0;
            char* out = (char*)gc_alloc(total + 1, VAL_STRING);
            memcpy(out, sa, la);
            memcpy(out+la, sb, lb);
            out[total] = '\0';
            res.v.s = out;
        }
        if (sa_alloc) free(sa_alloc);
        if (sb_alloc) free(sb_alloc);
        return res;
    }
    if(a.type == VAL_INT && b.type == VAL_INT)
    {
        return lumyr_make_int(a.v.i + b.v.i);
    }
    // 算术加法
    double na = value_as_number(a);
    double nb = value_as_number(b);
    return lumyr_make_double(na + nb);
}

Value lumyr_sub(Value a, Value b) {
    if(a.type == VAL_INT && b.type == VAL_INT)
    {
        return lumyr_make_int(a.v.i - b.v.i);
    }
    double na = value_as_number(a);
    double nb = value_as_number(b);
    return lumyr_make_double(na - nb);
}

Value lumyr_mul(Value a, Value b) {
    if(a.type == VAL_INT && b.type == VAL_INT)
    {
        return lumyr_make_int(a.v.i * b.v.i);
    }
    double na = value_as_number(a);
    double nb = value_as_number(b);
    return lumyr_make_double(na * nb);
}

Value lumyr_div(Value a, Value b) {
    double na = value_as_number(a);
    double nb = value_as_number(b);
    return lumyr_make_double(na / nb);
}

// % 取模：int%int → int（C 语义，负数与 C 一致）；任一 double → fmod
Value lumyr_mod(Value a, Value b) {
    if(a.type == VAL_INT && b.type == VAL_INT) {
        if(b.v.i == 0) return lumyr_make_double(0.0 / 0.0);  // 除零得 NaN，避免 UB
        return lumyr_make_int(a.v.i % b.v.i);
    }
    double na = value_as_number(a);
    double nb = value_as_number(b);
    return lumyr_make_double(fmod(na, nb));
}

// ! 逻辑非：返回 bool
Value lumyr_logic_not(Value v) {
    return lumyr_make_bool(!lumyr_to_bool(v));
}

// ---------------- 数组 ----------------

// 下标必须是数值；越界运行时错误
long long array_index_of(Value idx) {
    if(idx.type == VAL_INT) return idx.v.i;
    if(idx.type == VAL_DOUBLE) return (long long)idx.v.d;
    if(idx.type == VAL_CHAR) return (unsigned char)idx.v.c;
    runtime_error("数组下标必须是数值");
    return 0;
}

Value lumyr_array_get(Value arr, Value idx) {
    if(arr.type != VAL_ARRAY) runtime_error("下标访问的对象不是数组");
    long long i = array_index_of(idx);
    if(i < 0 || i >= arr.v.array->len) {
        char buf[128];
        snprintf(buf, sizeof(buf), "数组下标越界: %lld (长度 %d)", i, arr.v.array->len);
        runtime_error(buf);
    }
    return arr.v.array->items[i];   // 返回数组持有值的引用（调用方如需长期持有需 clone）
}

// len(x)：数组长度 / 字符串字符数
Value lumyr_len(Value v) {
    if(v.type == VAL_ARRAY) return lumyr_make_int(v.v.array->len);
    if(v.type == VAL_TYPED_ARRAY) return lumyr_make_int(v.v.typed_array->len);
    if(v.type == VAL_STRING) {
        /* 已知是字符串，直接内联访问，跳过 lumyr_str_len 的冗余 type 检查 */
        int l = v.str_inline ? (int)v.v.sso.len : (int)(v.v.s ? strlen(v.v.s) : 0);
        return lumyr_make_int((long long)l);
    }
    if(v.type == VAL_MAP) return lumyr_make_int(v.v.map->len);
    runtime_error("len() 参数必须是数组、字符串或字典");
    return lumyr_make_int(0);
}

// 下标读：数组元素 / 字符串字符（返回 char） / 字典键
Value lumyr_index_get(Value c, Value idx) {
    if(c.type == VAL_MAP) {
        return lumyr_map_get(c, idx);
    }
    /* VAL_STRUCT_PTR / VAL_CLASS_PTR（C 结构体实例）：
       类型拆分后，class 实例使用 VAL_CLASS_PTR，struct 实例使用 VAL_STRUCT_PTR
       通过类型字段直接区分，调用对应的专门属性访问函数 */
    if(c.type == VAL_STRUCT_PTR || c.type == VAL_CLASS_PTR) {
        if(idx.type == VAL_STRING) {
            const char* idxcs = lumyr_str_cstr(&idx);
            if(c.type == VAL_CLASS_PTR) {
                return lumyr_class_get_field(c, idxcs);
            } else {
                return lumyr_struct_get_field(c, idxcs);
            }
        }
        runtime_error("结构体属性访问必须是字符串键");
        return val_none();
    }
    if(c.type == VAL_ERROR) {
        if(idx.type != VAL_STRING) runtime_error("错误对象下标必须是字符串键");
        const char* idxcs = lumyr_str_cstr(&idx);
        if(strcmp(idxcs, "type") == 0) return lumyr_make_string(c.v.err.type ? c.v.err.type : "");
        if(strcmp(idxcs, "message") == 0) return lumyr_make_string(c.v.err.message ? c.v.err.message : "");
        if(strcmp(idxcs, "stack") == 0) return lumyr_make_string(c.v.err.stack ? c.v.err.stack : "");
        runtime_error("错误对象只有 type/message/stack 三个字段");
        return val_none();
    }
    long long i = array_index_of(idx);
    if(c.type == VAL_TYPED_ARRAY) {
        /* 类型化数组：根据元素类型返回对应值 */
        if(i < 0 || i >= c.v.typed_array->len) {
            char buf[128];
            snprintf(buf, sizeof(buf), "数组下标越界: %lld (长度 %d)", i, c.v.typed_array->len);
            runtime_error(buf);
        }
        TypedArray* tarr = c.v.typed_array;
        switch(tarr->elem_type) {
            case VAL_INT:
                return lumyr_make_int(((int*)tarr->items)[i]);
            case VAL_BOOL:
                return lumyr_make_int((long long)((_Bool*)tarr->items)[i]);
            case VAL_CHAR:
                return lumyr_make_char(((char*)tarr->items)[i]);
            case VAL_BYTE:
                return lumyr_make_int((long long)((unsigned char*)tarr->items)[i]);
            case VAL_DOUBLE:
                return lumyr_make_double(((double*)tarr->items)[i]);
            case VAL_FLOAT:
                return lumyr_make_double((double)((float*)tarr->items)[i]);
            case VAL_UINT32:
                return lumyr_make_int((long long)((unsigned int*)tarr->items)[i]);
            case VAL_INT8:
                return lumyr_make_int8(((int8_t*)tarr->items)[i]);
            case VAL_INT16:
                return lumyr_make_int16(((int16_t*)tarr->items)[i]);
            case VAL_INT32:
                return lumyr_make_int32(((int32_t*)tarr->items)[i]);
            case VAL_INT64:
                return lumyr_make_int64(((int64_t*)tarr->items)[i]);
            case VAL_UINT8:
                return lumyr_make_uint8(((uint8_t*)tarr->items)[i]);
            case VAL_UINT16:
                return lumyr_make_uint16(((uint16_t*)tarr->items)[i]);
            case VAL_UINT64:
                return lumyr_make_uint64(((uint64_t*)tarr->items)[i]);
            case VAL_LONG:
                return lumyr_make_long(((long*)tarr->items)[i]);
            case VAL_ULONG:
                return lumyr_make_ulong(((unsigned long*)tarr->items)[i]);
            case VAL_SIZE_T:
                return lumyr_make_size_t(((size_t*)tarr->items)[i]);
            case VAL_SSIZE_T:
                return lumyr_make_ssize_t(((ssize_t*)tarr->items)[i]);
            case VAL_LONG_DOUBLE:
                return lumyr_make_long_double(((long double*)tarr->items)[i]);
            case VAL_STRING:
                return lumyr_make_string(((char**)tarr->items)[i]);
            default:
                runtime_error("类型化数组索引访问暂不支持该元素类型");
                return val_none();
        }
    }
    if(c.type == VAL_ARRAY) {
        if(i < 0 || i >= c.v.array->len) {
            char buf[128];
            snprintf(buf, sizeof(buf), "数组下标越界: %lld (长度 %d)", i, c.v.array->len);
            runtime_error(buf);
        }
        return c.v.array->items[i];
    }
    if(c.type == VAL_STRING) {
        const char* cs = lumyr_str_cstr(&c);
        long long n = cs ? (long long)strlen(cs) : 0;
        if(i < 0 || i >= n) {
            char buf[128];
            snprintf(buf, sizeof(buf), "字符串下标越界: %lld (长度 %lld)", i, n);
            runtime_error(buf);
        }
        return lumyr_make_char(cs ? cs[i] : '\0');
    }
    runtime_error("下标访问的对象不是数组、字符串或字典");
    return val_none();
}

// ---------------- 内置函数 ----------------

Value lumyr_type(Value v) {
    switch(v.type) {
        case VAL_NONE:   return lumyr_make_string("none");
        case VAL_INT:    return lumyr_make_string("int");
        case VAL_DOUBLE: return lumyr_make_string("double");
        case VAL_BOOL:   return lumyr_make_string("bool");
        case VAL_CHAR:   return lumyr_make_string("char");
        case VAL_BYTE:   return lumyr_make_string("byte");
        case VAL_STRING: return lumyr_make_string("string");
        case VAL_FUNC:   return lumyr_make_string("func");
        case VAL_ARRAY:  return lumyr_make_string("array");
        case VAL_MAP:    return lumyr_make_string("map");
        case VAL_ERROR:  return lumyr_make_string("error");
        case VAL_GENERATOR: return lumyr_make_string("generator");
        case VAL_STRUCT_PTR: return lumyr_make_string("struct");
        case VAL_CLASS_PTR: return lumyr_make_string("class");
        case VAL_TYPED_ARRAY: return lumyr_make_string("typed_array");
    }
    return lumyr_make_string("unknown");
}

Value lumyr_input(void) {
    /* 动态读取整行：初始 64 字节，按需翻倍，无长度上限 */
    size_t cap = 64, n = 0;
    char* buf = (char*)malloc(cap);
    if(!buf) { fprintf(stderr, "input: 内存不足\n"); exit(EXIT_FAILURE); }
    for(;;) {
        if(!fgets(buf + n, (int)(cap - n), stdin)) {
            if(n == 0) { free(buf); return lumyr_make_string(""); }
            break;
        }
        n = strlen(buf);
        if(n > 0 && buf[n - 1] == '\n') break;
        if(n < cap - 1) break;                 /* 正常读满前退出（EOF 无换行） */
        size_t nc = cap * 2;
        char* nb = (char*)realloc(buf, nc);
        if(!nb) { fprintf(stderr, "input: 内存不足\n"); exit(EXIT_FAILURE); }
        buf = nb; cap = nc;
    }
    while(n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    Value r = lumyr_make_string(buf);
    free(buf);
    return r;
}

// range() 参数转 long long（double 整数值截断，兼容旧行为）
long long range_to_ll(Value v) {
    if(v.type == VAL_DOUBLE) return (long long)v.v.d;
    if(v.type != VAL_INT) runtime_error("range() 参数必须是整数");
    return v.v.i;
}

// range(n) / range(a,b) / range(a,b,step)：生成等差数列数组
// 只读属性检查：type 构造对象的 __mapname__ 不可写/删（map 写路径统一拦截）
void lumyr_check_mapname_ro(Value arr, Value idx, const char* op)
{
    if(arr.type == VAL_MAP && idx.type == VAL_STRING && strcmp(lumyr_str_cstr(&idx), "__mapname__") == 0) {
        char b[96];
        snprintf(b, sizeof b, "只读属性 __mapname__ 不能%s", op);
        runtime_error(b);
    }
}

Value lumyr_array_set(Value arr, Value idx, Value val) {
    if(arr.type == VAL_MAP) { lumyr_check_mapname_ro(arr, idx, "赋值"); lumyr_map_set(&arr, idx, val); return val; }
    /* VAL_STRUCT_PTR（C 结构体实例，包括 class 和 struct）：
       自动判断是 class 还是 struct，调用对应的专门属性写入函数 */
    if(arr.type == VAL_STRUCT_PTR || arr.type == VAL_CLASS_PTR) {
        if(idx.type == VAL_STRING) {
            const char* idxcs = lumyr_str_cstr(&idx);
            if(lumyr_is_class_instance(arr)) {
                lumyr_class_set_field(arr, idxcs, val);
            } else {
                lumyr_struct_set_field(arr, idxcs, val);
            }
            return val;
        }
        runtime_error("结构体属性写入必须是字符串键");
        return val;
    }
    if(arr.type != VAL_ARRAY) runtime_error("下标访问的对象不是数组");
    long long i = array_index_of(idx);
    if(i < 0 || i >= arr.v.array->len) {
        char buf[128];
        snprintf(buf, sizeof(buf), "数组下标越界: %lld (长度 %d)", i, arr.v.array->len);
        runtime_error(buf);
    }
    Value* slot = &arr.v.array->items[i];
    gc_write_barrier(val);  /* 增量标记写屏障：新值引用白色堆对象时变灰入栈 */
    gc_remembered_set_check(arr, val);  /* 老年代容器引用新生代时加入 remembered set */
    *slot = val;
    return val;
}

// > 弱类型：任意一方为字符串 → 字典序strcmp；否则数值比较
Value lumyr_gt(Value a, Value b) {
    if (is_string(a,b)) {
        char *sa = value_to_str(a);
        char *sb = value_to_str(b);
        int r = strcmp(sa, sb);
        free(sa);
        free(sb);
        return lumyr_make_bool(r > 0);
    }
    double na = value_as_number(a);
    double nb = value_as_number(b);
    return lumyr_make_bool(na > nb);
}

Value lumyr_lt(Value a, Value b) {
    if (is_string(a,b)) {
        char *sa = value_to_str(a);
        char *sb = value_to_str(b);
        int r = strcmp(sa, sb);
        free(sa);
        free(sb);
        return lumyr_make_bool(r < 0);
    }
    double na = value_as_number(a);
    double nb = value_as_number(b);
    return lumyr_make_bool(na < nb);
}

Value lumyr_ge(Value a, Value b) {
    if (is_string(a,b)) {
        char *sa = value_to_str(a);
        char *sb = value_to_str(b);
        int r = strcmp(sa, sb);
        free(sa);
        free(sb);
        return lumyr_make_bool(r >= 0);
    }
    double na = value_as_number(a);
    double nb = value_as_number(b);
    return lumyr_make_bool(na >= nb);
}

Value lumyr_le(Value a, Value b) {
    if (is_string(a,b)) {
        char *sa = value_to_str(a);
        char *sb = value_to_str(b);
        int r = strcmp(sa, sb);
        free(sa);
        free(sb);
        return lumyr_make_bool(r <= 0);
    }
    double na = value_as_number(a);
    double nb = value_as_number(b);
    return lumyr_make_bool(na <= nb);
}

// == 弱相等：一边字符串，全部转字符串比较；两边字符串strcmp；其余数值比较
Value lumyr_eq(Value a, Value b) {
    if (a.type == VAL_STRING && b.type == VAL_STRING) {
        /* 已知两边都是字符串，直接内联访问，跳过 lumyr_str_cstr 的冗余 type 检查 */
        const char* sa = a.str_inline ? a.v.sso.data : a.v.s;
        const char* sb = b.str_inline ? b.v.sso.data : b.v.s;
        if (sa == NULL && sb == NULL) return lumyr_make_bool(1);
        if (sa == NULL || sb == NULL) return lumyr_make_bool(0);
        return lumyr_make_bool(strcmp(sa, sb) == 0);
    }
    if (is_string(a,b)) {
        char *sa = value_to_str(a);
        char *sb = value_to_str(b);
        int eq = (strcmp(sa, sb) == 0);
        free(sa);
        free(sb);
        return lumyr_make_bool(eq);
    }
    if(a.type == VAL_ERROR || b.type == VAL_ERROR) {
        if(a.type == VAL_ERROR && b.type == VAL_ERROR) {
            int tm = strcmp(a.v.err.type ? a.v.err.type : "", b.v.err.type ? b.v.err.type : "");
            int mm = strcmp(a.v.err.message ? a.v.err.message : "", b.v.err.message ? b.v.err.message : "");
            return lumyr_make_bool(tm == 0 && mm == 0);
        }
        const char* am = (a.type == VAL_ERROR) ? a.v.err.message : (a.type == VAL_STRING ? lumyr_str_cstr(&a) : NULL);
        const char* bm = (b.type == VAL_ERROR) ? b.v.err.message : (b.type == VAL_STRING ? lumyr_str_cstr(&b) : NULL);
        if(a.type == VAL_ERROR && b.type == VAL_MAP && lumyr_map_has(b, lumyr_make_string("message"))) {
            Value mv = lumyr_map_get(b, lumyr_make_string("message"));
            if(mv.type != VAL_STRING) return lumyr_make_bool(0);
            const char* tm = NULL;
            if(a.v.err.type) {
                if(lumyr_map_has(b, lumyr_make_string("type"))) {
                    Value tv = lumyr_map_get(b, lumyr_make_string("type"));
                    if(tv.type == VAL_STRING) tm = lumyr_str_cstr(&tv);
                }
                if(tm && strcmp(tm, a.v.err.type) != 0) return lumyr_make_bool(0);
            }
            return lumyr_make_bool(strcmp(a.v.err.message, lumyr_str_cstr(&mv)) == 0);
        }
        if(!am || !bm) return lumyr_make_bool(0);
        return lumyr_make_bool(strcmp(am, bm) == 0);
    }
    /* VAL_STRUCT_PTR 类型：class 按引用比较，struct 按字段比较（隔离） */
    if(a.type == VAL_STRUCT_PTR || b.type == VAL_STRUCT_PTR) {
        if((a.type != VAL_STRUCT_PTR && a.type != VAL_CLASS_PTR) || (b.type != VAL_STRUCT_PTR && b.type != VAL_CLASS_PTR)) return lumyr_make_bool(0);
        if(!a.v.struct_ptr || !b.v.struct_ptr) return lumyr_make_bool(a.v.struct_ptr == b.v.struct_ptr);
        /* class 是引用类型，按指针比较 */
        if(lumyr_is_class_instance(a) || lumyr_is_class_instance(b)) {
            return lumyr_make_bool(a.v.struct_ptr == b.v.struct_ptr);
        }
        /* struct 是值类型，按字段比较 */
        return lumyr_make_bool(lumyr_struct_eq(a, b));
    }
    if(a.type == VAL_MAP || b.type == VAL_MAP) {
        if(a.type != VAL_MAP || b.type != VAL_MAP) return lumyr_make_bool(0);
        if(a.v.map->len != b.v.map->len) return lumyr_make_bool(0);
        MapIter it; map_iter_init(&it, a.v.map);
        Value k, vv;
        while(map_iter_next(&it, &k, &vv)) {
            if(!lumyr_map_has(b, k)) return lumyr_make_bool(0);
            Value bv = lumyr_map_get(b, k);
            Value eq = lumyr_eq(vv, bv);
            if(!eq.v.b) return lumyr_make_bool(0);
        }
        return lumyr_make_bool(1);
    }
    /* 类型检查：类型不同且不都是数字类型时，直接返回 false。
     * 修复 0 == null 返回 true 的 bug：value_as_number(null) 返回 0.0，
     * 导致 0 和 null 都被当作 0.0 比较。
     * 数字类型：int, double, char, byte（bool 不参与数值相等比较，保持类型严格） */
    int a_is_num = (a.type == VAL_INT || a.type == VAL_DOUBLE || a.type == VAL_CHAR || a.type == VAL_BYTE);
    int b_is_num = (b.type == VAL_INT || b.type == VAL_DOUBLE || b.type == VAL_CHAR || b.type == VAL_BYTE);
    if(a.type != b.type && !(a_is_num && b_is_num)) {
        return lumyr_make_bool(0);
    }
    double na = value_as_number(a);
    double nb = value_as_number(b);
    return lumyr_make_bool(na == nb);
}

Value lumyr_ne(Value a, Value b) {
    Value eq = lumyr_eq(a,b);
    return lumyr_make_bool(!eq.v.b);
}

_Bool lumyr_to_bool(Value v) {
    switch(v.type)
    {
        case VAL_INT:    return v.v.i != 0;
        case VAL_DOUBLE: return v.v.d != 0.0;
        case VAL_BOOL:   return v.v.b;
        case VAL_CHAR:   return (unsigned char)v.v.c != 0;
        case VAL_BYTE:   return (v.v.i & 0xFF) != 0;
        case VAL_STRING: return v.v.s && v.v.s[0] != '\0';
        case VAL_ARRAY:  return v.v.array && v.v.array->len > 0;
        case VAL_MAP:    return v.v.map && v.v.map->len > 0;
        case VAL_NONE:   return 0;
        default: return 1;  // 其他类型（如函数、线程等）默认为 true
    }
}

// (char)v 强转，C风格静默截断
Value lumyr_cast_char(Value v) {
    char cv = 0;
    switch(v.type)
    {
        case VAL_INT:
            cv = (char)v.v.i;
            break;
        case VAL_DOUBLE:
            cv = (char)(long long)v.v.d;
            break;
        case VAL_BOOL:
            cv = v.v.b ? 1 : 0;
            break;
        case VAL_CHAR:
            cv = v.v.c;
            break;
        case VAL_STRING: {
            const char* cs = lumyr_str_cstr(&v);
            if(cs == NULL || cs[0] == '\0'){
                cv = '\0';
            }else{
                cv = cs[0];
            }
            break;
        }
case VAL_ARRAY: {
    Value r = val_array(v.v.array->len);
    for(int i = 0; i < v.v.array->len; i++)
        r.v.array->items[i] = lumyr_cast_char(v.v.array->items[i]);
    return r;
}
case VAL_MAP: {
    Value r = val_map();
    MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v))
            lumyr_map_set(&r, __k, lumyr_cast_char(__v));
    return r;
}
        default:
            runtime_error("(char) cast: unsupported type");
    }
    return lumyr_make_char(cv);
}

Value lumyr_cast_byte(Value v) {
    unsigned long long bv = 0;
    switch(v.type)
    {
        case VAL_INT:
            bv = (unsigned long long)v.v.i & 0xFFULL;
            break;
        case VAL_DOUBLE:
            bv = ((unsigned long long)(long long)v.v.d) & 0xFFULL;
            break;
        case VAL_BOOL:
            bv = v.v.b ? 1 : 0;
            break;
        case VAL_CHAR:
            bv = (unsigned char)v.v.c;
            break;
        case VAL_BYTE:
            bv = (unsigned long long)(v.v.i & 0xFF);
            break;
        case VAL_STRING:
            bv = (unsigned long long)atoll(lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "0") & 0xFFULL;
            break;
        case VAL_NONE:
            bv = 0;
            break;
case VAL_ARRAY: {
    Value r = val_array(v.v.array->len);
    for(int i = 0; i < v.v.array->len; i++)
        r.v.array->items[i] = lumyr_cast_byte(v.v.array->items[i]);
    return r;
}
case VAL_MAP: {
    Value r = val_map();
    MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v))
            lumyr_map_set(&r, __k, lumyr_cast_byte(__v));
    return r;
}
        default:
            runtime_error("(byte) cast: unsupported type");
            return val_none();
    }
    return lumyr_make_byte((unsigned char)bv);
}

// (ASCII)v：char ↔ int，0‑255范围校验
Value lumyr_cast_ascii(Value v) {
    if(v.type == VAL_ARRAY) {
        Value r = val_array(v.v.array->len);
        for(int i = 0; i < v.v.array->len; i++)
            r.v.array->items[i] = lumyr_cast_ascii(v.v.array->items[i]);
        return r;
    }
    if(v.type == VAL_MAP) {
        Value r = val_map();
        MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v))
            lumyr_map_set(&r, __k, lumyr_cast_ascii(__v));
        return r;
    }
    if(v.type == VAL_CHAR)
    {
        // char → int编码
        return lumyr_make_int((unsigned char)v.v.c);
    }
    else if(v.type == VAL_INT)
    {
        long long x = v.v.i;
        if(x < 0 || x > 255)
        {
            runtime_error("(ASCII) value out of range 0~255");
        }
        return lumyr_make_char((char)(unsigned char)x);
    }
    else
    {
        runtime_error("(ASCII) cast only accept char / integer");
    }
    return lumyr_make_int(0);
}

// (int)v 强转
Value lumyr_cast_int(Value v) {
    long long iv = 0;
    switch(v.type)
    {
        case VAL_INT:
            iv = v.v.i;
            break;
        case VAL_DOUBLE:
            iv = (long long)v.v.d;
            break;
        case VAL_BOOL:
            iv = v.v.b ? 1 : 0;
            break;
        case VAL_CHAR:
            iv = (unsigned char)v.v.c;
            break;
        case VAL_BYTE:
            iv = v.v.i & 0xFF;
            break;
        case VAL_STRING: {
            const char* t = lumyr_str_cstr(&v);
            if(!t) { iv = 0; break; }
            while(*t && isspace((unsigned char)*t)) t++;
            char* end = NULL;
            long long r = strtoll(t, &end, 10);
            if(end == t) runtime_error("(int) cast: 字符串无法转为整数");
            while(*end && isspace((unsigned char)*end)) end++;
            if(*end == '.') {
                double d = strtod(t, &end);
                while(*end && isspace((unsigned char)*end)) end++;
                if(*end != '\0') runtime_error("(int) cast: 字符串无法转为整数");
                iv = (long long)d;
            } else if(*end != '\0') {
                runtime_error("(int) cast: 字符串无法转为整数");
            } else {
                iv = r;
            }
            break;
        }
case VAL_ARRAY: {
    Value r = val_array(v.v.array->len);
    for(int i = 0; i < v.v.array->len; i++)
        r.v.array->items[i] = lumyr_cast_int(v.v.array->items[i]);
    return r;
}
case VAL_MAP: {
    Value r = val_map();
    MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v))
            lumyr_map_set(&r, __k, lumyr_cast_int(__v));
    return r;
}
        default:
            runtime_error("(int) cast: unsupported type");
    }
    return lumyr_make_int(iv);
}

// (double)v 强转
Value lumyr_cast_double(Value v) {
    double dv = 0.0;
    switch(v.type)
    {
        case VAL_INT:
            dv = (double)v.v.i;
            break;
        case VAL_DOUBLE:
            dv = v.v.d;
            break;
        case VAL_BOOL:
            dv = v.v.b ? 1.0 : 0.0;
            break;
        case VAL_CHAR:
            dv = (double)(unsigned char)v.v.c;
            break;
        case VAL_BYTE:
            dv = (double)(v.v.i & 0xFF);
            break;
        case VAL_STRING: {
            const char* t = lumyr_str_cstr(&v);
            if(!t) { dv = 0.0; break; }
            while(*t && isspace((unsigned char)*t)) t++;
            char* end = NULL;
            double d = strtod(t, &end);
            if(end == t) runtime_error("(double) cast: 字符串无法转为数字");
            while(*end && isspace((unsigned char)*end)) end++;
            if(*end != '\0') runtime_error("(double) cast: 字符串无法转为数字");
            dv = d;
            break;
        }
case VAL_ARRAY: {
    Value r = val_array(v.v.array->len);
    for(int i = 0; i < v.v.array->len; i++)
        r.v.array->items[i] = lumyr_cast_double(v.v.array->items[i]);
    return r;
}
case VAL_MAP: {
    Value r = val_map();
    MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v))
            lumyr_map_set(&r, __k, lumyr_cast_double(__v));
    return r;
}
        default:
            runtime_error("(double) cast: unsupported type");
    }
    return lumyr_make_double(dv);
}

// (bool)v 强转
Value lumyr_cast_bool(Value v) {
    if(v.type == VAL_ARRAY) {
        Value r = val_array(v.v.array->len);
        for(int i = 0; i < v.v.array->len; i++)
            r.v.array->items[i] = lumyr_cast_bool(v.v.array->items[i]);
        return r;
    }
    if(v.type == VAL_MAP) {
        Value r = val_map();
        MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v))
            lumyr_map_set(&r, __k, lumyr_cast_bool(__v));
        return r;
    }
    _Bool b = lumyr_to_bool(v);
    return lumyr_make_bool(b);
}

// (string)v 强转
Value lumyr_cast_string(Value v) {
    if(v.type == VAL_ARRAY) {
        Value r = val_array(v.v.array->len);
        for(int i = 0; i < v.v.array->len; i++) {
            Value __cv = lumyr_cast_string(v.v.array->items[i]);
            gc_write_barrier(__cv);
            r.v.array->items[i] = __cv;
        }
        return r;
    }
    if(v.type == VAL_MAP) {
        Value r = val_map();
        MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v))
            lumyr_map_set(&r, __k, lumyr_cast_string(__v));
        return r;
    }
    char *s = value_to_str(v);
    Value res = lumyr_make_string(s);  // 转为 gc_alloc 字符串（GC 要求所有 Value 字符串都是 gc_alloc）
    free(s);                            // value_to_str 返回普通 malloc，需释放
    return res;
}

int lumyr_extract_int(Value v) {
    Value iv = lumyr_cast_int(v);
    if (iv.type == VAL_INT) {
        return iv.v.i;
    }
    return 0;
}

void lumyr_print(Value v) {
    switch(v.type)
    {
        case VAL_INT:
            printf("%lld\n", v.v.i);
            break;
        case VAL_DOUBLE:
            printf("%g\n", v.v.d);
            break;
        case VAL_BOOL:
            printf("%s\n", v.v.b ? "true" : "false");
            break;
        case VAL_STRING:
            printf("%s\n", lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "(null)");
            break;
        case VAL_CHAR:
            printf("%c\n", v.v.c);
            break;
        case VAL_BYTE:
            printf("%lld\n", v.v.i & 0xFF);
            break;
        case VAL_NONE:
            printf("null\n");
            break;
        case VAL_FUNC:
            printf("<func>\n");
            break;
        case VAL_ARRAY:
            printf("<array>\n");
            break;
        case VAL_MAP: {
            char* js = lumyr_json_stringify(v);   /* 字典按 JSON 序列化打印 */
            printf("%s\n", js);
            free(js);
            break;
        }
        default:
            printf("<unknown>\n");
            break;
    }
}

/* 打印单个值不换行，用于多参数 print(a, b, c) */
void lumyr_print_inline(Value v) {
    switch(v.type)
    {
        case VAL_INT:
            printf("%lld", v.v.i);
            break;
        case VAL_DOUBLE:
            printf("%g", v.v.d);
            break;
        case VAL_BOOL:
            printf("%s", v.v.b ? "true" : "false");
            break;
        case VAL_STRING:
            printf("%s", lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "(null)");
            break;
        case VAL_CHAR:
            printf("%c", v.v.c);
            break;
        case VAL_BYTE:
            printf("%lld", v.v.i & 0xFF);
            break;
        case VAL_INT8:
            printf("%lld", v.v.i);
            break;
        case VAL_INT16:
            printf("%lld", v.v.i);
            break;
        case VAL_INT32:
            printf("%lld", v.v.i);
            break;
        case VAL_INT64:
            printf("%lld", v.v.i);
            break;
        case VAL_UINT8:
            printf("%u", (unsigned int)v.v.i);
            break;
        case VAL_UINT16:
            printf("%u", (unsigned int)v.v.i);
            break;
        case VAL_UINT32:
            printf("%u", (unsigned int)v.v.i);
            break;
        case VAL_UINT64:
            printf("%llu", (unsigned long long)v.v.i);
            break;
        case VAL_LONG:
            printf("%ld", (long)v.v.i);
            break;
        case VAL_ULONG:
            printf("%lu", (unsigned long)v.v.i);
            break;
        case VAL_SIZE_T:
            printf("%zu", (size_t)v.v.i);
            break;
        case VAL_SSIZE_T:
            printf("%zd", (ssize_t)v.v.i);
            break;
        case VAL_LONG_DOUBLE:
            printf("%Lf", (long double)v.v.d);
            break;
        case VAL_NONE:
            printf("null");
            break;
        case VAL_FUNC:
            printf("<func>");
            break;
        case VAL_ARRAY:
            printf("<array>");
            break;
        case VAL_MAP: {
            char* js = lumyr_json_stringify(v);
            printf("%s", js);
            free(js);
            break;
        }
        case VAL_STRUCT_PTR:
            printf("<struct_ptr>");
            break;
        case VAL_GENERATOR:
            printf("<generator>");
            break;
        default:
            printf("<unknown>");
            break;
    }
}

// toupper/tolower：ASCII 大小写转换（非 ASCII 保持）

// ===== 固定宽度整数强转（返回 VAL_INT，C 风格截断） =====
static long long value_to_ll(Value v) {
    switch(v.type) {
        case VAL_INT: case VAL_BYTE: return v.v.i;
        case VAL_DOUBLE: return (long long)v.v.d;
        case VAL_BOOL: return v.v.b ? 1 : 0;
        case VAL_CHAR: return (long long)(unsigned char)v.v.c;
        case VAL_STRING: return atoll(lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "0");
        case VAL_NONE: return 0;
        default: runtime_error("整数强转: 不支持的类型"); return 0;
    }
}
static unsigned long long value_to_ull(Value v) {
    switch(v.type) {
        case VAL_INT: case VAL_BYTE: return (unsigned long long)v.v.i;
        case VAL_DOUBLE: return (unsigned long long)v.v.d;
        case VAL_BOOL: return v.v.b ? 1ULL : 0ULL;
        case VAL_CHAR: return (unsigned long long)(unsigned char)v.v.c;
        case VAL_STRING: return strtoull(lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "0", NULL, 10);
        case VAL_NONE: return 0;
        default: runtime_error("整数强转: 不支持的类型"); return 0;
    }
}
static Value cast_int_width(Value v, int bits, int is_signed) {
    if(v.type == VAL_ARRAY) {
        Value r = val_array(v.v.array->len);
        for(int i = 0; i < v.v.array->len; i++)
            r.v.array->items[i] = cast_int_width(v.v.array->items[i], bits, is_signed);
        return r;
    }
    if(v.type == VAL_MAP) {
        Value r = val_map();
        MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v)) {
            lumyr_map_set(&r, __k,
                          cast_int_width(__v, bits, is_signed));
        }
        return r;
    }
    long long ll = value_to_ll(v);
    unsigned long long ull = value_to_ull(v);
    switch(bits) {
        case 8:  return lumyr_make_int(is_signed ? (long long)(int8_t)ll : (long long)(uint8_t)ull);
        case 16: return lumyr_make_int(is_signed ? (long long)(int16_t)ll : (long long)(uint16_t)ull);
        case 32: return lumyr_make_int(is_signed ? (long long)(int32_t)ll : (long long)(uint32_t)ull);
        case 64: return lumyr_make_int(is_signed ? (long long)(int64_t)ll : (long long)(uint64_t)ull);
    }
    return lumyr_make_int(ll);
}
Value lumyr_cast_int8(Value v)  { return cast_int_width(v, 8, 1); }
Value lumyr_cast_int16(Value v) { return cast_int_width(v, 16, 1); }
Value lumyr_cast_int32(Value v) { return cast_int_width(v, 32, 1); }
Value lumyr_cast_int64(Value v) { return cast_int_width(v, 64, 1); }
Value lumyr_cast_uint8(Value v)  { return cast_int_width(v, 8, 0); }
Value lumyr_cast_uint16(Value v) { return cast_int_width(v, 16, 0); }
Value lumyr_cast_uint32(Value v) { return cast_int_width(v, 32, 0); }
Value lumyr_cast_uint64(Value v) { return cast_int_width(v, 64, 0); }
Value lumyr_cast_long(Value v)     { return lumyr_cast_int(v); }  // long → 64 位
Value lumyr_cast_longlong(Value v) { return lumyr_cast_int(v); }  // long long → 64 位
// float：32 位单精度截断（运行时仍存 double）
static Value cast_float_rec(Value v) {
    if(v.type == VAL_ARRAY) {
        Value r = val_array(v.v.array->len);
        for(int i = 0; i < v.v.array->len; i++)
            r.v.array->items[i] = cast_float_rec(v.v.array->items[i]);
        return r;
    }
    if(v.type == VAL_MAP) {
        Value r = val_map();
        MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v)) {
            lumyr_map_set(&r, __k,
                          cast_float_rec(__v));
        }
        return r;
    }
    double d;
    switch(v.type) {
        case VAL_DOUBLE: d = v.v.d; break;
        case VAL_INT: case VAL_BYTE: d = (double)v.v.i; break;
        case VAL_BOOL: d = v.v.b ? 1.0 : 0.0; break;
        case VAL_CHAR: d = (double)(unsigned char)v.v.c; break;
        case VAL_STRING: d = atof(lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "0"); break;
        case VAL_NONE: d = 0.0; break;
        default: runtime_error("(float) 强转: 不支持的类型"); return val_none();
    }
    return lumyr_make_double((double)(float)d);
}
Value lumyr_cast_float(Value v) { return cast_float_rec(v); }
