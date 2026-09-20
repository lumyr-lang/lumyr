#include "lm_value.h"
#include "lm_json.h"
#include "lm_class.h"
#include "lm_struct.h"
#include "lm_bigint.h"
#include "lm_decimal.h"
#include "lm_bitdecimal.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>

// 字典辅助（VAL_MAP）前向声明：lumyr_eq 等在定义之前引用
Value lumyr_make_int(int i) {
    Value v;
    v.type = VAL_INT;
    v.v.i = i;  // 使用专用的i成员，32位有符号整数
    return v;
}

Value lumyr_make_long_long(long long ll) {
    Value v;
    v.type = VAL_LONG_LONG;
    v.v.ll = ll;  // 使用专用的ll成员，64位有符号整数
    return v;
}

Value lumyr_make_double(double d) {
    Value v;
    v.type = VAL_DOUBLE;
    v.v.d = d;
    return v;
}

Value lumyr_make_float(float f) {
    Value v;
    v.type = VAL_FLOAT;
    v.v.f = f;  // 使用专用的f成员，避免与double混用
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
    v.v.by = b;  // 专用by成员
    return v;
}

Value lumyr_make_int8(int8_t i8) {
    Value v;
    v.type = VAL_INT8;
    v.v.i8 = i8;  // 使用专用的i8成员，避免与long long混用
    return v;
}

Value lumyr_make_int16(int16_t i16) {
    Value v;
    v.type = VAL_INT16;
    v.v.i16 = i16;  // 使用专用的i16成员，避免与long long混用
    return v;
}

Value lumyr_make_short(int16_t s) {
    Value v;
    v.type = VAL_SHORT;
    v.v.sh = (short)s;  // 专用sh成员
    return v;
}

Value lumyr_make_int32(int32_t i32) {
    Value v;
    v.type = VAL_INT32;
    v.v.i32 = i32;  // 使用专用的i32成员，避免与long long混用
    return v;
}

Value lumyr_make_int64(int64_t i64) {
    Value v;
    v.type = VAL_INT64;
    v.v.i64 = i64;  // 专用i64成员
    return v;
}

Value lumyr_make_uint8(uint8_t u8) {
    Value v;
    v.type = VAL_UINT8;
    v.v.u8 = u8;  // 使用专用的u8成员，避免与long long混用
    return v;
}

Value lumyr_make_uchar(unsigned char uc) {
    Value v;
    v.type = VAL_UCHAR;
    v.v.uc = uc;  // 专用uc成员
    return v;
}

Value lumyr_make_uint16(uint16_t u16) {
    Value v;
    v.type = VAL_UINT16;
    v.v.u16 = u16;  // 使用专用的u16成员，避免与long long混用
    return v;
}

Value lumyr_make_ushort(unsigned short us) {
    Value v;
    v.type = VAL_USHORT;
    v.v.us = us;  // 专用us成员
    return v;
}

Value lumyr_make_uint32(uint32_t u32) {
    Value v;
    v.type = VAL_UINT32;
    v.v.u32 = u32;  // 使用专用的u32成员，避免与long long混用
    return v;
}

Value lumyr_make_uint(unsigned int ui) {
    Value v;
    v.type = VAL_UINT;
    v.v.ui = ui;  // 专用ui成员
    return v;
}

Value lumyr_make_uint64(uint64_t u64) {
    Value v;
    v.type = VAL_UINT64;
    v.v.u64 = u64;  // 使用专用的u64成员，避免与long long混用
    return v;
}

Value lumyr_make_long(long lv) {
    Value v;
    v.type = VAL_LONG;
    v.v.l = lv;  // 使用专用的l成员，避免与long long混用
    return v;
}

Value lumyr_make_ulong(unsigned long ulv) {
    Value v;
    v.type = VAL_ULONG;
    v.v.ul = ulv;  // 使用专用的ul成员，避免与long long混用
    return v;
}

Value lumyr_make_size_t(size_t stv) {
    Value v;
    v.type = VAL_SIZE_T;
    v.v.st = stv;  // 使用专用的st成员，避免与long long混用
    return v;
}

Value lumyr_make_ssize_t(ssize_t sstv) {
    Value v;
    v.type = VAL_SSIZE_T;
    v.v.sst = sstv;  // 使用专用的sst成员，避免与long long混用
    return v;
}

Value lumyr_make_long_double(long double ldv) {
    Value v;
    v.type = VAL_LONG_DOUBLE;
    v.v.ld = ldv;  // 使用专用的ld成员，避免与double混用
    return v;
}

// 获取value的数值，int转double
double value_as_number(Value x) {
    switch(x.type) {
        case VAL_INT:       return (double)x.v.i;
        case VAL_INT8:      return (double)x.v.i8;
        case VAL_INT16:     return (double)x.v.i16;
        case VAL_SHORT:     return (double)x.v.sh;
        case VAL_INT32:     return (double)x.v.i32;
        case VAL_INT64:     return (double)x.v.i64;
        case VAL_LONG_LONG: return (double)x.v.ll;
        case VAL_LONG:      return (double)x.v.l;
        case VAL_BYTE:      return (double)x.v.by;
        case VAL_UINT8:     return (double)x.v.u8;
        case VAL_UCHAR:     return (double)x.v.uc;
        case VAL_UINT16:    return (double)x.v.u16;
        case VAL_USHORT:    return (double)x.v.us;
        case VAL_UINT32:    return (double)x.v.u32;
        case VAL_UINT:      return (double)x.v.ui;
        case VAL_UINT64:    return (double)x.v.u64;
        case VAL_ULONG:     return (double)x.v.ul;
        case VAL_SIZE_T:    return (double)x.v.st;
        case VAL_SSIZE_T:   return (double)x.v.sst;
        case VAL_BOOL:      return x.v.b ? 1.0 : 0.0;
        case VAL_CHAR:      return (double)(unsigned char)x.v.c;
        case VAL_FLOAT:     return (double)x.v.f;
        case VAL_DOUBLE:    return x.v.d;
        case VAL_LONG_DOUBLE: return (double)x.v.ld;
        default:            return 0.0;
    }
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
        /* 有符号整数类型：每个类型独立 case，直接读对应字段 */
        case VAL_INT:          snprintf(buf, sizeof(buf), "%d", v.v.i); break;
        case VAL_INT8:         snprintf(buf, sizeof(buf), "%d", (int)v.v.i8); break;
        case VAL_INT16:        snprintf(buf, sizeof(buf), "%d", (int)v.v.i16); break;
        case VAL_SHORT:        snprintf(buf, sizeof(buf), "%d", (int)v.v.sh); break;
        case VAL_INT32:        snprintf(buf, sizeof(buf), "%d", (int)v.v.i32); break;
        case VAL_INT64:        snprintf(buf, sizeof(buf), "%lld", (long long)v.v.i64); break;
        case VAL_LONG_LONG:    snprintf(buf, sizeof(buf), "%lld", v.v.ll); break;
        case VAL_LONG:         snprintf(buf, sizeof(buf), "%ld", v.v.l); break;
        /* 无符号整数类型：每个类型独立 case */
        case VAL_BYTE:         snprintf(buf, sizeof(buf), "%u", (unsigned int)v.v.by); break;
        case VAL_UINT8:        snprintf(buf, sizeof(buf), "%u", (unsigned int)v.v.u8); break;
        case VAL_UCHAR:        snprintf(buf, sizeof(buf), "%u", (unsigned int)v.v.uc); break;
        case VAL_UINT16:       snprintf(buf, sizeof(buf), "%u", (unsigned int)v.v.u16); break;
        case VAL_USHORT:       snprintf(buf, sizeof(buf), "%u", (unsigned int)v.v.us); break;
        case VAL_UINT32:       snprintf(buf, sizeof(buf), "%u", (unsigned int)v.v.u32); break;
        case VAL_UINT:         snprintf(buf, sizeof(buf), "%u", (unsigned int)v.v.ui); break;
        case VAL_UINT64:       snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v.v.u64); break;
        case VAL_ULONG:        snprintf(buf, sizeof(buf), "%lu", v.v.ul); break;
        case VAL_SIZE_T:       snprintf(buf, sizeof(buf), "%zu", v.v.st); break;
        case VAL_SSIZE_T:      snprintf(buf, sizeof(buf), "%zd", v.v.sst); break;
        /* 浮点类型 */
        case VAL_FLOAT:        snprintf(buf, sizeof(buf), "%g", (double)v.v.f); break;
        case VAL_DOUBLE:       snprintf(buf, sizeof(buf), "%g", v.v.d); break;
        case VAL_LONG_DOUBLE:  snprintf(buf, sizeof(buf), "%Lg", v.v.ld); break;
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
        if(v.v.i == INT_MIN) return lumyr_make_double(-(double)v.v.i);  // 溢出保护，int类型用INT_MIN
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
    /* 所有整数/浮点/字符/布尔类型都可以作为下标，统一走 lumyr_extract_ll */
    if(idx.type == VAL_STRING) runtime_error("数组下标必须是数值，不能是字符串");
    return lumyr_extract_ll(idx);
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
            case VAL_INT8:
                return lumyr_make_int8(((int8_t*)tarr->items)[i]);
            case VAL_INT16:
                return lumyr_make_int16(((int16_t*)tarr->items)[i]);
            case VAL_SHORT:
                return lumyr_make_short(((short*)tarr->items)[i]);
            case VAL_INT32:
                return lumyr_make_int32(((int32_t*)tarr->items)[i]);
            case VAL_INT64:
                return lumyr_make_int64(((int64_t*)tarr->items)[i]);
            case VAL_LONG_LONG:
                return lumyr_make_long_long(((long long*)tarr->items)[i]);
            case VAL_LONG:
                return lumyr_make_long(((long*)tarr->items)[i]);
            case VAL_BYTE:
                return lumyr_make_byte(((unsigned char*)tarr->items)[i]);
            case VAL_UINT8:
                return lumyr_make_uint8(((uint8_t*)tarr->items)[i]);
            case VAL_UCHAR:
                return lumyr_make_uchar(((unsigned char*)tarr->items)[i]);
            case VAL_UINT16:
                return lumyr_make_uint16(((uint16_t*)tarr->items)[i]);
            case VAL_USHORT:
                return lumyr_make_ushort(((unsigned short*)tarr->items)[i]);
            case VAL_UINT32:
                return lumyr_make_uint32(((uint32_t*)tarr->items)[i]);
            case VAL_UINT:
                return lumyr_make_uint(((unsigned int*)tarr->items)[i]);
            case VAL_UINT64:
                return lumyr_make_uint64(((uint64_t*)tarr->items)[i]);
            case VAL_ULONG:
                return lumyr_make_ulong(((unsigned long*)tarr->items)[i]);
            case VAL_SIZE_T:
                return lumyr_make_size_t(((size_t*)tarr->items)[i]);
            case VAL_SSIZE_T:
                return lumyr_make_ssize_t(((ssize_t*)tarr->items)[i]);
            case VAL_FLOAT:
                return lumyr_make_float(((float*)tarr->items)[i]);
            case VAL_DOUBLE:
                return lumyr_make_double(((double*)tarr->items)[i]);
            case VAL_LONG_DOUBLE:
                return lumyr_make_long_double(((long double*)tarr->items)[i]);
            case VAL_BOOL:
                return lumyr_make_bool((_Bool)((_Bool*)tarr->items)[i]);
            case VAL_CHAR:
                return lumyr_make_char(((char*)tarr->items)[i]);
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
        // C类型（各类型专用，不混用）
        case VAL_VOID:     return lumyr_make_string("void");
        case VAL_INT8:     return lumyr_make_string("int8");
        case VAL_INT16:    return lumyr_make_string("int16");
        case VAL_INT32:    return lumyr_make_string("int32");
        case VAL_INT64:    return lumyr_make_string("int64");
        case VAL_LONG_LONG: return lumyr_make_string("long long");
        case VAL_LONG:     return lumyr_make_string("long");
        case VAL_UINT8:    return lumyr_make_string("uint8");
        case VAL_UINT16:   return lumyr_make_string("uint16");
        case VAL_UINT32:   return lumyr_make_string("uint32");
        case VAL_UINT64:   return lumyr_make_string("uint64");
        case VAL_ULONG:    return lumyr_make_string("unsigned long");
        case VAL_UCHAR:    return lumyr_make_string("unsigned char");
        case VAL_SHORT:    return lumyr_make_string("short");
        case VAL_USHORT:   return lumyr_make_string("unsigned short");
        case VAL_SIZE_T:   return lumyr_make_string("size_t");
        case VAL_SSIZE_T:  return lumyr_make_string("ssize_t");
        case VAL_FLOAT:    return lumyr_make_string("float");
        case VAL_LONG_DOUBLE: return lumyr_make_string("long double");
        case VAL_PTR:      return lumyr_make_string("pointer");
        case VAL_CALLBACK: return lumyr_make_string("callback");
        case VAL_UINT:     return lumyr_make_string("uint");
        case VAL_BIGINT:   return lumyr_make_string("bigint");
        case VAL_DECIMAL:  return lumyr_make_string("decimal");
        case VAL_BITDECIMAL: return lumyr_make_string("bitdecimal");
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
    /* 支持类型化数组（VAL_TYPED_ARRAY）：直接写入对应类型的元素，零转换开销 */
    if(arr.type == VAL_TYPED_ARRAY && arr.v.typed_array) {
        TypedArray* tarr = arr.v.typed_array;
        long long i = array_index_of(idx);
        if(i < 0 || i >= tarr->len) {
            char buf[128];
            snprintf(buf, sizeof(buf), "类型化数组下标越界: %lld (长度 %d)", i, tarr->len);
            runtime_error(buf);
        }
        /* 根据元素类型一对一写入，类型匹配时零转换开销 */
        switch(tarr->elem_type) {
            case VAL_INT:
                ((int*)tarr->items)[i] = (val.type == VAL_INT) ? val.v.i : (int)lumyr_extract_ll(val);
                break;
            case VAL_INT8:
                ((int8_t*)tarr->items)[i] = (val.type == VAL_INT8) ? val.v.i8 : (int8_t)lumyr_extract_ll(val);
                break;
            case VAL_INT16:
                ((int16_t*)tarr->items)[i] = (val.type == VAL_INT16) ? val.v.i16 : (int16_t)lumyr_extract_ll(val);
                break;
            case VAL_SHORT:
                ((short*)tarr->items)[i] = (val.type == VAL_SHORT) ? val.v.sh : (short)lumyr_extract_ll(val);
                break;
            case VAL_INT32:
                ((int32_t*)tarr->items)[i] = (val.type == VAL_INT32) ? val.v.i32 : (int32_t)lumyr_extract_ll(val);
                break;
            case VAL_INT64:
                ((int64_t*)tarr->items)[i] = (val.type == VAL_INT64) ? val.v.i64 : (int64_t)lumyr_extract_ll(val);
                break;
            case VAL_LONG_LONG:
                ((long long*)tarr->items)[i] = (val.type == VAL_LONG_LONG) ? val.v.ll : lumyr_extract_ll(val);
                break;
            case VAL_LONG:
                ((long*)tarr->items)[i] = (val.type == VAL_LONG) ? val.v.l : (long)lumyr_extract_ll(val);
                break;
            case VAL_BYTE:
                ((unsigned char*)tarr->items)[i] = (val.type == VAL_BYTE) ? val.v.by : (unsigned char)lumyr_extract_byte(val);
                break;
            case VAL_UINT8:
                ((uint8_t*)tarr->items)[i] = (val.type == VAL_UINT8) ? val.v.u8 : (uint8_t)lumyr_extract_ll(val);
                break;
            case VAL_UCHAR:
                ((unsigned char*)tarr->items)[i] = (val.type == VAL_UCHAR) ? val.v.uc : (unsigned char)lumyr_extract_ll(val);
                break;
            case VAL_UINT16:
                ((uint16_t*)tarr->items)[i] = (val.type == VAL_UINT16) ? val.v.u16 : (uint16_t)lumyr_extract_ll(val);
                break;
            case VAL_USHORT:
                ((unsigned short*)tarr->items)[i] = (val.type == VAL_USHORT) ? val.v.us : (unsigned short)lumyr_extract_ll(val);
                break;
            case VAL_UINT32:
                ((uint32_t*)tarr->items)[i] = (val.type == VAL_UINT32) ? val.v.u32 : (uint32_t)lumyr_extract_ll(val);
                break;
            case VAL_UINT:
                ((unsigned int*)tarr->items)[i] = (val.type == VAL_UINT) ? val.v.ui : (unsigned int)lumyr_extract_ll(val);
                break;
            case VAL_UINT64:
                ((uint64_t*)tarr->items)[i] = (val.type == VAL_UINT64) ? val.v.u64 : (uint64_t)lumyr_extract_ll(val);
                break;
            case VAL_ULONG:
                ((unsigned long*)tarr->items)[i] = (val.type == VAL_ULONG) ? val.v.ul : (unsigned long)lumyr_extract_ll(val);
                break;
            case VAL_SIZE_T:
                ((size_t*)tarr->items)[i] = (val.type == VAL_SIZE_T) ? val.v.st : (size_t)lumyr_extract_ll(val);
                break;
            case VAL_SSIZE_T:
                ((ssize_t*)tarr->items)[i] = (val.type == VAL_SSIZE_T) ? val.v.sst : (ssize_t)lumyr_extract_ll(val);
                break;
            case VAL_FLOAT:
                ((float*)tarr->items)[i] = (val.type == VAL_FLOAT) ? val.v.f : lumyr_extract_float(val);
                break;
            case VAL_DOUBLE:
                ((double*)tarr->items)[i] = (val.type == VAL_DOUBLE) ? val.v.d : lumyr_extract_double(val);
                break;
            case VAL_LONG_DOUBLE:
                ((long double*)tarr->items)[i] = (val.type == VAL_LONG_DOUBLE) ? val.v.ld : (long double)value_as_number(val);
                break;
            case VAL_BOOL:
                ((_Bool*)tarr->items)[i] = (val.type == VAL_BOOL) ? val.v.b : lumyr_extract_bool(val);
                break;
            case VAL_CHAR:
                ((char*)tarr->items)[i] = (val.type == VAL_CHAR) ? val.v.c : lumyr_extract_char(val);
                break;
            default:
                runtime_error("类型化数组元素赋值：不支持的元素类型");
                break;
        }
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
/* 数值比较辅助：同类型直接读字段，跨类型用统一转换 */
static int value_compare(Value a, Value b) {
    if(a.type == b.type) {
        /* 同类型：直接读对应字段比较，零转换开销 */
        switch(a.type) {
            case VAL_INT:          return (a.v.i > b.v.i) - (a.v.i < b.v.i);
            case VAL_INT8:         return (a.v.i8 > b.v.i8) - (a.v.i8 < b.v.i8);
            case VAL_INT16:        return (a.v.i16 > b.v.i16) - (a.v.i16 < b.v.i16);
            case VAL_SHORT:        return (a.v.sh > b.v.sh) - (a.v.sh < b.v.sh);
            case VAL_INT32:        return (a.v.i32 > b.v.i32) - (a.v.i32 < b.v.i32);
            case VAL_INT64:        return (a.v.i64 > b.v.i64) - (a.v.i64 < b.v.i64);
            case VAL_LONG_LONG:    return (a.v.ll > b.v.ll) - (a.v.ll < b.v.ll);
            case VAL_LONG:         return (a.v.l > b.v.l) - (a.v.l < b.v.l);
            case VAL_BYTE:         return (a.v.by > b.v.by) - (a.v.by < b.v.by);
            case VAL_UINT8:        return (a.v.u8 > b.v.u8) - (a.v.u8 < b.v.u8);
            case VAL_UCHAR:        return (a.v.uc > b.v.uc) - (a.v.uc < b.v.uc);
            case VAL_UINT16:       return (a.v.u16 > b.v.u16) - (a.v.u16 < b.v.u16);
            case VAL_USHORT:       return (a.v.us > b.v.us) - (a.v.us < b.v.us);
            case VAL_UINT32:       return (a.v.u32 > b.v.u32) - (a.v.u32 < b.v.u32);
            case VAL_UINT:         return (a.v.ui > b.v.ui) - (a.v.ui < b.v.ui);
            case VAL_UINT64:       return (a.v.u64 > b.v.u64) - (a.v.u64 < b.v.u64);
            case VAL_ULONG:        return (a.v.ul > b.v.ul) - (a.v.ul < b.v.ul);
            case VAL_SIZE_T:       return (a.v.st > b.v.st) - (a.v.st < b.v.st);
            case VAL_SSIZE_T:      return (a.v.sst > b.v.sst) - (a.v.sst < b.v.sst);
            case VAL_FLOAT:        return (a.v.f > b.v.f) - (a.v.f < b.v.f);
            case VAL_DOUBLE:       return (a.v.d > b.v.d) - (a.v.d < b.v.d);
            case VAL_LONG_DOUBLE:  return (a.v.ld > b.v.ld) - (a.v.ld < b.v.ld);
            case VAL_BOOL:         return (a.v.b > b.v.b) - (a.v.b < b.v.b);
            case VAL_CHAR:         return ((unsigned char)a.v.c > (unsigned char)b.v.c) -
                                         ((unsigned char)a.v.c < (unsigned char)b.v.c);
            default: break;
        }
    }
    /* 跨类型：用统一转换 */
    double na = value_as_number(a);
    double nb = value_as_number(b);
    return (na > nb) - (na < nb);
}

Value lumyr_gt(Value a, Value b) {
    if (is_string(a,b)) {
        char *sa = value_to_str(a);
        char *sb = value_to_str(b);
        int r = strcmp(sa, sb);
        free(sa);
        free(sb);
        return lumyr_make_bool(r > 0);
    }
    return lumyr_make_bool(value_compare(a, b) > 0);
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
    return lumyr_make_bool(value_compare(a, b) < 0);
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
    return lumyr_make_bool(value_compare(a, b) >= 0);
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
    /* 类型检查：类型不同且不都是数值类型时，直接返回 false */
    int a_is_num = (a.type >= VAL_INT && a.type <= VAL_LONG_DOUBLE);
    int b_is_num = (b.type >= VAL_INT && b.type <= VAL_LONG_DOUBLE);
    if(a.type != b.type && !(a_is_num && b_is_num)) {
        return lumyr_make_bool(0);
    }
    return lumyr_make_bool(value_compare(a, b) == 0);
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
        case VAL_BYTE:   return v.v.by != 0;
        case VAL_STRING: return v.v.s && v.v.s[0] != '\0';
        case VAL_ARRAY:  return v.v.array && v.v.array->len > 0;
        case VAL_MAP:    return v.v.map && v.v.map->len > 0;
        case VAL_NONE:   return 0;
        default: return 1;  // 其他类型（如函数、线程等）默认为 true
    }
}

// (char)v 强转，C风格静默截断
/* 前向声明：标量值转整数（见下方强转部分） */
static long long value_to_ll(Value v);
static unsigned long long value_to_ull(Value v);

Value lumyr_cast_char(Value v) {
    /* 数组/map 递归处理每个元素 */
    if(v.type == VAL_ARRAY) {
        Value r = val_array(v.v.array->len);
        for(int i = 0; i < v.v.array->len; i++)
            r.v.array->items[i] = lumyr_cast_char(v.v.array->items[i]);
        return r;
    }
    if(v.type == VAL_MAP) {
        Value r = val_map();
        MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v))
            lumyr_map_set(&r, __k, lumyr_cast_char(__v));
        return r;
    }
    /* 字符串取首字符 */
    if(v.type == VAL_STRING) {
        const char* cs = lumyr_str_cstr(&v);
        char cv = (cs == NULL || cs[0] == '\0') ? '\0' : cs[0];
        return lumyr_make_char(cv);
    }
    /* 其他标量：统一走 value_to_ll，截断为 char */
    long long iv = value_to_ll(v);
    return lumyr_make_char((char)iv);
}

Value lumyr_cast_byte(Value v) {
    /* 数组/map 递归处理每个元素 */
    if(v.type == VAL_ARRAY) {
        Value r = val_array(v.v.array->len);
        for(int i = 0; i < v.v.array->len; i++)
            r.v.array->items[i] = lumyr_cast_byte(v.v.array->items[i]);
        return r;
    }
    if(v.type == VAL_MAP) {
        Value r = val_map();
        MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v))
            lumyr_map_set(&r, __k, lumyr_cast_byte(__v));
        return r;
    }
    /* 字符串解析为数字后截断为 byte */
    if(v.type == VAL_STRING) {
        unsigned long long bv = (unsigned long long)atoll(lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "0") & 0xFFULL;
        return lumyr_make_byte((unsigned char)bv);
    }
    /* 其他标量：统一走 value_to_ull，截断为 byte */
    unsigned long long bv = value_to_ull(v);
    return lumyr_make_byte((unsigned char)(bv & 0xFFULL));
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
    /* 数组/map 递归处理每个元素 */
    if(v.type == VAL_ARRAY) {
        Value r = val_array(v.v.array->len);
        for(int i = 0; i < v.v.array->len; i++)
            r.v.array->items[i] = lumyr_cast_int(v.v.array->items[i]);
        return r;
    }
    if(v.type == VAL_MAP) {
        Value r = val_map();
        MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v))
            lumyr_map_set(&r, __k, lumyr_cast_int(__v));
        return r;
    }
    /* 标量类型：统一走 value_to_ll（已补全所有类型） */
    long long iv = value_to_ll(v);
    return lumyr_make_int((int)iv);
}

// (double)v 强转
Value lumyr_cast_double(Value v) {
    /* 数组/map 递归处理每个元素 */
    if(v.type == VAL_ARRAY) {
        Value r = val_array(v.v.array->len);
        for(int i = 0; i < v.v.array->len; i++)
            r.v.array->items[i] = lumyr_cast_double(v.v.array->items[i]);
        return r;
    }
    if(v.type == VAL_MAP) {
        Value r = val_map();
        MapIter it; map_iter_init(&it, v.v.map);
        Value __k, __v;
        while(map_iter_next(&it, &__k, &__v))
            lumyr_map_set(&r, __k, lumyr_cast_double(__v));
        return r;
    }
    /* 字符串单独处理（value_as_number 不处理字符串） */
    if(v.type == VAL_STRING) {
        const char* t = lumyr_str_cstr(&v);
        if(!t) return lumyr_make_double(0.0);
        while(*t && isspace((unsigned char)*t)) t++;
        char* end = NULL;
        double d = strtod(t, &end);
        if(end == t) runtime_error("(double) cast: 字符串无法转为数字");
        while(*end && isspace((unsigned char)*end)) end++;
        if(*end != '\0') runtime_error("(double) cast: 字符串无法转为数字");
        return lumyr_make_double(d);
    }
    /* 其他标量类型：统一走 value_as_number（已补全所有类型） */
    double dv = value_as_number(v);
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

long long lumyr_extract_long_long(Value v) {
    Value llv = lumyr_cast_longlong(v);
    if (llv.type == VAL_LONG_LONG) {
        return llv.v.ll;
    }
    return 0;
}

double lumyr_extract_double(Value v) {
    Value dv = lumyr_cast_double(v);
    if (dv.type == VAL_DOUBLE) {
        return dv.v.d;
    }
    return 0.0;
}

float lumyr_extract_float(Value v) {
    Value fv = lumyr_cast_float(v);
    if (fv.type == VAL_FLOAT) {
        return fv.v.f;  // 使用专用的f成员，避免与double混用
    }
    return 0.0f;
}

_Bool lumyr_extract_bool(Value v) {
    return lumyr_to_bool(v);
}

char lumyr_extract_char(Value v) {
    Value cv = lumyr_cast_char(v);
    if (cv.type == VAL_CHAR) {
        return cv.v.c;
    }
    return 0;
}

unsigned char lumyr_extract_byte(Value v) {
    Value bv = lumyr_cast_byte(v);
    if (bv.type == VAL_BYTE) {
        return bv.v.u8;  // 使用专用的u8成员，避免与long long混用
    }
    return 0;
}

uint32_t lumyr_extract_uint32(Value v) {
    Value uv = lumyr_cast_uint32(v);
    if (uv.type == VAL_UINT32) {
        return uv.v.u32;  // 使用专用的u32成员，避免与long long混用
    }
    return 0;
}

/* 公共辅助函数：根据value的类型提取整数值，各数据类型专用
   用于替代各个文件中重复的extract_ll辅助函数 */
long long lumyr_extract_ll(Value v) {
    switch(v.type) {
        case VAL_INT:     return v.v.i;
        case VAL_INT8:    return (long long)v.v.i8;
        case VAL_INT16:   return (long long)v.v.i16;
        case VAL_SHORT:   return (long long)v.v.sh;
        case VAL_INT32:   return (long long)v.v.i32;
        case VAL_INT64:   return (long long)v.v.i64;
        case VAL_LONG_LONG: return v.v.ll;
        case VAL_LONG:    return (long long)v.v.l;
        case VAL_BYTE:    return (long long)v.v.by;
        case VAL_UINT8:   return (long long)v.v.u8;
        case VAL_UCHAR:   return (long long)v.v.uc;
        case VAL_UINT16:  return (long long)v.v.u16;
        case VAL_USHORT:  return (long long)v.v.us;
        case VAL_UINT32:  return (long long)v.v.u32;
        case VAL_UINT:    return (long long)v.v.ui;
        case VAL_UINT64:  return (long long)v.v.u64;
        case VAL_ULONG:   return (long long)v.v.ul;
        case VAL_SIZE_T:  return (long long)v.v.st;
        case VAL_SSIZE_T: return (long long)v.v.sst;
        case VAL_BOOL:    return v.v.b ? 1 : 0;
        case VAL_CHAR:    return (long long)(unsigned char)v.v.c;
        case VAL_FLOAT:   return (long long)v.v.f;
        case VAL_DOUBLE:  return (long long)v.v.d;
        case VAL_LONG_DOUBLE: return (long long)v.v.ld;
        default:          return 0;
    }
}

void lumyr_print(Value v) {
    switch(v.type) {
        case VAL_INT:         printf("%d\n", v.v.i); break;
        case VAL_INT8:        printf("%d\n", (int)v.v.i8); break;
        case VAL_INT16:       printf("%d\n", (int)v.v.i16); break;
        case VAL_SHORT:       printf("%d\n", (int)v.v.sh); break;
        case VAL_INT32:       printf("%d\n", (int)v.v.i32); break;
        case VAL_INT64:       printf("%lld\n", (long long)v.v.i64); break;
        case VAL_LONG_LONG:   printf("%lld\n", v.v.ll); break;
        case VAL_LONG:        printf("%ld\n", v.v.l); break;
        case VAL_BYTE:        printf("%u\n", (unsigned int)v.v.by); break;
        case VAL_UINT8:       printf("%u\n", (unsigned int)v.v.u8); break;
        case VAL_UCHAR:       printf("%u\n", (unsigned int)v.v.uc); break;
        case VAL_UINT16:      printf("%u\n", (unsigned int)v.v.u16); break;
        case VAL_USHORT:      printf("%u\n", (unsigned int)v.v.us); break;
        case VAL_UINT32:      printf("%u\n", (unsigned int)v.v.u32); break;
        case VAL_UINT:        printf("%u\n", (unsigned int)v.v.ui); break;
        case VAL_UINT64:      printf("%llu\n", (unsigned long long)v.v.u64); break;
        case VAL_ULONG:       printf("%lu\n", v.v.ul); break;
        case VAL_SIZE_T:      printf("%zu\n", v.v.st); break;
        case VAL_SSIZE_T:     printf("%zd\n", v.v.sst); break;
        case VAL_FLOAT:       printf("%g\n", (double)v.v.f); break;
        case VAL_DOUBLE:      printf("%g\n", v.v.d); break;
        case VAL_LONG_DOUBLE: printf("%Lg\n", v.v.ld); break;
        case VAL_BOOL:        printf("%s\n", v.v.b ? "true" : "false"); break;
        case VAL_CHAR:        printf("%c\n", v.v.c); break;
        case VAL_STRING:      printf("%s\n", lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "(null)"); break;
        case VAL_NONE:        printf("null\n"); break;
        case VAL_VOID:        printf("void\n"); break;
        case VAL_FUNC:        printf("<func>\n"); break;
        case VAL_ARRAY:       printf("<array>\n"); break;
        case VAL_TYPED_ARRAY: printf("<typed_array>\n"); break;
        case VAL_MAP: {
            char* js = lumyr_json_stringify(v);
            printf("%s\n", js);
            free(js);
            break;
        }
        case VAL_ERROR:       printf("<error>\n"); break;
        case VAL_STRUCT_PTR:  printf("<struct_ptr>\n"); break;
        case VAL_CLASS_PTR:   printf("<class_ptr>\n"); break;
        case VAL_GENERATOR:   printf("<generator>\n"); break;
        case VAL_PTR:         printf("%p\n", (void*)(intptr_t)v.v.ll); break;
        case VAL_CALLBACK:    printf("<callback>\n"); break;
        case VAL_BIGINT: {
            char* s = lumyr_bigint_to_string(v.v.bigint);
            printf("%s\n", s ? s : "(null)");
            free(s);
            break;
        }
        case VAL_DECIMAL: {
            char* s = lumyr_decimal_to_string(v.v.decimal);
            printf("%s\n", s ? s : "(null)");
            free(s);
            break;
        }
        case VAL_BITDECIMAL: {
            char* s = lumyr_bitdecimal_to_string(v.v.bitdecimal);
            printf("%s\n", s ? s : "(null)");
            free(s);
            break;
        }
        default:              printf("<unknown>\n"); break;
    }
}

/* 打印单个值不换行，用于多参数 print(a, b, c) */
void lumyr_print_inline(Value v) {
    switch(v.type)
    {
        case VAL_INT:
            printf("%d", v.v.i);  // int类型用%d格式符
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
            printf("%u", (unsigned int)v.v.by);  // 专用by成员
            break;
        case VAL_INT8:
            printf("%d", (int)v.v.i8);  // 使用专用的i8成员
            break;
        case VAL_INT16:
            printf("%d", (int)v.v.i16);  // 使用专用的i16成员
            break;
        case VAL_SHORT:
            printf("%d", (int)v.v.sh);  // 专用sh成员
            break;
        case VAL_INT32:
            printf("%d", (int)v.v.i32);  // 使用专用的i32成员
            break;
        case VAL_INT64:
            printf("%lld", (long long)v.v.i64);  // 专用i64成员
            break;
        case VAL_LONG_LONG:
            printf("%lld", v.v.ll);
            break;
        case VAL_UINT8:
            printf("%u", (unsigned int)v.v.u8);  // 使用专用的u8成员
            break;
        case VAL_UCHAR:
            printf("%u", (unsigned int)v.v.uc);  // 专用uc成员
            break;
        case VAL_UINT16:
            printf("%u", (unsigned int)v.v.u16);  // 使用专用的u16成员
            break;
        case VAL_USHORT:
            printf("%u", (unsigned int)v.v.us);  // 专用us成员
            break;
        case VAL_UINT32:
            printf("%u", (unsigned int)v.v.u32);  // 使用专用的u32成员
            break;
        case VAL_UINT:
            printf("%u", (unsigned int)v.v.ui);  // 专用ui成员
            break;
        case VAL_UINT64:
            printf("%llu", (unsigned long long)v.v.u64);  // 使用专用的u64成员
            break;
        case VAL_FLOAT:
            printf("%g", (double)v.v.f);
            break;
        case VAL_LONG:
            printf("%ld", v.v.l);  // 使用专用的l成员
            break;
        case VAL_ULONG:
            printf("%lu", v.v.ul);  // 使用专用的ul成员
            break;
        case VAL_SIZE_T:
            printf("%zu", v.v.st);  // 使用专用的st成员
            break;
        case VAL_SSIZE_T:
            printf("%zd", v.v.sst);  // 使用专用的sst成员
            break;
        case VAL_LONG_DOUBLE:
            printf("%Lg", v.v.ld);  // 使用专用的ld成员
            break;
        case VAL_ERROR:
            printf("<error>");
            break;
        case VAL_STRUCT_PTR:
            printf("<struct_ptr>");
            break;
        case VAL_CLASS_PTR:
            printf("<class_ptr>");
            break;
        case VAL_TYPED_ARRAY:
            printf("<typed_array>");
            break;
        case VAL_GENERATOR:
            printf("<generator>");
            break;
        case VAL_PTR:
            printf("%p", (void*)(intptr_t)v.v.ll);
            break;
        case VAL_CALLBACK:
            printf("<callback>");
            break;
        case VAL_VOID:
            printf("void");
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
        default:
            printf("<unknown>");
            break;
    }
}

// toupper/tolower：ASCII 大小写转换（非 ASCII 保持）

// ===== 固定宽度整数强转（返回 VAL_INT，C 风格截断） =====
static long long value_to_ll(Value v) {
    switch(v.type) {
        case VAL_INT:          return v.v.i;
        case VAL_INT8:         return (long long)v.v.i8;
        case VAL_INT16:        return (long long)v.v.i16;
        case VAL_SHORT:        return (long long)v.v.sh;
        case VAL_INT32:        return (long long)v.v.i32;
        case VAL_INT64:        return (long long)v.v.i64;
        case VAL_LONG_LONG:    return v.v.ll;
        case VAL_LONG:         return (long long)v.v.l;
        case VAL_BYTE:         return (long long)v.v.by;
        case VAL_UINT8:        return (long long)v.v.u8;
        case VAL_UCHAR:        return (long long)v.v.uc;
        case VAL_UINT16:       return (long long)v.v.u16;
        case VAL_USHORT:       return (long long)v.v.us;
        case VAL_UINT32:       return (long long)v.v.u32;
        case VAL_UINT:         return (long long)v.v.ui;
        case VAL_UINT64:       return (long long)v.v.u64;
        case VAL_ULONG:        return (long long)v.v.ul;
        case VAL_SIZE_T:       return (long long)v.v.st;
        case VAL_SSIZE_T:      return (long long)v.v.sst;
        case VAL_FLOAT:        return (long long)v.v.f;
        case VAL_DOUBLE:       return (long long)v.v.d;
        case VAL_LONG_DOUBLE:  return (long long)v.v.ld;
        case VAL_BOOL:         return v.v.b ? 1 : 0;
        case VAL_CHAR:         return (long long)(unsigned char)v.v.c;
        case VAL_STRING:       return atoll(lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "0");
        case VAL_NONE:         return 0;
        default: runtime_error("整数强转: 不支持的类型"); return 0;
    }
}
static unsigned long long value_to_ull(Value v) {
    switch(v.type) {
        case VAL_INT:          return (unsigned long long)v.v.i;
        case VAL_INT8:         return (unsigned long long)(uint8_t)v.v.i8;
        case VAL_INT16:        return (unsigned long long)(uint16_t)v.v.i16;
        case VAL_SHORT:        return (unsigned long long)(uint16_t)v.v.sh;
        case VAL_INT32:        return (unsigned long long)(uint32_t)v.v.i32;
        case VAL_INT64:        return (unsigned long long)v.v.i64;
        case VAL_LONG_LONG:    return (unsigned long long)v.v.ll;
        case VAL_LONG:         return (unsigned long long)v.v.l;
        case VAL_BYTE:         return (unsigned long long)v.v.by;
        case VAL_UINT8:        return (unsigned long long)v.v.u8;
        case VAL_UCHAR:        return (unsigned long long)v.v.uc;
        case VAL_UINT16:       return (unsigned long long)v.v.u16;
        case VAL_USHORT:       return (unsigned long long)v.v.us;
        case VAL_UINT32:       return (unsigned long long)v.v.u32;
        case VAL_UINT:         return (unsigned long long)v.v.ui;
        case VAL_UINT64:       return (unsigned long long)v.v.u64;
        case VAL_ULONG:        return (unsigned long long)v.v.ul;
        case VAL_SIZE_T:       return (unsigned long long)v.v.st;
        case VAL_SSIZE_T:      return (unsigned long long)v.v.sst;
        case VAL_FLOAT:        return (unsigned long long)v.v.f;
        case VAL_DOUBLE:       return (unsigned long long)v.v.d;
        case VAL_LONG_DOUBLE:  return (unsigned long long)v.v.ld;
        case VAL_BOOL:         return v.v.b ? 1ULL : 0ULL;
        case VAL_CHAR:         return (unsigned long long)(unsigned char)v.v.c;
        case VAL_STRING:       return strtoull(lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "0", NULL, 10);
        case VAL_NONE:         return 0;
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
// float：32 位单精度截断
static Value cast_float_rec(Value v) {
    /* 数组/map 递归处理每个元素 */
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
        while(map_iter_next(&it, &__k, &__v))
            lumyr_map_set(&r, __k, cast_float_rec(__v));
        return r;
    }
    /* 标量类型：统一走 value_as_number（已补全所有类型） */
    double d = value_as_number(v);
    return lumyr_make_float((float)d);
}
Value lumyr_cast_float(Value v) { return cast_float_rec(v); }
