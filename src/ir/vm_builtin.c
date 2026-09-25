/*
 * vm_builtin.c - 内置函数/方法统一分发
 *
 * OPC_BUILTIN（全局形式 m(x, ...)：argv[0] 即首参/receiver）
 * OPC_CALL_BUILTIN_METHOD（方法/属性形式 x.m(...)：receiver 单独在实参之下）
 * 两者共用 builtin_dispatch：外层 switch(BuiltinId) → 内层按 receiver 运行时类型分派。
 *
 * 零转换开销原则：
 *  - 热路径（元素级循环）不构造 Value：TypedArray 裸指针 cast 直读直写；
 *    VAL_ARRAY 元素本来就是 Value，直读 union 字段
 *  - 装箱只发生在"跨栈边界"且每值最多一次（标量返回、回调传参）
 */
#include "vm_types.h"
#include "vm_exec.h"
#include "vm_generator.h"
#include "stack_manager.h"
#include "lm_value.h"
#include "gc_runtime.h"
#include "lm_value.h"
#include "lm_string.h"
#include "lm_array.h"
#include "lm_map.h"
#include "lm_math.h"
#include "lm_crypto.h"
#include "lm_regex.h"
#include "lm_time.h"
#include "lm_container.h"
#include "lm_calendar.h"
#include "lm_file.h"
#include "lm_formdata.h"
#include "lm_socket.h"
#include "lm_type.h"
#include "lm_json.h"
#include "lm_io.h"
#include "vm_serialize.h"
#include "lm_http.h"
#include "lm_lock.h"
#include "lm_tls.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <sys/stat.h>

/* ===== 整型族元素读取/写入（与 vm_exec_stack.c 同语义，零装箱） ===== */

static int bi_is_int_et(ValueType et) {
    switch(et) {
    case VAL_INT: case VAL_INT8: case VAL_INT16: case VAL_INT32:
    case VAL_INT64: case VAL_LONG_LONG: case VAL_LONG:
    case VAL_UINT8: case VAL_UINT16: case VAL_UINT32: case VAL_UINT:
    case VAL_UINT64: case VAL_ULONG: case VAL_UCHAR:
    case VAL_SHORT: case VAL_USHORT: case VAL_SIZE_T: case VAL_SSIZE_T:
    case VAL_BOOL: case VAL_CHAR: case VAL_BYTE:
        return 1;
    default:
        return 0;
    }
}

static int bi_is_float_et(ValueType et) {
    return et == VAL_DOUBLE || et == VAL_FLOAT || et == VAL_LONG_DOUBLE;
}

static int64_t bi_read_i64_et(ValueType et, const void* items, int i) {
    switch(et) {
    case VAL_INT:       return ((const int*)items)[i];
    case VAL_INT8:      return ((const int8_t*)items)[i];
    case VAL_INT16:     return ((const int16_t*)items)[i];
    case VAL_INT32:     return ((const int32_t*)items)[i];
    case VAL_INT64:     return ((const int64_t*)items)[i];
    case VAL_LONG_LONG: return ((const long long*)items)[i];
    case VAL_LONG:      return ((const long*)items)[i];
    case VAL_UINT8:     return ((const uint8_t*)items)[i];
    case VAL_UINT16:    return ((const uint16_t*)items)[i];
    case VAL_UINT32:    return ((const uint32_t*)items)[i];
    case VAL_UINT:      return ((const unsigned int*)items)[i];
    case VAL_UINT64:    return ((const uint64_t*)items)[i];
    case VAL_ULONG:     return ((const unsigned long*)items)[i];
    case VAL_UCHAR:     return ((const unsigned char*)items)[i];
    case VAL_SHORT:     return ((const short*)items)[i];
    case VAL_USHORT:    return ((const unsigned short*)items)[i];
    case VAL_SIZE_T:    return (int64_t)((const size_t*)items)[i];
    case VAL_SSIZE_T:   return (int64_t)((const ssize_t*)items)[i];
    case VAL_BOOL:      return ((const _Bool*)items)[i];
    case VAL_CHAR:      return ((const char*)items)[i];
    case VAL_BYTE:      return ((const uint8_t*)items)[i];
    case VAL_DOUBLE:    return (int64_t)((const double*)items)[i];
    case VAL_FLOAT:     return (int64_t)((const float*)items)[i];
    case VAL_LONG_DOUBLE: return (int64_t)((const long double*)items)[i];
    default:            return 0;
    }
}

static double bi_read_dbl_et(ValueType et, const void* items, int i) {
    switch(et) {
    case VAL_DOUBLE:      return ((const double*)items)[i];
    case VAL_FLOAT:       return (double)((const float*)items)[i];
    case VAL_LONG_DOUBLE: return (double)((const long double*)items)[i];
    default:              return (double)bi_read_i64_et(et, items, i);
    }
}

static void bi_write_et(ValueType et, void* items, int i, int64_t iv, double dv) {
    if(bi_is_float_et(et)) {
        if(et == VAL_DOUBLE)          ((double*)items)[i] = dv;
        else if(et == VAL_FLOAT)      ((float*)items)[i] = (float)dv;
        else                          ((long double*)items)[i] = (long double)dv;
        return;
    }
    switch(et) {
    case VAL_INT:       ((int*)items)[i] = (int)iv; break;
    case VAL_INT8:      ((int8_t*)items)[i] = (int8_t)iv; break;
    case VAL_INT16:     ((int16_t*)items)[i] = (int16_t)iv; break;
    case VAL_INT32:     ((int32_t*)items)[i] = (int32_t)iv; break;
    case VAL_INT64:     ((int64_t*)items)[i] = iv; break;
    case VAL_LONG_LONG: ((long long*)items)[i] = (long long)iv; break;
    case VAL_LONG:      ((long*)items)[i] = (long)iv; break;
    case VAL_UINT8:     ((uint8_t*)items)[i] = (uint8_t)iv; break;
    case VAL_UINT16:    ((uint16_t*)items)[i] = (uint16_t)iv; break;
    case VAL_UINT32:    ((uint32_t*)items)[i] = (uint32_t)iv; break;
    case VAL_UINT:      ((unsigned int*)items)[i] = (unsigned int)iv; break;
    case VAL_UINT64:    ((uint64_t*)items)[i] = (uint64_t)iv; break;
    case VAL_ULONG:     ((unsigned long*)items)[i] = (unsigned long)iv; break;
    case VAL_UCHAR:     ((unsigned char*)items)[i] = (unsigned char)iv; break;
    case VAL_SHORT:     ((short*)items)[i] = (short)iv; break;
    case VAL_USHORT:    ((unsigned short*)items)[i] = (unsigned short)iv; break;
    case VAL_SIZE_T:    ((size_t*)items)[i] = (size_t)iv; break;
    case VAL_SSIZE_T:   ((ssize_t*)items)[i] = (ssize_t)iv; break;
    case VAL_BOOL:      ((_Bool*)items)[i] = (_Bool)iv; break;
    case VAL_CHAR:      ((char*)items)[i] = (char)iv; break;
    case VAL_BYTE:      ((uint8_t*)items)[i] = (uint8_t)iv; break;
    default: break;
    }
}

/* ===== TypedArray 动态操作辅助（add/set/insert/remove/clear/addAll 共用） =====
 * 根因修复：此前类型化数组不支持任何动态增长操作（add/insert/remove/clear/
 * addAll/set 只有 VAL_ARRAY 分支），存入类字段后无法原地修改，
 * Buffer 封装类被迫用普通数组 + 手工截断规避。 */

static int64_t bi_num_i64(Value v);

/* 确保容量 >= need（2x 增长，items 经 gc_realloc 扩容，与数组 add 同策略） */
static void bi_ta_reserve(TypedArray* ta, int need) {
    if(!ta || need <= ta->cap) return;
    int nc = ta->cap > 0 ? ta->cap * 2 : 8;
    while(nc < need) nc *= 2;
    size_t isz = lumyr_etype_itemsz(ta->elem_type);
    ta->items = gc_realloc(ta->items, isz * (size_t)nc);
    ta->cap = nc;
}

/* 把 Value 写入 TypedArray 第 i 个元素槽（与 vm_exec_stack.c 下标写同语义）：
 * 数值族经 bi_write_et 按元素类型截断；string 槽复制为 GC 堆串；
 * PTR 族槽（容器/实例/高精度）存裸指针 */
static void bi_ta_write_value(TypedArray* ta, int i, Value v) {
    ValueType et = ta->elem_type;
    if(bi_is_float_et(et)) {
        double d = (v.type == VAL_DOUBLE)        ? v.v.d :
                   (v.type == VAL_FLOAT)         ? (double)v.v.f :
                   (v.type == VAL_LONG_DOUBLE)   ? (double)v.v.ld :
                   (v.type == VAL_STRING)        ? 0.0 :
                                                   (double)bi_num_i64(v);
        bi_write_et(et, ta->items, i, 0, d);
        return;
    }
    if(et == VAL_STRING) {
        const char* s = NULL;
        char buf[40];
        if(v.type == VAL_STRING) {
            s = v.str_inline ? v.v.sso.data : v.v.s;
        } else if(v.type == VAL_DOUBLE || v.type == VAL_FLOAT || v.type == VAL_LONG_DOUBLE) {
            double d = (v.type == VAL_DOUBLE) ? v.v.d :
                       (v.type == VAL_FLOAT) ? (double)v.v.f : (double)v.v.ld;
            snprintf(buf, sizeof(buf), "%g", d);
            s = buf;
        } else {
            snprintf(buf, sizeof(buf), "%lld", (long long)bi_num_i64(v));
            s = buf;
        }
        size_t l = s ? strlen(s) : 0;
        char* gs = (char*)gc_alloc(l + 1, VAL_STRING);
        if(l) memcpy(gs, s, l);
        gs[l] = '\0';
        ((char**)ta->items)[i] = gs;
        return;
    }
    if(lumyr_etype_stackcls(et) == 3) {
        /* PTR 族槽位：容器/实例/高精度对象存裸指针 */
        void* raw = NULL;
        switch(v.type) {
        case VAL_PTR:        raw = v.v.struct_ptr; break;
        case VAL_STRUCT_PTR: raw = v.v.struct_ptr; break;
        case VAL_CLASS_PTR:  raw = v.v.struct_ptr; break;
        case VAL_BIGINT:     raw = v.v.bigint; break;
        case VAL_DECIMAL:    raw = v.v.decimal; break;
        case VAL_BITDECIMAL: raw = v.v.bitdecimal; break;
        case VAL_MAP:        raw = v.v.map; break;
        case VAL_ARRAY:      raw = v.v.array; break;
        case VAL_TYPED_ARRAY: raw = v.v.typed_array; break;
        default: break;
        }
        ((void**)ta->items)[i] = raw;
        return;
    }
    bi_write_et(et, ta->items, i, bi_num_i64(v), 0);
}

/* TypedArray 第 i 个元素裸指针（PTR 族槽位通用读取） */
static void* bi_ptr_et(TypedArray* ta, int i) {
    return ta->items ? ((void**)ta->items)[i] : NULL;
}

/* TypedArray 第 i 个元素装箱为 Value（与 vm_exec_stack.c typed_box_elem 同语义：
 * 保留精确元素类型） */
static Value bi_ta_read_value(TypedArray* ta, int i) {
    Value r = val_none();
    if(!ta || i < 0 || i >= ta->len) return r;
    ValueType et = ta->elem_type;
    int cls = lumyr_etype_stackcls(et);
    if(cls == 1) {
        r.type = et;
        r.v.ll = bi_read_i64_et(et, ta->items, i);
    } else if(cls == 2) {
        r.type = et;
        if(et == VAL_DOUBLE)      r.v.d = ((const double*)ta->items)[i];
        else if(et == VAL_FLOAT)  r.v.f = ((const float*)ta->items)[i];
        else                      r.v.ld = ((const long double*)ta->items)[i];
    } else if(cls == 3) {
        r.type = et;
        r.str_inline = 0;
        r.v.s = (et == VAL_STRING) ? ((char**)ta->items)[i] : (char*)bi_ptr_et(ta, i);
    }
    return r;
}

/* TypedArray 尾部追加一个 Value（原地修改） */
static TypedArray* bi_ta_add(TypedArray* ta, Value v) {
    bi_ta_reserve(ta, ta->len + 1);
    bi_ta_write_value(ta, ta->len, v);
    ta->len++;
    return ta;
}

/* TypedArray 第 idx 处整体后移一位并写入（insert 语义） */
static void bi_ta_insert(TypedArray* ta, int idx, Value v) {
    if(idx < 0) idx = 0;
    if(idx > ta->len) idx = ta->len;
    bi_ta_reserve(ta, ta->len + 1);
    if(idx < ta->len) {
        size_t isz = lumyr_etype_itemsz(ta->elem_type);
        char* base = (char*)ta->items;
        memmove(base + (size_t)(idx + 1) * isz, base + (size_t)idx * isz,
                (size_t)(ta->len - idx) * isz);
    }
    bi_ta_write_value(ta, idx, v);
    ta->len++;
}

/* TypedArray 删除第 idx 个元素并整体前移（remove 语义，与 lumyr_del 同为按下标） */
static void bi_ta_remove(TypedArray* ta, int idx) {
    if(idx < 0 || idx >= ta->len) {
        char b[96];
        snprintf(b, sizeof b, "typed array remove: 下标 %d 越界（长度 %d）", idx, ta->len);
        runtime_error(b);
        return;
    }
    if(idx < ta->len - 1) {
        size_t isz = lumyr_etype_itemsz(ta->elem_type);
        char* base = (char*)ta->items;
        memmove(base + (size_t)idx * isz, base + (size_t)(idx + 1) * isz,
                (size_t)(ta->len - idx - 1) * isz);
    }
    ta->len--;
}

/* TypedArray 追加 src 的全部元素（addAll 语义：VAL_ARRAY / VAL_TYPED_ARRAY 通用） */
static void bi_ta_addall(TypedArray* ta, Value src) {
    if(src.type == VAL_TYPED_ARRAY && src.v.typed_array) {
        TypedArray* o = src.v.typed_array;
        if(o->elem_type == ta->elem_type && o->len > 0) {
            /* 同元素类型：整块 memcpy 零装箱 */
            bi_ta_reserve(ta, ta->len + o->len);
            size_t isz = lumyr_etype_itemsz(ta->elem_type);
            memcpy((char*)ta->items + (size_t)ta->len * isz, o->items,
                   (size_t)o->len * isz);
            ta->len += o->len;
            return;
        }
        for(int i = 0; i < o->len; i++)
            bi_ta_add(ta, bi_ta_read_value(o, i));
        return;
    }
    if(src.type == VAL_ARRAY && src.v.array) {
        ValueArray* a = src.v.array;
        for(int i = 0; i < a->len; i++)
            bi_ta_add(ta, a->items[i]);
        return;
    }
    runtime_error("typed array addAll: 参数必须是数组或类型化数组");
}

/* ===== 向量/矩阵元素读取抽象（VAL_ARRAY 直读 union / TypedArray 裸读） ===== */

static int bi_len_of(Value v) {
    switch(v.type) {
    case VAL_STRING:      return lumyr_str_len(&v);
    case VAL_ARRAY:       return v.v.array ? v.v.array->len : 0;
    case VAL_TYPED_ARRAY: return v.v.typed_array ? v.v.typed_array->len : 0;
    case VAL_MAP:         return v.v.map ? v.v.map->len : 0;
    case VAL_TUPLE:       return lumyr_tuple_len(v);
    case VAL_SET:         return lumyr_set_len(v);
    case VAL_BYTES:       return lumyr_bytes_len(v);
    case VAL_FORMDATA:    return lumyr_formdata_len(v);
    default:              return 0;
    }
}

static int64_t bi_num_i64(Value v);

/* 第 i 个元素作为 double（array 元素经一次 union 判读，typed 裸读） */
static double bi_vec_dbl(Value v, int i) {
    if(v.type == VAL_ARRAY) {
        Value e = v.v.array->items[i];
        switch(e.type) {
        case VAL_DOUBLE: return e.v.d;
        case VAL_FLOAT:  return (double)e.v.f;
        case VAL_LONG_DOUBLE: return (double)e.v.ld;
        case VAL_INT64:  return (double)e.v.i64;
        case VAL_INT:    return (double)e.v.i;
        case VAL_LONG:   return (double)e.v.l;
        case VAL_LONG_LONG: return (double)e.v.ll;
        case VAL_BOOL:   return (double)e.v.b;
        case VAL_CHAR:   return (double)e.v.c;
        default:         return (double)bi_num_i64(e); /* 其余整型族兜底 */
        }
    }
    if(v.type == VAL_TYPED_ARRAY && v.v.typed_array) {
        TypedArray* ta = v.v.typed_array;
        return bi_read_dbl_et(ta->elem_type, ta->items, i);
    }
    return 0.0;
}

/* 元素是否全为整型族（决定 sum/dot 等输出 int 还是 double） */
static int bi_vec_int_only(Value v) {
    int n = bi_len_of(v);
    for(int i = 0; i < n; i++) {
        if(v.type == VAL_ARRAY) {
            ValueType t = v.v.array->items[i].type;
            if(t == VAL_DOUBLE || t == VAL_FLOAT || t == VAL_LONG_DOUBLE) return 0;
        } else if(v.type == VAL_TYPED_ARRAY) {
            if(bi_is_float_et(v.v.typed_array->elem_type)) return 0;
        }
    }
    return 1;
}

/* 标量数值兜底：任意 Value → int64（覆盖全部整型族 + 浮点截断） */
static int64_t bi_num_i64(Value v) {
    switch(v.type) {
    case VAL_INT64: return v.v.i64;
    case VAL_INT:   return (int64_t)v.v.i;
    case VAL_INT8:  return (int64_t)v.v.i8;
    case VAL_INT16: return (int64_t)v.v.i16;
    case VAL_INT32: return (int64_t)v.v.i32;
    case VAL_LONG:  return (int64_t)v.v.l;
    case VAL_LONG_LONG: return (int64_t)v.v.ll;
    case VAL_SHORT: return (int64_t)v.v.sh;
    case VAL_UINT8: return (int64_t)v.v.u8;
    case VAL_UCHAR: return (int64_t)v.v.uc;
    case VAL_BYTE:  return (int64_t)v.v.by;
    case VAL_UINT16: return (int64_t)v.v.u16;
    case VAL_USHORT: return (int64_t)v.v.us;
    case VAL_UINT32: return (int64_t)v.v.u32;
    case VAL_UINT:  return (int64_t)v.v.ui;
    case VAL_UINT64: return (int64_t)v.v.u64;
    case VAL_ULONG: return (int64_t)v.v.ul;
    case VAL_SIZE_T: return (int64_t)v.v.st;
    case VAL_SSIZE_T: return (int64_t)v.v.sst;
    case VAL_BOOL:  return (int64_t)v.v.b;
    case VAL_CHAR:  return (int64_t)v.v.c;
    case VAL_DOUBLE:return (int64_t)v.v.d;
    case VAL_FLOAT: return (int64_t)v.v.f;
    case VAL_LONG_DOUBLE: return (int64_t)v.v.ld;
    default:        return 0;
    }
}

/* 第 i 个元素作为 int64：整型聚合的零精度损失路径。
 * 根因修复：此前整型 sum/dot/matmul/transpose 经 bi_vec_dbl（double）往返，
 * int64 大值（如 2^63-1）转 double 舍入为 2^63，回转 int64 得 INT64_MIN，精度丢失。
 * 整型元素原生读取，仅浮点元素保留截断语义。 */
static int64_t bi_vec_i64(Value v, int i) {
    if(v.type == VAL_ARRAY) {
        Value e = v.v.array->items[i];
        switch(e.type) {
        case VAL_DOUBLE: case VAL_FLOAT: case VAL_LONG_DOUBLE:
            return (int64_t)bi_vec_dbl(v, i);
        default:
            return bi_num_i64(e);
        }
    }
    if(v.type == VAL_TYPED_ARRAY && v.v.typed_array) {
        TypedArray* ta = v.v.typed_array;
        if(bi_is_float_et(ta->elem_type))
            return (int64_t)bi_read_dbl_et(ta->elem_type, ta->items, i);
        return bi_read_i64_et(ta->elem_type, ta->items, i);
    }
    return 0;
}

/* ===== 值相等 / 引用身份 ===== */

static int bi_value_eq(Value a, Value b) {
    if(a.type == b.type) {
        switch(a.type) {
        case VAL_INT64: return a.v.i64 == b.v.i64;
        case VAL_INT:   return a.v.i == b.v.i;
        case VAL_LONG:  return a.v.l == b.v.l;
        case VAL_LONG_LONG: return a.v.ll == b.v.ll;
        case VAL_BOOL:  return a.v.b == b.v.b;
        case VAL_CHAR:  return a.v.c == b.v.c;
        case VAL_DOUBLE:return a.v.d == b.v.d;
        case VAL_FLOAT: return a.v.f == b.v.f;
        case VAL_STRING:return strcmp(lumyr_str_cstr(&a), lumyr_str_cstr(&b)) == 0;
        default: break;
        }
    }
    /* 数值跨类型比较 */
    switch(a.type) {
    case VAL_INT64: case VAL_INT: case VAL_LONG: case VAL_LONG_LONG:
    case VAL_BOOL: case VAL_CHAR: case VAL_DOUBLE: case VAL_FLOAT: case VAL_LONG_DOUBLE:
        break;
    default:
        return 0;
    }
    switch(b.type) {
    case VAL_INT64: case VAL_INT: case VAL_LONG: case VAL_LONG_LONG:
    case VAL_BOOL: case VAL_CHAR: case VAL_DOUBLE: case VAL_FLOAT: case VAL_LONG_DOUBLE:
        break;
    default:
        return 0;
    }
    double da, db;
    if(a.type == VAL_DOUBLE) da = a.v.d; else if(a.type == VAL_FLOAT) da = a.v.f;
    else if(a.type == VAL_LONG_DOUBLE) da = (double)a.v.ld; else da = (double)bi_num_i64(a);
    if(b.type == VAL_DOUBLE) db = b.v.d; else if(b.type == VAL_FLOAT) db = b.v.f;
    else if(b.type == VAL_LONG_DOUBLE) db = (double)b.v.ld; else db = (double)bi_num_i64(b);
    return da == db;
}

/* 引用身份：union 指针相等（objectIndex 用），标量退化为值相等 */
static int bi_same_ref(Value a, Value b) {
    if(a.type != b.type) return 0;
    switch(a.type) {
    case VAL_ARRAY:       return a.v.array == b.v.array;
    case VAL_TYPED_ARRAY: return a.v.typed_array == b.v.typed_array;
    case VAL_MAP:         return a.v.map == b.v.map;
    case VAL_STRUCT_PTR: case VAL_CLASS_PTR:
                          return a.v.struct_ptr == b.v.struct_ptr;
    case VAL_BIGINT:      return a.v.bigint == b.v.bigint;
    case VAL_DECIMAL:     return a.v.decimal == b.v.decimal;
    case VAL_BITDECIMAL:  return a.v.bitdecimal == b.v.bitdecimal;
    case VAL_FORMDATA:    return a.v.formdata_obj == b.v.formdata_obj;
    case VAL_GENERATOR:   return a.v.generator == b.v.generator;
    case VAL_FUNC:        return a.v.func.func_obj == b.v.func.func_obj;
    case VAL_STRING:      return strcmp(lumyr_str_cstr(&a), lumyr_str_cstr(&b)) == 0;
    default:              return bi_value_eq(a, b);
    }
}

/* ===== AI / 线代：形状与构造 ===== */

#define BI_MAX_DIMS 8

/* 收集形状：返回维度数；检测到数组元素类型混合/超深返回 -1。
 * TypedArray 视为叶子向量（其 len 计入最后一维）。 */
static int bi_shape_of(Value v, int* dims) {
    int d = 0;
    Value cur = v;
    for(;;) {
        if(d >= BI_MAX_DIMS) return -1;
        if(cur.type == VAL_TYPED_ARRAY) {
            TypedArray* ta = cur.v.typed_array;
            if(!ta) return -1;
            if(!bi_is_int_et(ta->elem_type) && !bi_is_float_et(ta->elem_type))
                return -1; /* 非数值 typed 不能参与形状 */
            dims[d++] = ta->len;
            return d;
        }
        if(cur.type != VAL_ARRAY) {
            /* 叶子标量：形状到此为止 */
            return d;
        }
        ValueArray* a = cur.v.array;
        dims[d++] = a ? a->len : 0;
        if(!a || a->len == 0) return d;
        cur = a->items[0];
    }
}

/* 校验形状一致（不规则报错）；level 从 0 起，nd 为总维数 */
static int bi_check_shape(Value v, const int* dims, int level, int nd) {
    if(level >= nd) {
        /* 叶子：不得再是数组/向量 */
        return v.type != VAL_ARRAY && v.type != VAL_TYPED_ARRAY;
    }
    if(v.type == VAL_ARRAY) {
        ValueArray* a = v.v.array;
        if(!a || a->len != dims[level]) return 0;
        for(int i = 0; i < a->len; i++) {
            if(!bi_check_shape(a->items[i], dims, level + 1, nd)) return 0;
        }
        return 1;
    }
    if(v.type == VAL_TYPED_ARRAY) {
        TypedArray* ta = v.v.typed_array;
        return ta && ta->len == dims[level] && level == nd - 1;
    }
    return 0;
}

/* 按输入种类构造同构数值数组输出：typed → 同 et 新 TypedArray；array → val_array
 * vals 契约：is_double=1 → double 数组（每元素 8 字节）；
 *           is_double=0 → 按 et 原生宽度的裸元素缓冲（isz 字节/元素）。
 * 根因修复：此前 is_double=0 一律按 int64（8 字节）读取，而 slice/concat
 * 传入的是原生宽度缓冲（如 int 为 4 字节）→ 错位读 + 越界，输出乱值。 */
static Value bi_vec_out(Value proto, ValueType et, int n, const void* vals, int is_double) {
    if(proto.type == VAL_TYPED_ARRAY) {
        Value r;
        memset(&r, 0, sizeof(r));
        r.type = VAL_TYPED_ARRAY;
        gc_disable();
        TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
        ta->elem_type = et;
        ta->stack_alloc = 0;
        ta->len = n;
        ta->cap = n > 0 ? n : 8;
        size_t isz = lumyr_etype_itemsz(et);
        /* 根因修复：空结果同样分配缓冲区（此前 cap=8 而 items=NULL，
         * 容量承诺与实际分配不符 → 后续写 NULL 指针 SIGSEGV） */
        ta->items = gc_alloc_old(isz * (size_t)ta->cap, VAL_TYPED_ARRAY);
        gc_mark_internal_buf(ta->items);
        if(n > 0) {
            if(is_double) {
                for(int i = 0; i < n; i++)
                    bi_write_et(et, ta->items, i, (int64_t)((const double*)vals)[i],
                                ((const double*)vals)[i]);
            } else {
                memcpy(ta->items, vals, isz * (size_t)n);   /* 原生宽度裸元素直拷 */
            }
        }
        r.v.typed_array = ta;
        gc_enable();
        return r;
    }
    /* 未声明数组输出：每元素一次装箱（Value 表示所限） */
    Value r = val_array(n);
    for(int i = 0; i < n; i++) {
        if(is_double) r.v.array->items[i] = lumyr_make_double(((const double*)vals)[i]);
        else          r.v.array->items[i] = lumyr_make_int64(bi_read_i64_et(et, vals, i));
    }
    return r;
}

/* 递归展平收集（reshape 用）：typed 元素装箱一次，array 元素零拷贝 */
static void bi_flatten(Value v, Value* out, int* k) {
    if(v.type == VAL_ARRAY) {
        ValueArray* a = v.v.array;
        if(a) for(int i = 0; i < a->len; i++) bi_flatten(a->items[i], out, k);
        return;
    }
    if(v.type == VAL_TYPED_ARRAY) {
        TypedArray* ta = v.v.typed_array;
        if(ta) for(int i = 0; i < ta->len; i++) {
            ValueType et = ta->elem_type;
            if(bi_is_float_et(et))
                out[(*k)++] = lumyr_make_double(bi_read_dbl_et(et, ta->items, i));
            else
                out[(*k)++] = lumyr_make_int64(bi_read_i64_et(et, ta->items, i));
        }
        return;
    }
    out[(*k)++] = v;
}

/* 用扁平元素按 dims 重建嵌套数组 */
static Value bi_build_nested(Value* flat, const int* dims, int nd, int* k, int level) {
    int n = dims[level];
    if(level == nd - 1) {
        Value r = val_array(n);
        for(int i = 0; i < n; i++) r.v.array->items[i] = flat[(*k)++];
        return r;
    }
    Value r = val_array(n);
    for(int i = 0; i < n; i++) {
        r.v.array->items[i] = bi_build_nested(flat, dims, nd, k, level + 1);
    }
    return r;
}

/* 2D 矩阵元素读取（行可为 VAL_ARRAY 或数值 TypedArray） */
static double bi_mat_read(Value A, int r, int c) {
    Value row = A.v.array->items[r];
    if(row.type == VAL_ARRAY) return bi_vec_dbl(row, c);
    if(row.type == VAL_TYPED_ARRAY && row.v.typed_array)
        return bi_read_dbl_et(row.v.typed_array->elem_type, row.v.typed_array->items, c);
    return 0.0;
}

/* 2D 矩阵元素读取为 int64：整型路径零精度损失（配合 bi_vec_i64） */
static int64_t bi_mat_i64(Value A, int r, int c) {
    Value row = A.v.array->items[r];
    if(row.type == VAL_ARRAY) return bi_vec_i64(row, c);
    if(row.type == VAL_TYPED_ARRAY && row.v.typed_array)
        return bi_read_i64_et(row.v.typed_array->elem_type, row.v.typed_array->items, c);
    return 0;
}

/* 2D 校验：rows/cols 输出；非法返回 0 */
static int bi_mat_dims(Value A, int* rows, int* cols) {
    if(A.type != VAL_ARRAY || !A.v.array) return 0;
    int dims[BI_MAX_DIMS];
    int nd = bi_shape_of(A, dims);
    if(nd != 2) return 0;
    if(!bi_check_shape(A, dims, 0, 2)) return 0;
    *rows = dims[0];
    *cols = dims[1];
    return 1;
}

/* inv 输出行构造：跟随输入行类型（typed 行 → 同 et typed；array 行 → double Value 行） */
static Value bi_inv_row_like(Value proto_row, const double* vals, int n) {
    if(proto_row.type == VAL_TYPED_ARRAY) {
        return bi_vec_out(proto_row, proto_row.v.typed_array->elem_type, n, vals, 1);
    }
    Value r = val_array(n);
    for(int i = 0; i < n; i++) r.v.array->items[i] = lumyr_make_double(vals[i]);
    return r;
}

/* 原地 qsort 比较器：array（Value 数字/字符串） */
static int bi_cmp_val(const void* pa, const void* pb) {
    Value a = *(const Value*)pa, b = *(const Value*)pb;
    if(a.type == VAL_STRING && b.type == VAL_STRING)
        return strcmp(lumyr_str_cstr(&a), lumyr_str_cstr(&b));
    double da, db;
    if(a.type == VAL_DOUBLE) da = a.v.d; else if(a.type == VAL_FLOAT) da = a.v.f;
    else if(a.type == VAL_LONG_DOUBLE) da = (double)a.v.ld; else da = (double)bi_num_i64(a);
    if(b.type == VAL_DOUBLE) db = b.v.d; else if(b.type == VAL_FLOAT) db = b.v.f;
    else if(b.type == VAL_LONG_DOUBLE) db = (double)b.v.ld; else db = (double)bi_num_i64(b);
    return (da > db) - (da < db);
}

/* 按元素宽度比较的 qsort 比较器（int 族/float 族）：
 * 元素宽度由 g_cmp_width 决定（qsort 比较器无上下文参数，用文件级变量传递） */
static int g_cmp_width = 8;

static int bi_cmp_int_w(const void* pa, const void* pb) {
    int64_t a, b;
    switch(g_cmp_width) {
    case 1:  a = *(const int8_t*)pa;  b = *(const int8_t*)pb;  break;
    case 2:  a = *(const int16_t*)pa; b = *(const int16_t*)pb; break;
    case 4:  a = *(const int32_t*)pa; b = *(const int32_t*)pb; break;
    default: a = *(const int64_t*)pa; b = *(const int64_t*)pb; break;
    }
    return (a > b) - (a < b);
}

static int bi_cmp_dbl_w(const void* pa, const void* pb) {
    double a, b;
    switch(g_cmp_width) {
    case 4:  a = *(const float*)pa;  b = *(const float*)pb;  break;
    default: a = *(const double*)pa; b = *(const double*)pb; break;
    }
    return (a > b) - (a < b);
}

static int bi_cmp_str(const void* pa, const void* pb) {
    const char* a = *(const char* const*)pa;
    const char* b = *(const char* const*)pb;
    if(!a) return b ? -1 : 0;
    if(!b) return 1;
    return strcmp(a, b);
}

/* 类型错误提示（无兜底：不可恢复，立即中止进程） */
static int bi_type_err(const char* name, Value recv) {
    Value tn = lumyr_type(recv);
    fprintf(stderr, "运行时错误: 类型 %s 不支持方法 .%s\n",
            lumyr_str_cstr(&tn), name);
    exit(1);
}

static int bi_need_args(const char* name, int argc, int need) {
    if(argc < need) {
        fprintf(stderr, "运行时错误: .%s 需要 %d 个参数，实际 %d 个\n", name, need, argc);
        exit(1);
    }
    return 1;
}

/* 线程/锁族参数校验（中英双语；不可恢复同 bi_type_err，立即中止） */
static int bi_need_args_mt(const char* name, int argc, int need) {
    if(argc < need) {
        fprintf(stderr, "运行时错误: %s 需要 %d 个参数，实际 %d 个 / %s expects %d args, got %d\n",
                name, need, argc, name, need, argc);
        exit(1);
    }
    return 1;
}

static void bi_need_int_mt(const char* name, Value v, int pos) {
    if(!bi_is_int_et(v.type)) {
        fprintf(stderr, "运行时错误: %s 第 %d 个参数须为整数 id / %s: argument %d must be an integer id\n",
                name, pos, name, pos);
        exit(1);
    }
}

/* 返回的 cstr 指向 *v 内部（SSO 内联缓冲区），调用方须保证 v 在使用期间存活
 * （不能传按值拷贝的局部 Value——返回后其 SSO 指针随栈帧失效） */
static const char* bi_need_str_mt(const char* name, const Value* v, int pos) {
    if(v->type != VAL_STRING) {
        fprintf(stderr, "运行时错误: %s 第 %d 个参数须为字符串 / %s: argument %d must be a string\n",
                name, pos, name, pos);
        exit(1);
    }
    return lumyr_str_cstr(v);
}

/* formdata → map：同名聚合（单个=值，多个=数组，保序），file/bytes 值保留原样 */
static Value bi_formdata_to_map(Value fd) {
    FormDataObj* o = fd.v.formdata_obj;
    Value m = val_map();
    int n = o ? o->len : 0;
    for(int i = 0; i < n; i++) {
        Value k = lumyr_make_string(o->names[i] ? o->names[i] : "");
        Value v = o->vals[i];
        if(lumyr_map_has(m, k)) {
            Value ex = lumyr_map_get(m, k);
            if(ex.type == VAL_ARRAY) {
                ValueArray* a = ex.v.array;
                int old = a ? a->len : 0;
                Value nr = val_array(old + 1);
                for(int j = 0; j < old; j++) nr.v.array->items[j] = a->items[j];
                nr.v.array->items[old] = v;
                lumyr_map_set(&m, k, nr);
            } else {
                Value nr = val_array(2);
                nr.v.array->items[0] = ex;
                nr.v.array->items[1] = v;
                lumyr_map_set(&m, k, nr);
            }
        } else {
            lumyr_map_set(&m, k, v);
        }
    }
    return m;
}

/* struct/class 实例 → map：全字段（含 private），保声明顺序 */
static Value bi_instance_to_map(Value obj) {
    Value m = val_map();
    RuntimeTypeInfo* info = lumyr_instance_get_info(obj);
    if(info) {
        for(int i = 0; i < info->nfields; i++) {
            Value fv = lumyr_field_get(obj, info->fields[i].name);
            lumyr_map_set(&m, lumyr_make_string(info->fields[i].name), fv);
        }
    }
    return m;
}

/* struct/class 实例 → [[field, v], ...]：保声明顺序 */
static Value bi_instance_to_array(Value obj) {
    RuntimeTypeInfo* info = lumyr_instance_get_info(obj);
    int n = info ? info->nfields : 0;
    Value r = val_array(n);
    if(info) {
        for(int i = 0; i < n; i++) {
            Value pair = val_array(2);
            pair.v.array->items[0] = lumyr_make_string(info->fields[i].name);
            pair.v.array->items[1] = lumyr_field_get(obj, info->fields[i].name);
            r.v.array->items[i] = pair;
        }
    }
    return r;
}

/* JSON 清洗：递归把不可序列化类型转为等价值
 * file → 路径字符串；bytes → 原始内容字符串；formdata/实例 → map；容器深拷贝递归 */
static Value bi_json_clean(Value v) {
    switch(v.type) {
    case VAL_FILE: {
        FileObj* f = (FileObj*)v.v.file_obj;
        return lumyr_make_string((f && f->path) ? f->path : "");
    }
    case VAL_BYTES: {
        BytesObj* b = (BytesObj*)v.v.bytes_obj;
        if(!b || b->len <= 0 || !b->data) return lumyr_make_string("");
        char* tmp = (char*)malloc((size_t)b->len + 1);
        if(!tmp) return lumyr_make_string("");
        memcpy(tmp, b->data, (size_t)b->len);
        tmp[b->len] = '\0';
        Value r = lumyr_make_string(tmp);
        free(tmp);
        return r;
    }
    case VAL_FORMDATA:
        return bi_json_clean(bi_formdata_to_map(v));
    case VAL_SOCKET: {
        /* socket → 描述字符串（不可序列化为 JSON 结构） */
        char* s = lumyr_socket_to_str(v);
        Value r = lumyr_make_string(s ? s : "");
        free(s);
        return r;
    }
    case VAL_STRUCT_PTR:
    case VAL_CLASS_PTR:
        return bi_json_clean(bi_instance_to_map(v));
    case VAL_ARRAY: {
        ValueArray* a = v.v.array;
        int n = a ? a->len : 0;
        Value r = val_array(n);
        for(int i = 0; i < n; i++) r.v.array->items[i] = bi_json_clean(a->items[i]);
        return r;
    }
    case VAL_MAP: {
        Value ks = lumyr_map_keys(v);
        ValueArray* ka = ks.v.array;
        int n = ka ? ka->len : 0;
        Value r = val_map();
        for(int i = 0; i < n; i++) {
            Value nv = bi_json_clean(lumyr_map_get(v, ka->items[i]));
            lumyr_map_set(&r, ka->items[i], nv);
        }
        return r;
    }
    default:
        return v;
    }
}

/* ============================================================
 * 通用深拷贝：所有数据类型 .copy()
 *   标量/不可变数值（int/double/bool/char/bigint/decimal/bytes/complex/calendar）→ 原值
 *   string/容器（array/map/formdata/tuple/set/typed array）→ 独立副本并递归
 *   struct/class 实例 → 新实例裸拷贝后，对引用字段递归（绕过 private 访问检查）
 *   file/date/folder/error → 复制对象及其字符串
 * ============================================================ */
static Value bi_copy(Value v) {
    switch(v.type) {
    /* ---- 标量与不可变类型：直接返回（共享安全） ---- */
    case VAL_NONE:
    /* int 族 */
    case VAL_INT: case VAL_INT8: case VAL_INT16: case VAL_INT32: case VAL_INT64:
    case VAL_LONG: case VAL_LONG_LONG: case VAL_SHORT:
    case VAL_UINT: case VAL_UINT8: case VAL_UINT16: case VAL_UINT32: case VAL_UINT64:
    case VAL_ULONG: case VAL_UCHAR: case VAL_USHORT: case VAL_SIZE_T: case VAL_SSIZE_T:
    /* double 族 */
    case VAL_DOUBLE: case VAL_FLOAT: case VAL_LONG_DOUBLE:
    case VAL_BOOL: case VAL_CHAR: case VAL_BYTE:
    /* 不可变堆对象/纯值 */
    case VAL_BIGINT: case VAL_DECIMAL: case VAL_BITDECIMAL:
    case VAL_BYTES: case VAL_COMPLEX: case VAL_CALENDAR:
    case VAL_FUNC: case VAL_GENERATOR:
    case VAL_SOCKET:   /* socket：fd 为唯一系统资源，拷贝共享引用（不复制 fd，避免双重关闭） */
        return v;

    /* ---- 字符串：独立副本（make_string 内部按长度 SSO/GC 堆） ---- */
    case VAL_STRING: {
        const char* cs = lumyr_str_cstr(&v);
        return lumyr_make_string(cs ? cs : "");
    }

    /* ---- 动态数组：逐项递归 ---- */
    case VAL_ARRAY: {
        ValueArray* a = v.v.array;
        int n = a ? a->len : 0;
        Value r = val_array(n);
        for(int i = 0; i < n; i++)
            r.v.array->items[i] = bi_copy(a->items[i]);
        return r;
    }

    /* ---- map：键值均递归 ---- */
    case VAL_MAP: {
        Value ks = lumyr_map_keys(v);
        ValueArray* ka = ks.v.array;
        int n = ka ? ka->len : 0;
        Value r = val_map();
        for(int i = 0; i < n; i++) {
            Value nk = bi_copy(ka->items[i]);
            Value nv = bi_copy(lumyr_map_get(v, ka->items[i]));
            lumyr_map_set(&r, nk, nv);
        }
        return r;
    }

    /* ---- typed array：缓冲复制；字符串元素逐个独立 ---- */
    case VAL_TYPED_ARRAY: {
        TypedArray* ta = v.v.typed_array;
        int n = ta ? ta->len : 0;
        ValueType et = ta ? ta->elem_type : VAL_INT;
        Value rv; memset(&rv, 0, sizeof(rv));
        rv.type = VAL_TYPED_ARRAY;
        gc_disable();
        TypedArray* nta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
        nta->elem_type = et;
        nta->stack_alloc = 0;
        nta->len = n;
        /* 根因修复：源 items 为空时容量置 0（不虚报 cap），
         * 保持"cap 承诺与实际分配一致"不变式 */
        nta->cap = (ta && ta->items) ? ta->cap : 0;
        if(n > 0 && ta->items) {
            size_t isz = lumyr_etype_itemsz(et);
            nta->items = gc_alloc_old(isz * (size_t)nta->cap, VAL_TYPED_ARRAY);
            gc_mark_internal_buf(nta->items);
            memcpy(nta->items, ta->items, isz * (size_t)n);
            /* 字符串元素：逐个深拷贝（bigint/decimal 等不可变对象保持共享） */
            if(et == VAL_STRING) {
                for(int i = 0; i < n; i++) {
                    char* p = ((char**)nta->items)[i];
                    if(p) {
                        size_t l = strlen(p);
                        char* np = (char*)gc_alloc(l + 1, VAL_STRING);
                        memcpy(np, p, l + 1);
                        ((char**)nta->items)[i] = np;
                    }
                }
            }
        } else {
            nta->items = NULL;
        }
        rv.v.typed_array = nta;
        gc_enable();
        return rv;
    }

    /* ---- tuple：逐项递归 ---- */
    case VAL_TUPLE: {
        TupleObj* t = (TupleObj*)v.v.tuple_obj;
        int n = t ? t->len : 0;
        Value* tmp = n > 0 ? (Value*)malloc(sizeof(Value) * (size_t)n) : NULL;
        for(int i = 0; i < n; i++) tmp[i] = bi_copy(t->items[i]);
        Value r = lumyr_tuple_make(n, tmp);
        free(tmp);
        return r;
    }

    /* ---- set：元素递归（经数组快照重建） ---- */
    case VAL_SET: {
        Value r = lumyr_set_make(0, NULL);
        Value elems = lumyr_set_to_array(v);
        ValueArray* ea = elems.v.array;
        int n = ea ? ea->len : 0;
        for(int i = 0; i < n; i++)
            lumyr_set_add(&r, bi_copy(ea->items[i]));
        return r;
    }

    /* ---- formdata：名/值逐项递归（保留重复名与顺序） ---- */
    case VAL_FORMDATA: {
        FormDataObj* o = (FormDataObj*)v.v.formdata_obj;
        int n = o ? o->len : 0;
        Value r = lumyr_formdata_make(n > 0 ? n : 4);
        for(int i = 0; i < n; i++)
            lumyr_formdata_add(r, lumyr_make_string(o->names[i] ? o->names[i] : ""),
                               bi_copy(o->vals[i]));
        return r;
    }

    /* ---- struct/class：浅拷贝实例后，对引用字段裸递归（绕过访问检查） ---- */
    case VAL_STRUCT_PTR:
    case VAL_CLASS_PTR: {
        /* 根因修复：实例创建 + 字段深拷贝（map/嵌套实例递归分配）全程 gc_disable，
         * 否则字段拷贝的分配可能触发 GC——新实例仅存于 C 局部变量未生根，被回收
         * 复用后字段写入腐败（症状：拷贝实例的方法内字段读返回错误对象，且
         * 是否复现取决于分配时序）。与上方 typed array 分支同一保护模式；
         * gc_disable/enable 为引用计数，嵌套递归安全。 */
        gc_disable();
        Value r = lumyr_instance_copy(v); /* gc 新实例 + memcpy */
        RuntimeTypeInfo* info = lumyr_instance_get_info(v);
        if(!info || r.type != v.type) { gc_enable(); return r; }
        char* base = (char*)r.v.struct_ptr;
        for(int i = 0; i < info->nfields; i++) {
            FieldInfo* fi = &info->fields[i];
            int cls = lumyr_etype_stackcls(fi->valtype);
            if(cls == 1 || cls == 2) continue; /* int/double 裸值已随 memcpy 复制 */
            char* fp = base + fi->offset;
            if(fi->valtype == VAL_STRING) {
                char* p = *(char**)fp;
                if(p) {
                    size_t l = strlen(p);
                    char* np = (char*)gc_alloc(l + 1, VAL_STRING);
                    memcpy(np, p, l + 1);
                    *(char**)fp = np;
                }
                continue;
            }
            void* p = *(void**)fp;
            if(!p) continue; /* 可空字段未指向对象 */
            Value fv; memset(&fv, 0, sizeof(fv));
            fv.type = fi->valtype;
            fv.v.struct_ptr = p;
            Value cv = bi_copy(fv);
            *(void**)fp = cv.v.struct_ptr; /* union 内各指针字段同偏移 */
        }
        gc_enable();
        return r;
    }

    /* ---- date/time：无内部引用，复制对象 ---- */
    case VAL_DATE: case VAL_DATETIME: case VAL_TIME: case VAL_TIMEDELTA: {
        DateObj* o = (DateObj*)v.v.date_obj;
        if(!o) return v;
        DateObj* no = (DateObj*)gc_alloc(sizeof(DateObj), v.type);
        memcpy(no, o, sizeof(DateObj));
        Value r = v; r.v.date_obj = no;
        return r;
    }

    /* ---- folder：复制对象 + 路径串 ---- */
    case VAL_FOLDER: {
        FolderObj* o = (FolderObj*)v.v.folder_obj;
        if(!o) return v;
        FolderObj* no = (FolderObj*)gc_alloc(sizeof(FolderObj), VAL_FOLDER);
        no->stack_alloc = 0;
        if(o->path) {
            size_t l = strlen(o->path);
            no->path = (char*)gc_alloc(l + 1, VAL_STRING);
            memcpy(no->path, o->path, l + 1);
        } else no->path = NULL;
        Value r = v; r.v.folder_obj = no;
        return r;
    }

    /* ---- file：复制对象 + path/mode/content（内存文件内容独立） ---- */
    case VAL_FILE: {
        FileObj* o = (FileObj*)v.v.file_obj;
        if(!o) return v;
        FileObj* no = (FileObj*)gc_alloc(sizeof(FileObj), VAL_FILE);
        no->stack_alloc = 0;
        if(o->path) {
            size_t l = strlen(o->path);
            no->path = (char*)gc_alloc(l + 1, VAL_STRING);
            memcpy(no->path, o->path, l + 1);
        } else no->path = NULL;
        if(o->mode) {
            size_t l = strlen(o->mode);
            no->mode = (char*)gc_alloc(l + 1, VAL_STRING);
            memcpy(no->mode, o->mode, l + 1);
        } else no->mode = NULL;
        if(o->content && o->contentLen > 0) {
            no->content = (uint8_t*)gc_alloc((size_t)o->contentLen + 1, VAL_BYTES);
            memcpy(no->content, o->content, (size_t)o->contentLen);
            no->content[o->contentLen] = 0;
            no->contentLen = o->contentLen;
        } else { no->content = NULL; no->contentLen = 0; }
        Value r = v; r.v.file_obj = no;
        return r;
    }

    /* ---- error：复制三个字符串 ---- */
    case VAL_ERROR: {
        Value r = v;
        ValueError* ne = (ValueError*)malloc(sizeof(ValueError));
        const char* t = v.v.err.type; const char* m = v.v.err.message; const char* s = v.v.err.stack;
        ne->type = t ? strdup(t) : NULL;
        ne->message = m ? strdup(m) : NULL;
        ne->stack = s ? strdup(s) : NULL;
        r.v.err = *ne;
        free(ne);
        return r;
    }

    default:
        return v;
    }
}

/* ===== 分发核心 ===== */

/* socket 构造简写：按 config map 字段自动构造并 connect/bind+listen。
 * config 字段（可选）：
 *   TCP/UDP：host(string, 默认 127.0.0.1)、port(int)、listen(bool)、backlog(int)
 *   Unix 流/数据报：path(string)、listen(bool)、backlog(int)
 * listen=true → 服务端（TCP/Unix流：bind+listen；UDP/Unix数据报：bind）
 * listen=false/缺省 → 客户端（TCP/Unix流：connect；UDP：connect 设默认对端） */
static Value socket_ctor_from_config(int kind, Value config) {
    Value v = lumyr_socket_make(kind);
    if(v.type != VAL_SOCKET) return v;
    int is_unix = (kind == SOCK_KIND_UNIX_STREAM || kind == SOCK_KIND_UNIX_DGRAM);
    Value vListen = lumyr_map_get(config, lumyr_make_string("listen"));
    int listen = lumyr_to_bool(vListen) ? 1 : 0;
    int backlog = 128;
    Value vBack = lumyr_map_get(config, lumyr_make_string("backlog"));
    if(vBack.type != VAL_NONE) backlog = (int)bi_num_i64(vBack);
    if(is_unix) {
        Value vPath = lumyr_map_get(config, lumyr_make_string("path"));
        if(vPath.type == VAL_STRING) {
            const char* path = lumyr_str_cstr(&vPath);
            if(listen) {
                lumyr_socket_bind(v, path, 0);
                if(kind == SOCK_KIND_UNIX_STREAM) lumyr_socket_listen(v, backlog);
            } else {
                lumyr_socket_connect(v, path, 0);
            }
        }
    } else {
        Value vHost = lumyr_map_get(config, lumyr_make_string("host"));
        const char* host = (vHost.type == VAL_STRING) ? lumyr_str_cstr(&vHost) : "127.0.0.1";
        Value vPort = lumyr_map_get(config, lumyr_make_string("port"));
        int port = (vPort.type != VAL_NONE) ? (int)bi_num_i64(vPort) : 0;
        if(listen) {
            lumyr_socket_bind(v, host, port);
            if(kind == SOCK_KIND_TCP) lumyr_socket_listen(v, backlog);
        } else {
            lumyr_socket_connect(v, host, port);
        }
    }
    return v;
}

/* builtin_dispatch：id=BuiltinId；argv[0]=receiver；
 * is_method=1（OPC_CALL_BUILTIN_METHOD）argc 不含 receiver；
 * is_method=0（OPC_BUILTIN 全局形式）argv[0]=首参即 receiver，argc 含它 */
int builtin_dispatch(VMExecCtx* ctx, int id, Value* argv, int argc, Value* out, int is_method) {
    *out = val_none();
    Value recv = argv[0];

    /* 用户方法优先：receiver 是 struct/class 实例且声明了与该内置同名的用户方法时，
     * 构造 bound method 调用用户实现（用户方法允许覆盖内置，如实例自定义 sum/get/set）。
     * 仅方法形式；BUILTIN_TYPE 对实例本就有正确语义，不拦截。
     * 未命中用户方法则落回常规分派（保持既有报错行为）。 */
    if(is_method == 1 && id != BUILTIN_TYPE &&
       (recv.type == VAL_STRUCT_PTR || recv.type == VAL_CLASS_PTR)) {
        const char* mname = builtin_id_name(id);
        Value bm;
        if(mname && vm_make_bound_method(recv, mname, &bm)) {
            return vm_call_func_value(ctx, bm, argc, argv + 1, out);
        }
    }

    switch(id) {
    /* ===== 通用 ===== */
    case BUILTIN_LEN: {
        if(recv.type == VAL_FILE) {
            /* file 长度 = 行数（同 .lines 字段） */
            *out = lumyr_file_field(recv, "lines");
            return 1;
        }
        if(recv.type == VAL_FOLDER) {
            /* folder 长度 = 直接子条目数（同 .count 字段） */
            *out = lumyr_folder_field(recv, "count");
            return 1;
        }
        if(recv.type != VAL_STRING && recv.type != VAL_ARRAY &&
           recv.type != VAL_TYPED_ARRAY && recv.type != VAL_MAP &&
           recv.type != VAL_TUPLE && recv.type != VAL_SET &&
           recv.type != VAL_BYTES && recv.type != VAL_FORMDATA)
            return bi_type_err("len", recv);
        *out = lumyr_make_int64(bi_len_of(recv));
        return 1;
    }
    case BUILTIN_TYPE:
        *out = lumyr_type(recv);
        return 1;
    /* ===== HTTP 请求（requests 内置模块；全局形式 argv[0]=url） ===== */
    case BUILTIN_HTTP_GET: case BUILTIN_HTTP_POST: case BUILTIN_HTTP_PUT:
    case BUILTIN_HTTP_DELETE: case BUILTIN_HTTP_HEAD: case BUILTIN_HTTP_PATCH: {
        const char* method;
        switch(id) {
            case BUILTIN_HTTP_GET:    method = "GET"; break;
            case BUILTIN_HTTP_POST:   method = "POST"; break;
            case BUILTIN_HTTP_PUT:    method = "PUT"; break;
            case BUILTIN_HTTP_DELETE: method = "DELETE"; break;
            case BUILTIN_HTTP_HEAD:   method = "HEAD"; break;
            default:                  method = "PATCH"; break;
        }
        if(!bi_need_args(method, argc, 1)) return 0;
        Value config = argc >= 2 ? argv[1] : val_none();
        Value result = lumyr_http_request(method, argv[0], config);
        /* C 运行时错误以 VAL_ERROR 返回：走协作式 throw（同层跳 catch，
         * 否则启动跨帧展开），随后返回 VM_LOOP_UNWIND 停止当前分派 */
        if(result.type == VAL_ERROR) {
            vm_except_throw_value(ctx, result);
            return VM_LOOP_UNWIND;
        }
        *out = result;
        return 1;
    }
    /* ===== FormData 字面量构造（内部 builtin） ===== */
    case BUILTIN_FORMDATA_NEW:
        *out = lumyr_formdata_make(4);
        return 1;
    case BUILTIN_FORMDATA_APPEND: {
        if(!bi_need_args("formdata", argc, 3)) return 0;
        if(argv[0].type != VAL_FORMDATA)
            return bi_type_err("formdata.append", argv[0]);
        if(!lumyr_formdata_add(argv[0], argv[1], argv[2])) return 0;
        /* 返回 fd 本身：调用方逐字段复用（fd,name,value 弹 3 压 1，栈顶始终是 fd） */
        *out = argv[0];
        return 1;
    }
    /* ===== toMap/toArray/toJSONString：formdata + struct/class/type 实例 ===== */
    case BUILTIN_TOMAP:
        if(recv.type == VAL_FORMDATA) { *out = bi_formdata_to_map(recv); return 1; }
        if(recv.type == VAL_STRUCT_PTR || recv.type == VAL_CLASS_PTR) {
            *out = bi_instance_to_map(recv);
            return 1;
        }
        if(recv.type == VAL_MAP) { *out = recv; return 1; } /* type 形状实例本质是 map */
        return bi_type_err("toMap", recv);
    case BUILTIN_TOARRAY:
        if(recv.type == VAL_FORMDATA) {
            /* [[name, v], ...]：与 <formdata>[[k,v],...] 字面量互转，保留重复名 */
            FormDataObj* o = recv.v.formdata_obj;
            int n = o ? o->len : 0;
            Value r = val_array(n);
            for(int i = 0; i < n; i++) {
                Value pair = val_array(2);
                pair.v.array->items[0] = lumyr_make_string(o->names[i] ? o->names[i] : "");
                pair.v.array->items[1] = o->vals[i];
                r.v.array->items[i] = pair;
            }
            *out = r;
            return 1;
        }
        if(recv.type == VAL_STRUCT_PTR || recv.type == VAL_CLASS_PTR) {
            *out = bi_instance_to_array(recv);
            return 1;
        }
        if(recv.type == VAL_MAP) {
            /* map → [[k, v], ...]（type 形状实例同 map；键序哈希序） */
            Value ks = lumyr_map_keys(recv);
            ValueArray* ka = ks.v.array;
            int n = ka ? ka->len : 0;
            Value r = val_array(n);
            for(int i = 0; i < n; i++) {
                Value pair = val_array(2);
                pair.v.array->items[0] = ka->items[i];
                pair.v.array->items[1] = lumyr_map_get(recv, ka->items[i]);
                r.v.array->items[i] = pair;
            }
            *out = r;
            return 1;
        }
        return bi_type_err("toArray", recv);
    case BUILTIN_TOJSON: {
        if(recv.type == VAL_FORMDATA || recv.type == VAL_STRUCT_PTR ||
           recv.type == VAL_CLASS_PTR || recv.type == VAL_MAP || recv.type == VAL_ARRAY) {
            Value cleaned = bi_json_clean(recv);
            char* s = lumyr_json_stringify(cleaned);
            *out = s ? lumyr_make_string(s) : val_none();
            free(s);
            return 1;
        }
        return bi_type_err("toJSONString", recv);
    }
    /* copy：所有数据类型深拷贝（无额外参数） */
    case BUILTIN_COPY:
        *out = bi_copy(recv);
        return 1;
    case BUILTIN_RANGE:
        *out = lumyr_range_n(argv, argc);
        return 1;

    /* ===== 字符串组 ===== */
    case BUILTIN_SUBSTR:
        if(recv.type != VAL_STRING) return bi_type_err("substr", recv);
        if(!bi_need_args("substr", argc, 2)) return 0;
        *out = lumyr_substr(recv, argv[1], argv[2]);
        return 1;
    case BUILTIN_TOUPPER:
        if(recv.type != VAL_STRING) return bi_type_err("toupper", recv);
        *out = lumyr_toupper(recv);
        return 1;
    case BUILTIN_TOLOWER:
        if(recv.type != VAL_STRING) return bi_type_err("tolower", recv);
        *out = lumyr_tolower(recv);
        return 1;
    case BUILTIN_STRIP:
        if(recv.type != VAL_STRING) return bi_type_err("strip", recv);
        *out = lumyr_strip(recv);
        return 1;
    case BUILTIN_REPEAT:
        if(recv.type != VAL_STRING) return bi_type_err("repeat", recv);
        if(!bi_need_args("repeat", argc, 1)) return 0;
        *out = lumyr_repeat(recv, argv[1]);
        return 1;
    case BUILTIN_SPLIT:
        if(recv.type != VAL_STRING) return bi_type_err("split", recv);
        if(!bi_need_args("split", argc, 1)) return 0;
        *out = lumyr_split(recv, argv[1]);
        return 1;
    case BUILTIN_REPLACE:
        if(recv.type != VAL_STRING) return bi_type_err("replace", recv);
        if(!bi_need_args("replace", argc, 2)) return 0;
        *out = lumyr_replace(recv, argv[1], argv[2]);
        return 1;
    case BUILTIN_STARTSWITH:
        if(recv.type != VAL_STRING) return bi_type_err("startswith", recv);
        if(!bi_need_args("startswith", argc, 1)) return 0;
        *out = lumyr_startswith(recv, argv[1]);
        return 1;
    case BUILTIN_ENDSWITH:
        if(recv.type != VAL_STRING) return bi_type_err("endswith", recv);
        if(!bi_need_args("endswith", argc, 1)) return 0;
        *out = lumyr_endswith(recv, argv[1]);
        return 1;
    case BUILTIN_CHAR_AT: {
        if(recv.type != VAL_STRING) return bi_type_err("charAt", recv);
        if(!bi_need_args("charAt", argc, 1)) return 0;
        const char* s = lumyr_str_cstr(&recv);
        int64_t i = bi_num_i64(argv[1]);
        int len = lumyr_str_len(&recv);
        if(i < 0 || i >= len) {
            fprintf(stderr, "运行时错误: char_at(%lld) 越界（长度 %d）\n", (long long)i, len);
            return 0;
        }
        *out = lumyr_make_char(s[i]);
        return 1;
    }
    /* 文件 I/O 简写函数（read "path" / write "path" value 语法糖映射到此） */
    case BUILTIN_READ_FILE: {
        if(argc < 1 || argv[0].type != VAL_STRING) {
            runtime_error("read_file() 参数必须是文件路径字符串");
            return 0;
        }
        Value args[1] = { argv[0] };
        *out = lumyr_read_file(args, 1);
        return 1;
    }
    case BUILTIN_WRITE_FILE: {
        if(argc < 2) { runtime_error("write_file() 需要 (路径, 内容) 两个参数"); return 0; }
        Value args[2] = { argv[0], argv[1] };
        *out = lumyr_write_file(args, 2);
        return 1;
    }
    case BUILTIN_FILE_EXISTS: {
        if(argc < 1 || argv[0].type != VAL_STRING) {
            runtime_error("file_exists() 参数必须是文件路径字符串");
            return 0;
        }
        struct stat st;
        *out = lumyr_make_bool(stat(lumyr_str_cstr(&argv[0]), &st) == 0);
        return 1;
    }

    /* ===== 加密/编码/正则（接入 lm_crypto/lm_regex 现成封装） ===== */
    case BUILTIN_MD5: {
        if(recv.type != VAL_STRING) return bi_type_err("md5", recv);
        const char* s = lumyr_str_cstr(&recv);
        char* h = lumyr_md5_hex(s, (int)strlen(s));
        *out = lumyr_make_string(h ? h : "");
        free(h);
        return 1;
    }
    case BUILTIN_ENCODE_BASE64: {
        if(recv.type != VAL_STRING) return bi_type_err("encodeBase64", recv);
        const char* s = lumyr_str_cstr(&recv);
        char* h = lumyr_base64_encode(s, (int)strlen(s));
        *out = lumyr_make_string(h ? h : "");
        free(h);
        return 1;
    }
    case BUILTIN_DECODE_BASE64: {
        if(recv.type != VAL_STRING) return bi_type_err("decodeBase64", recv);
        const char* s = lumyr_str_cstr(&recv);
        int outlen = 0;
        char* h = lumyr_base64_decode(s, &outlen);
        if(h) h[outlen > 0 ? outlen : 0] = '\0';
        *out = lumyr_make_string(h ? h : "");
        free(h);
        return 1;
    }
    case BUILTIN_ENCODE_URL: {
        if(recv.type != VAL_STRING) return bi_type_err("encodeURL", recv);
        char* h = lumyr_url_encode(lumyr_str_cstr(&recv));
        *out = lumyr_make_string(h ? h : "");
        free(h);
        return 1;
    }
    case BUILTIN_DECODE_URL: {
        if(recv.type != VAL_STRING) return bi_type_err("decodeURL", recv);
        char* h = lumyr_url_decode(lumyr_str_cstr(&recv));
        *out = lumyr_make_string(h ? h : "");
        free(h);
        return 1;
    }
    case BUILTIN_REGEX_MATCH: {
        if(recv.type != VAL_STRING) return bi_type_err("regexMatch", recv);
        if(!bi_need_args("regexMatch", argc, 1)) return 0;
        *out = lumyr_make_bool(lumyr_regex_match(lumyr_str_cstr(&recv),
                                                 lumyr_str_cstr(&argv[1])));
        return 1;
    }
    case BUILTIN_REGEX_SEARCH: {
        if(recv.type != VAL_STRING) return bi_type_err("regexSearch", recv);
        if(!bi_need_args("regexSearch", argc, 1)) return 0;
        *out = lumyr_regex_search(lumyr_str_cstr(&recv), lumyr_str_cstr(&argv[1]));
        return 1;
    }
    case BUILTIN_REGEX_REPLACE: {
        if(recv.type != VAL_STRING) return bi_type_err("regexReplace", recv);
        if(!bi_need_args("regexReplace", argc, 2)) return 0;
        char* h = lumyr_regex_replace(lumyr_str_cstr(&recv), lumyr_str_cstr(&argv[1]),
                                      lumyr_str_cstr(&argv[2]));
        *out = lumyr_make_string(h ? h : "");
        free(h);
        return 1;
    }

    /* ===== 格式化（模板字符串 f"{x}" 编译目标） ===== */
    case BUILTIN_FORMAT: {
        /* date 族：d.format(fmt) → strftime 格式化（与全局 format_date 等价） */
        if(recv.type == VAL_DATE || recv.type == VAL_DATETIME ||
           recv.type == VAL_TIME || recv.type == VAL_TIMEDELTA) {
            if(!bi_need_args("format", argc, 1)) return 0;
            if(argv[1].type != VAL_STRING) return bi_type_err("format", argv[1]);
            char* s = lumyr_date_format(recv, lumyr_str_cstr(&argv[1]));
            *out = lumyr_make_string(s ? s : "");
            free(s);
            return 1;
        }
        /* tuple/set/bytes/complex：format(c) → 字符串化（无格式参数时） */
        if(recv.type == VAL_TUPLE || recv.type == VAL_SET ||
           recv.type == VAL_BYTES || recv.type == VAL_COMPLEX) {
            char* s = value_to_str(recv);
            *out = lumyr_make_string(s ? s : "");
            free(s);
            return 1;
        }
        if(recv.type != VAL_STRING) return bi_type_err("format", recv);
        /* receiver 为格式串。is_method=1：argc 为插值实参个数（不含 receiver）；
         * is_method=0（全局形式）：argv[0]=格式串也是 receiver，实参共 argc-1 个 */
        int total = is_method ? argc : argc - 1;
        if(total < 0) total = 0;
        Value* fargs = (Value*)malloc(sizeof(Value) * (size_t)(total + 1));
        fargs[0] = recv;
        for(int i = 0; i < total; i++) fargs[i + 1] = argv[i + 1];
        *out = lumyr_format(fargs, total + 1);
        free(fargs);
        return 1;
    }

    /* ===== 数组组 ===== */
    case BUILTIN_ARRAY_ADD: {
        /* receiver 分派：数组追加 / 字典设键值 / date 族加偏移
         * （数组/字典：原地修改并返回 self；date 族：返回同类型新对象） */
        if(recv.type == VAL_MAP) {
            if(!bi_need_args("add", argc, 2)) return 0;
            *out = lumyr_map_add(recv, argv[1], argv[2]);
            return 1;
        }
        if(recv.type == VAL_DATE || recv.type == VAL_DATETIME ||
           recv.type == VAL_TIME || recv.type == VAL_TIMEDELTA) {
            /* date 族 add(n, "days"/"months"/"years"/...) → 新对象
             * 方法形式 argc=2（n, unit）；全局形式 argc=3（recv, n, unit） */
            int need = is_method ? 2 : 3;
            if(argc < need) { runtime_error("add() 参数不足（需 n, unit）"); return 0; }
            if(argv[2].type != VAL_STRING) { runtime_error("add() 单位参数必须是字符串"); return 0; }
            *out = lumyr_date_add(recv, bi_num_i64(argv[1]), lumyr_str_cstr(&argv[2]));
            return 1;
        }
        if(recv.type == VAL_CALENDAR) {
            /* calendar add(n, "months"/"years") → 新 calendar */
            int need = is_method ? 2 : 3;
            if(argc < need) { runtime_error("add() 参数不足（需 n, unit）"); return 0; }
            if(argv[2].type != VAL_STRING) { runtime_error("add() 单位参数必须是字符串"); return 0; }
            *out = lumyr_calendar_add(recv, bi_num_i64(argv[1]), lumyr_str_cstr(&argv[2]));
            return 1;
        }
        /* set.add(x)：原地添加元素（返回 self） */
        if(recv.type == VAL_SET) {
            int need = is_method ? 1 : 2;
            if(argc < need) { runtime_error("add() 参数不足"); return 0; }
            Value arg = is_method ? argv[1] : argv[1];
            *out = lumyr_set_add(&recv, arg);
            return 1;
        }
        /* 类型化数组追加：按元素类型截断写入（原地修改，返回 self）。
         * 根因修复：此前 TypedArray 无 add 分支，报"类型 array 不支持方法 .add" */
        if(recv.type == VAL_TYPED_ARRAY) {
            if(!bi_need_args("add", argc, 1)) return 0;
            if(!recv.v.typed_array) { *out = recv; return 1; }
            bi_ta_add(recv.v.typed_array, argv[1]);
            *out = recv;
            return 1;
        }
        if(recv.type != VAL_ARRAY) return bi_type_err("add", recv);
        if(!bi_need_args("add", argc, 1)) return 0;
        *out = lumyr_array_add(&recv, argv[1]); /* 原地追加，返回 self */
        return 1;
    }
    case BUILTIN_INSERT: {
        /* 类型化数组按元素类型截断写入（原地插入，返回 self） */
        if(recv.type == VAL_TYPED_ARRAY) {
            if(!bi_need_args("insert", argc, 2)) return 0;
            if(recv.v.typed_array)
                bi_ta_insert(recv.v.typed_array, (int)bi_num_i64(argv[1]), argv[2]);
            *out = recv;
            return 1;
        }
        if(recv.type != VAL_ARRAY) return bi_type_err("insert", recv);
        if(!bi_need_args("insert", argc, 2)) return 0;
        *out = lumyr_insert(&recv, argv[1], argv[2]); /* 原地插入，返回 self */
        return 1;
    }
    case BUILTIN_ARRAY_REMOVE: {
        /* folder.remove()：无参数，递归删除目录 */
        if(recv.type == VAL_FOLDER) {
            *out = lumyr_folder_remove(recv);
            return 1;
        }
        /* 类型化数组按下标删除（与 lumyr_del 同语义），整体前移 */
        if(recv.type == VAL_TYPED_ARRAY) {
            if(!bi_need_args("remove", argc, 1)) return 0;
            if(recv.v.typed_array)
                bi_ta_remove(recv.v.typed_array, (int)bi_num_i64(argv[1]));
            *out = recv;
            return 1;
        }
        if(recv.type != VAL_ARRAY && recv.type != VAL_MAP && recv.type != VAL_SET)
            return bi_type_err("remove", recv);
        if(!bi_need_args("remove", argc, 1)) return 0;
        if(recv.type == VAL_SET)      *out = lumyr_set_remove(&recv, argv[1]);
        else if(recv.type == VAL_MAP) *out = lumyr_map_del(&recv, argv[1]);
        else                          *out = lumyr_del(&recv, argv[1]);
        return 1;
    }
    case BUILTIN_ARRAY_CLEAR: {
        /* 类型化数组清空：只重置长度，容量保留 */
        if(recv.type == VAL_TYPED_ARRAY) {
            if(recv.v.typed_array) recv.v.typed_array->len = 0;
            *out = recv;
            return 1;
        }
        if(recv.type != VAL_ARRAY && recv.type != VAL_MAP)
            return bi_type_err("clear", recv);
        if(recv.type == VAL_MAP) {
            /* 字典清空：迭代键后逐个删除 */
            Value keys = lumyr_map_keys(recv);
            int n = bi_len_of(keys);
            for(int i = 0; i < n; i++)
                lumyr_map_del(&recv, keys.v.array->items[i]);
            *out = recv;
            return 1;
        }
        *out = lumyr_array_clear(&recv);
        return 1;
    }
    case BUILTIN_ARRAY_ADDALL: {
        /* 类型化数组批量追加（VAL_ARRAY / VAL_TYPED_ARRAY 通用） */
        if(recv.type == VAL_TYPED_ARRAY) {
            if(!bi_need_args("addAll", argc, 1)) return 0;
            if(recv.v.typed_array) bi_ta_addall(recv.v.typed_array, argv[1]);
            *out = recv;
            return 1;
        }
        if(recv.type != VAL_ARRAY && recv.type != VAL_MAP)
            return bi_type_err("addAll", recv);
        if(!bi_need_args("addAll", argc, 1)) return 0;
        *out = lumyr_array_addall(&recv, argv[1]); /* 数组追加/字典合并，原地返回 self */
        return 1;
    }
    case BUILTIN_GET: {
        if(recv.type == VAL_ARRAY) {
            if(!bi_need_args("get", argc, 1)) return 0;
            *out = lumyr_array_get_safe(recv, argv[1]);
            return 1;
        }
        /* 类型化数组下标取值（越界返回 none，与数组 get_safe 同语义） */
        if(recv.type == VAL_TYPED_ARRAY) {
            if(!bi_need_args("get", argc, 1)) return 0;
            TypedArray* ta = recv.v.typed_array;
            int idx = (int)bi_num_i64(argv[1]);
            if(!ta || idx < 0 || idx >= ta->len) { *out = val_none(); return 1; }
            *out = bi_ta_read_value(ta, idx);
            return 1;
        }
        if(recv.type == VAL_MAP) {
            if(!bi_need_args("get", argc, 1)) return 0;
            *out = lumyr_map_get(recv, argv[1]);
            return 1;
        }
        if(recv.type == VAL_FORMDATA) {
            /* formdata.get(name)：取第一个同名字段值（无匹配 none） */
            if(!bi_need_args("get", argc, 1)) return 0;
            *out = lumyr_formdata_get_by_name(recv, argv[1]);
            return 1;
        }
        /* tuple/bytes.get(i)：下标访问 */
        if(recv.type == VAL_TUPLE || recv.type == VAL_BYTES) {
            if(!bi_need_args("get", argc, 1)) return 0;
            int idx = (int)bi_num_i64(argv[1]);
            *out = (recv.type == VAL_TUPLE) ? lumyr_tuple_get(recv, idx) : lumyr_bytes_get(recv, idx);
            return 1;
        }
        return bi_type_err("get", recv);
    }
    case BUILTIN_SET: {
        /* set(...) 全局构造（is_method=0）：set(1,2,3) → 新 VAL_SET */
        if(!is_method) {
            *out = lumyr_set_make(argc, argv);
            return 1;
        }
        if(recv.type == VAL_ARRAY) {
            if(!bi_need_args("set", argc, 2)) return 0;
            *out = lumyr_array_set_method(recv, argv[1], argv[2]); /* 原地写返回 self */
            return 1;
        }
        if(recv.type == VAL_MAP) {
            if(!bi_need_args("set", argc, 2)) return 0;
            lumyr_map_set(&recv, argv[1], argv[2]);
            *out = recv;
            return 1;
        }
        /* 类型化数组下标写：按元素类型截断（原地修改，返回 self） */
        if(recv.type == VAL_TYPED_ARRAY) {
            if(!bi_need_args("set", argc, 2)) return 0;
            TypedArray* ta = recv.v.typed_array;
            if(ta) {
                int idx = (int)bi_num_i64(argv[1]);
                if(idx < 0 || idx >= ta->len) {
                    char b[96];
                    snprintf(b, sizeof b, "typed array set: 下标 %d 越界（长度 %d）", idx, ta->len);
                    runtime_error(b);
                    return 0;
                }
                bi_ta_write_value(ta, idx, argv[2]);
            }
            *out = recv;
            return 1;
        }
        if(recv.type == VAL_FORMDATA) {
            /* formdata.set(name, value)：删除全部同名后写入（覆盖语义），返回 self */
            if(!bi_need_args("set", argc, 2)) return 0;
            if(!lumyr_formdata_set(recv, argv[1], argv[2])) return 0;
            *out = recv;
            return 1;
        }
        return bi_type_err("set", recv);
    }
    case BUILTIN_ARRAY_INDEXOF: {
        if(recv.type == VAL_TYPED_ARRAY) {
            /* 类型化数组按值查找：元素装箱后与数组同语义比较 */
            if(!bi_need_args("indexOf", argc, 1)) return 0;
            TypedArray* ta = recv.v.typed_array;
            int found = -1;
            if(ta) {
                for(int i = 0; i < ta->len; i++) {
                    Value eq = lumyr_eq(bi_ta_read_value(ta, i), argv[1]);
                    if(lumyr_to_bool(eq)) { found = i; break; }
                }
            }
            *out = lumyr_make_int(found);
            return 1;
        }
        if(recv.type != VAL_ARRAY) return bi_type_err("indexOf", recv);
        if(!bi_need_args("indexOf", argc, 1)) return 0;
        *out = lumyr_index_of(recv, argv[1]); /* 按值相等 */
        return 1;
    }
    case BUILTIN_OBJECT_INDEX: {
        if(recv.type != VAL_ARRAY) return bi_type_err("objectIndex", recv);
        if(!bi_need_args("objectIndex", argc, 1)) return 0;
        ValueArray* a = recv.v.array;
        int64_t found = -1;
        if(a) {
            for(int i = 0; i < a->len; i++) {
                if(bi_same_ref(a->items[i], argv[1])) { found = i; break; }
            }
        }
        *out = lumyr_make_int64(found); /* 按引用身份 */
        return 1;
    }
    case BUILTIN_ARRAY_FIRST:
        if(recv.type == VAL_TYPED_ARRAY) {
            TypedArray* ta = recv.v.typed_array;
            *out = (ta && ta->len > 0) ? bi_ta_read_value(ta, 0) : val_none();
            return 1;
        }
        if(recv.type != VAL_ARRAY) return bi_type_err("first", recv);
        *out = lumyr_array_first(recv);
        return 1;
    case BUILTIN_ARRAY_LAST:
        if(recv.type == VAL_TYPED_ARRAY) {
            TypedArray* ta = recv.v.typed_array;
            *out = (ta && ta->len > 0) ? bi_ta_read_value(ta, ta->len - 1) : val_none();
            return 1;
        }
        if(recv.type != VAL_ARRAY) return bi_type_err("last", recv);
        *out = lumyr_array_last(recv);
        return 1;
    case BUILTIN_ARRAY_FLAT: {
        if(recv.type != VAL_ARRAY) return bi_type_err("flat", recv);
        int depth = 1;
        if(argc >= 1) depth = (int)bi_num_i64(argv[1]);
        *out = lumyr_array_flat(recv, depth);
        return 1;
    }
    case BUILTIN_JOIN: {
        if(!bi_need_args("join", argc, 1)) return 0;
        if(argv[1].type != VAL_STRING) return bi_type_err("join", argv[1]);
        if(recv.type == VAL_TYPED_ARRAY) {
            /* TypedArray join：逐元素读值 → value_to_str → 分隔符拼接（与 lumyr_join 同语义） */
            TypedArray* ta = recv.v.typed_array;
            int n = ta ? ta->len : 0;
            const char* sep = lumyr_str_cstr(&argv[1]);
            size_t seplen = strlen(sep);
            size_t total = 1;
            for(int i = 0; i < n; i++) {
                char* t = value_to_str(bi_ta_read_value(ta, i));
                total += strlen(t) + (i < n - 1 ? seplen : 0);
                free(t);
            }
            char* buf = (char*)malloc(total);
            if(!buf) { perror("join"); exit(EXIT_FAILURE); }
            buf[0] = '\0';
            for(int i = 0; i < n; i++) {
                if(i > 0) strcat(buf, sep);
                char* t = value_to_str(bi_ta_read_value(ta, i));
                strcat(buf, t);
                free(t);
            }
            *out = lumyr_make_string(buf);
            free(buf);
            return 1;
        }
        if(recv.type != VAL_ARRAY) return bi_type_err("join", recv);
        *out = lumyr_join(recv, argv[1]);
        return 1;
    }

    /* ===== 数值聚合（array/TypedArray，热路径零装箱） ===== */
    case BUILTIN_SUM:
    case BUILTIN_AVG:
    case BUILTIN_MEAN: {
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY)
            return bi_type_err(id == BUILTIN_SUM ? "sum" : "avg", recv);
        int n = bi_len_of(recv);
        if(n == 0) {
            *out = (id == BUILTIN_SUM) ? lumyr_make_int64(0)
                                       : lumyr_make_double(0.0);
            return 1;
        }
        if(bi_vec_int_only(recv)) {
            int64_t s = 0;
            for(int i = 0; i < n; i++) s += bi_vec_i64(recv, i);
            if(id == BUILTIN_SUM) { *out = lumyr_make_int64(s); return 1; }
            *out = lumyr_make_double((double)s / (double)n);
            return 1;
        }
        double s = 0.0;
        for(int i = 0; i < n; i++) s += bi_vec_dbl(recv, i);
        if(id == BUILTIN_SUM) { *out = lumyr_make_double(s); return 1; }
        *out = lumyr_make_double(s / (double)n);
        return 1;
    }
    case BUILTIN_MIN:
    case BUILTIN_MAX: {
        const char* nm = (id == BUILTIN_MIN) ? "min" : "max";
        if(argc >= 2) {
            /* 全局变参形式 min(a, b, ...) */
            *out = (id == BUILTIN_MIN) ? lumyr_min(argv, argc) : lumyr_max(argv, argc);
            return 1;
        }
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY)
            return bi_type_err(nm, recv);
        int n = bi_len_of(recv);
        if(n == 0) { *out = val_none(); return 1; }
        if(recv.type == VAL_ARRAY) {
            /* 直接返回元素 Value，零拷贝 */
            int best = 0;
            double bv = bi_vec_dbl(recv, 0);
            for(int i = 1; i < n; i++) {
                double x = bi_vec_dbl(recv, i);
                if((id == BUILTIN_MIN) ? (x < bv) : (x > bv)) { bv = x; best = i; }
            }
            *out = recv.v.array->items[best];
            return 1;
        }
        /* TypedArray：装箱一次返回 */
        int best = 0;
        double bv = bi_vec_dbl(recv, 0);
        for(int i = 1; i < n; i++) {
            double x = bi_vec_dbl(recv, i);
            if((id == BUILTIN_MIN) ? (x < bv) : (x > bv)) { bv = x; best = i; }
        }
        TypedArray* ta = recv.v.typed_array;
        if(bi_is_float_et(ta->elem_type))
            *out = lumyr_make_double(bi_read_dbl_et(ta->elem_type, ta->items, best));
        else
            *out = lumyr_make_int64(bi_read_i64_et(ta->elem_type, ta->items, best));
        return 1;
    }
    case BUILTIN_ARGMAX:
    case BUILTIN_ARGMIN: {
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY)
            return bi_type_err(id == BUILTIN_ARGMAX ? "argmax" : "argmin", recv);
        int n = bi_len_of(recv);
        if(n == 0) { *out = lumyr_make_int64(-1); return 1; }
        int best = 0;
        double bv = bi_vec_dbl(recv, 0);
        for(int i = 1; i < n; i++) {
            double x = bi_vec_dbl(recv, i);
            if((id == BUILTIN_ARGMAX) ? (x > bv) : (x < bv)) { bv = x; best = i; }
        }
        *out = lumyr_make_int64(best);
        return 1;
    }
    case BUILTIN_NORM: {
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY)
            return bi_type_err("norm", recv);
        int n = bi_len_of(recv);
        double s = 0.0;
        for(int i = 0; i < n; i++) { double x = bi_vec_dbl(recv, i); s += x * x; }
        *out = lumyr_make_double(sqrt(s));
        return 1;
    }
    case BUILTIN_STD:
    case BUILTIN_VARIANCE: {
        const char* nm = (id == BUILTIN_STD) ? "std" : "var";
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY)
            return bi_type_err(nm, recv);
        int n = bi_len_of(recv);
        if(n == 0) { *out = lumyr_make_double(0.0); return 1; }
        double sum = 0.0;
        for(int i = 0; i < n; i++) sum += bi_vec_dbl(recv, i);
        double mu = sum / (double)n;
        double acc = 0.0;
        for(int i = 0; i < n; i++) {
            double d = bi_vec_dbl(recv, i) - mu;
            acc += d * d;
        }
        double var = acc / (double)n; /* 总体方差 */
        *out = lumyr_make_double((id == BUILTIN_STD) ? sqrt(var) : var);
        return 1;
    }
    case BUILTIN_NORMALIZE: {
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY)
            return bi_type_err("normalize", recv);
        int n = bi_len_of(recv);
        double norm = 0.0;
        for(int i = 0; i < n; i++) { double x = bi_vec_dbl(recv, i); norm += x * x; }
        norm = sqrt(norm);
        if(norm == 0.0) norm = 1.0;
        double* vals = (double*)malloc(sizeof(double) * (size_t)(n > 0 ? n : 1));
        for(int i = 0; i < n; i++) vals[i] = bi_vec_dbl(recv, i) / norm;
        *out = bi_vec_out(recv, VAL_DOUBLE, n, vals, 1);
        free(vals);
        return 1;
    }
    case BUILTIN_SOFTMAX: {
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY)
            return bi_type_err("softmax", recv);
        int n = bi_len_of(recv);
        if(n == 0) { *out = val_array(0); return 1; }
        double mx = bi_vec_dbl(recv, 0);
        for(int i = 1; i < n; i++) {
            double x = bi_vec_dbl(recv, i);
            if(x > mx) mx = x;
        }
        double* ex = (double*)malloc(sizeof(double) * (size_t)n);
        double sum = 0.0;
        for(int i = 0; i < n; i++) { ex[i] = exp(bi_vec_dbl(recv, i) - mx); sum += ex[i]; }
        for(int i = 0; i < n; i++) ex[i] /= sum;
        *out = bi_vec_out(recv, VAL_DOUBLE, n, ex, 1);
        free(ex);
        return 1;
    }

    /* ===== 排序/反转（原地修改并返回 self） ===== */
    case BUILTIN_SORT: {
        if(recv.type == VAL_ARRAY) {
            ValueArray* a = recv.v.array;
            if(a && a->len > 1) qsort(a->items, (size_t)a->len, sizeof(Value), bi_cmp_val);
            *out = recv;
            return 1;
        }
        if(recv.type == VAL_TYPED_ARRAY) {
            TypedArray* ta = recv.v.typed_array;
            if(ta && ta->len > 1) {
                size_t isz = lumyr_etype_itemsz(ta->elem_type);
                if(bi_is_int_et(ta->elem_type)) {
                    /* 必须按元素真实宽度比较（固定 8 字节读会读穿相邻元素） */
                    g_cmp_width = (int)isz;
                    qsort(ta->items, (size_t)ta->len, isz, bi_cmp_int_w);
                } else if(bi_is_float_et(ta->elem_type)) {
                    g_cmp_width = (int)isz;
                    qsort(ta->items, (size_t)ta->len, isz, bi_cmp_dbl_w);
                } else if(ta->elem_type == VAL_STRING) {
                    qsort(ta->items, (size_t)ta->len, isz, bi_cmp_str);
                } else {
                    fprintf(stderr, "运行时错误: TypedArray 元素类型不支持 sort\n");
                    return 0;
                }
            }
            *out = recv;
            return 1;
        }
        return bi_type_err("sort", recv);
    }
    case BUILTIN_REVERSE: {
        if(recv.type == VAL_ARRAY) {
            ValueArray* a = recv.v.array;
            if(a) {
                for(int i = 0, j = a->len - 1; i < j; i++, j--) {
                    Value t = a->items[i]; a->items[i] = a->items[j]; a->items[j] = t;
                }
            }
            *out = recv;
            return 1;
        }
        if(recv.type == VAL_TYPED_ARRAY) {
            TypedArray* ta = recv.v.typed_array;
            if(ta && ta->len > 1) {
                size_t isz = lumyr_etype_itemsz(ta->elem_type);
                char* items = (char*)ta->items;
                char* tmp = (char*)malloc(isz);
                for(int i = 0, j = ta->len - 1; i < j; i++, j--) {
                    memcpy(tmp, items + (size_t)i * isz, isz);
                    memcpy(items + (size_t)i * isz, items + (size_t)j * isz, isz);
                    memcpy(items + (size_t)j * isz, tmp, isz);
                }
                free(tmp);
            }
            *out = recv;
            return 1;
        }
        return bi_type_err("reverse", recv);
    }

    /* ===== 字典组 ===== */
    case BUILTIN_KEYS:
        if(recv.type == VAL_FORMDATA) { *out = lumyr_formdata_keys(recv); return 1; }
        if(recv.type != VAL_MAP) return bi_type_err("keys", recv);
        *out = lumyr_map_keys(recv);
        return 1;
    case BUILTIN_VALUES:
        if(recv.type == VAL_FORMDATA) { *out = lumyr_formdata_values(recv); return 1; }
        if(recv.type != VAL_MAP) return bi_type_err("values", recv);
        *out = lumyr_map_values(recv);
        return 1;
    case BUILTIN_MAP_HAS:
        if(recv.type == VAL_SET) {
            if(!bi_need_args("has", argc, 1)) return 0;
            *out = lumyr_make_bool(lumyr_set_has(recv, argv[1]));
            return 1;
        }
        if(recv.type == VAL_FORMDATA) {
            if(!bi_need_args("has", argc, 1)) return 0;
            *out = lumyr_make_bool(lumyr_formdata_has(recv, argv[1]));
            return 1;
        }
        if(recv.type != VAL_MAP) return bi_type_err("has", recv);
        if(!bi_need_args("has", argc, 1)) return 0;
        *out = lumyr_make_bool(lumyr_map_has(recv, argv[1]));
        return 1;

    /* enum 增强反查：fromValue(map, val) → 键名字符串；未找到返回 null */
    case BUILTIN_FROM_VALUE:
        if(recv.type != VAL_MAP) return bi_type_err("fromValue", recv);
        if(!bi_need_args("fromValue", argc, 1)) return 0;
        *out = lumyr_map_find_key(recv, argv[1]);
        return 1;

    /* ===== contains/indexOf 通用（string/array/map） ===== */
    case BUILTIN_CONTAINS:
        if(!bi_need_args("contains", argc, 1)) return 0;
        if(recv.type == VAL_CALENDAR) {
            *out = lumyr_calendar_contains(recv, argv[1]);
            return 1;
        }
        *out = lumyr_contains(recv, argv[1]);
        return 1;

    /* ===== 高阶函数（仅 VAL_ARRAY；回调跨边界装箱一次） ===== */
    case BUILTIN_FOREACH: {
        /* forEach(cb)：array→cb(v,i)；map→cb(k,v)；formdata→cb(name,v)（含重复名）
         * map/formdata 先取键快照，避免回调内修改容器导致指针失效 */
        if(recv.type == VAL_ARRAY) {
            ValueArray* a = recv.v.array;
            int n = a ? a->len : 0;
            for(int i = 0; i < n; i++) {
                Value fargs[2];
                Value rv;
                fargs[0] = a->items[i];
                fargs[1] = lumyr_make_int64(i);
                int rc = vm_call_func_value(ctx, argv[1], 2, fargs, &rv);
                if(rc != 1) return rc; /* 0=错误 / VM_LOOP_UNWIND 传播 */
            }
            *out = recv;
            return 1;
        }
        if(recv.type == VAL_MAP) {
            Value ks = lumyr_map_keys(recv);
            ValueArray* ka = ks.v.array;
            int n = ka ? ka->len : 0;
            for(int i = 0; i < n; i++) {
                Value fargs[2];
                Value rv;
                fargs[0] = ka->items[i];
                fargs[1] = lumyr_map_get(recv, ka->items[i]);
                int rc = vm_call_func_value(ctx, argv[1], 2, fargs, &rv);
                if(rc != 1) return rc;
            }
            *out = recv;
            return 1;
        }
        if(recv.type == VAL_FORMDATA) {
            Value ks = lumyr_formdata_keys(recv);
            Value vs = lumyr_formdata_values(recv);
            ValueArray* ka = ks.v.array;
            ValueArray* va = vs.v.array;
            int n = (ka && va) ? ka->len : 0;
            for(int i = 0; i < n; i++) {
                Value fargs[2];
                Value rv;
                fargs[0] = ka->items[i];
                fargs[1] = va->items[i];
                int rc = vm_call_func_value(ctx, argv[1], 2, fargs, &rv);
                if(rc != 1) return rc;
            }
            *out = recv;
            return 1;
        }
        return bi_type_err("forEach", recv);
    }
    case BUILTIN_GETALL:
        if(recv.type == VAL_FORMDATA) {
            if(!bi_need_args("getAll", argc, 1)) return 0;
            *out = lumyr_formdata_get_all(recv, argv[1]);
            return 1;
        }
        return bi_type_err("getAll", recv);
    case BUILTIN_MAP: {
        if(recv.type != VAL_ARRAY) return bi_type_err("map", recv);
        if(!bi_need_args("map", argc, 1)) return 0;
        ValueArray* a = recv.v.array;
        int n = a ? a->len : 0;
        Value r = val_array(n);
        for(int i = 0; i < n; i++) {
            Value fargs[1];
            Value rv;
            fargs[0] = a->items[i];
            int rc = vm_call_func_value(ctx, argv[1], 1, fargs, &rv);
            if(rc != 1) return rc; /* 0=错误 / VM_LOOP_UNWIND 传播 */
            r.v.array->items[i] = rv;
        }
        *out = r;
        return 1;
    }
    case BUILTIN_FILTER: {
        if(recv.type != VAL_ARRAY) return bi_type_err("filter", recv);
        if(!bi_need_args("filter", argc, 1)) return 0;
        ValueArray* a = recv.v.array;
        int n = a ? a->len : 0;
        Value r = val_array(n);
        int k = 0;
        for(int i = 0; i < n; i++) {
            Value fargs[1];
            Value rv;
            fargs[0] = a->items[i];
            int rc = vm_call_func_value(ctx, argv[1], 1, fargs, &rv);
            if(rc != 1) return rc;
            /* 真值判定：bool/int 直接判，double 非零判，string 非空判 */
            int truthy = 0;
            switch(rv.type) {
            case VAL_BOOL: truthy = rv.v.b != 0; break;
            case VAL_INT: truthy = rv.v.i != 0; break;
            case VAL_INT64: truthy = rv.v.i64 != 0; break;
            case VAL_DOUBLE: truthy = rv.v.d != 0.0; break;
            case VAL_STRING: truthy = lumyr_str_len(&rv) > 0; break;
            default: truthy = 1; break;
            }
            if(truthy) r.v.array->items[k++] = a->items[i];
        }
        r.v.array->len = k;
        *out = r;
        return 1;
    }
    case BUILTIN_REDUCE: {
        if(recv.type != VAL_ARRAY) return bi_type_err("reduce", recv);
        if(!bi_need_args("reduce", argc, 2)) return 0;
        ValueArray* a = recv.v.array;
        int n = a ? a->len : 0;
        Value acc = argv[2]; /* 初值 */
        for(int i = 0; i < n; i++) {
            Value fargs[2];
            Value rv;
            fargs[0] = acc;
            fargs[1] = a->items[i];
            int rc = vm_call_func_value(ctx, argv[1], 2, fargs, &rv);
            if(rc != 1) return rc;
            acc = rv;
        }
        *out = acc;
        return 1;
    }

    /* ===== AI / 线性代数 ===== */
    case BUILTIN_SHAPE: {
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY)
            return bi_type_err("shape", recv);
        int dims[BI_MAX_DIMS];
        int nd = bi_shape_of(recv, dims);
        if(nd < 0) {
            fprintf(stderr, "运行时错误: shape 嵌套超限或元素非数值\n");
            return 0;
        }
        Value r = val_array(nd);
        for(int i = 0; i < nd; i++) r.v.array->items[i] = lumyr_make_int64(dims[i]);
        /* 递归校验规则性 */
        if(nd > 0 && !bi_check_shape(recv, dims, 0, nd)) {
            fprintf(stderr, "运行时错误: shape 数组维度不规则\n");
            return 0;
        }
        *out = r;
        return 1;
    }
    case BUILTIN_RESHAPE: {
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY)
            return bi_type_err("reshape", recv);
        /* 新维度：变参 ints，或单个 int 数组 */
        int nd = 0;
        int dims[BI_MAX_DIMS];
        if(argc == 1 && argv[1].type == VAL_ARRAY) {
            ValueArray* da = argv[1].v.array;
            nd = da ? da->len : 0;
            if(nd > BI_MAX_DIMS) { fprintf(stderr, "运行时错误: reshape 维度过深\n"); return 0; }
            for(int i = 0; i < nd; i++) dims[i] = (int)bi_num_i64(da->items[i]);
        } else {
            nd = argc;
            if(nd > BI_MAX_DIMS) { fprintf(stderr, "运行时错误: reshape 维度过深\n"); return 0; }
            for(int i = 0; i < nd; i++) dims[i] = (int)bi_num_i64(argv[1 + i]);
        }
        if(nd == 0) { fprintf(stderr, "运行时错误: reshape 需要至少一个维度\n"); return 0; }
        /* 原形状校验 + 展平 */
        int od[BI_MAX_DIMS];
        int ond = bi_shape_of(recv, od);
        if(ond < 0 || (ond > 0 && !bi_check_shape(recv, od, 0, ond))) {
            fprintf(stderr, "运行时错误: reshape 数组维度不规则\n");
            return 0;
        }
        int total = 1;
        for(int i = 0; i < nd; i++) total *= dims[i];
        Value* flat = (Value*)malloc(sizeof(Value) * (size_t)(total > 0 ? total : 1));
        int k = 0;
        bi_flatten(recv, flat, &k);
        if(k != total) {
            fprintf(stderr, "运行时错误: reshape 元素数不匹配（%d 个元素 vs 新形状 %d）\n", k, total);
            free(flat);
            return 0;
        }
        int fk = 0;
        *out = bi_build_nested(flat, dims, nd, &fk, 0);
        free(flat);
        return 1;
    }
    case BUILTIN_SLICE: {
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY && recv.type != VAL_STRING)
            return bi_type_err("slice", recv);
        int n = bi_len_of(recv);
        /* 参数布局（builtin_dispatch 契约）：argv[0]=receiver，argv[1]=start，argv[2]=end；
         * argc = 用户实参个数（不含 receiver）。
         * 根因修复：此前按全局形式（argc 含 receiver）判下标，方法形式整体偏移一位
         * → slice(start, end) 的 end 永远被忽略（当成缺省 n）。 */
        int64_t start = 0;
        if(argc >= 1 && argv[1].type != VAL_NONE) start = bi_num_i64(argv[1]);
        int64_t end = n;
        if(argc >= 2 && argv[2].type != VAL_NONE) end = bi_num_i64(argv[2]);
        if(start < 0) start = 0;
        if(end > n) end = n;
        if(start > end) start = end;
        int cnt = (int)(end - start);
        if(recv.type == VAL_STRING) {
            /* 字符串切片：按字节拷贝（v1 不处理 UTF-8 多字节边界） */
            const char* s = lumyr_str_cstr(&recv);
            size_t slen = (size_t)n;
            if(!s) { *out = lumyr_make_string(""); return 1; }
            size_t blen = (start < (int64_t)slen) ? (size_t)(end - start) : 0;
            char* buf = (char*)malloc(blen + 1);
            if(blen > 0) memcpy(buf, s + (size_t)start, blen);
            buf[blen] = '\0';
            *out = lumyr_make_string(buf);
            free(buf);
            return 1;
        }
        if(recv.type == VAL_ARRAY) {
            Value r = val_array(cnt);
            for(int i = 0; i < cnt; i++) r.v.array->items[i] = recv.v.array->items[start + i];
            *out = r;
            return 1;
        }
        /* TypedArray：裸缓冲拷贝（仅数值元素类型） */
        TypedArray* ta = recv.v.typed_array;
        if(!bi_is_int_et(ta->elem_type) && !bi_is_float_et(ta->elem_type)) {
            fprintf(stderr, "运行时错误: slice 仅支持数值 TypedArray\n");
            return 0;
        }
        size_t isz = lumyr_etype_itemsz(ta->elem_type);
        char* buf = (char*)malloc(isz * (size_t)(cnt > 0 ? cnt : 1));
        if(cnt > 0) memcpy(buf, (char*)ta->items + (size_t)start * isz, isz * (size_t)cnt);
        Value r = bi_vec_out(recv, ta->elem_type, cnt, buf,
                             bi_is_float_et(ta->elem_type) ? 1 : 0);
        free(buf);
        *out = r;
        return 1;
    }
    case BUILTIN_CONCAT: {
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY)
            return bi_type_err("concat", recv);
        if(!bi_need_args("concat", argc, 1)) return 0;
        Value other = argv[1];
        if(recv.type == VAL_ARRAY && other.type == VAL_ARRAY) {
            int n1 = bi_len_of(recv), n2 = bi_len_of(other);
            Value r = val_array(n1 + n2);
            for(int i = 0; i < n1; i++) r.v.array->items[i] = recv.v.array->items[i];
            for(int i = 0; i < n2; i++) r.v.array->items[n1 + i] = other.v.array->items[i];
            *out = r;
            return 1;
        }
        if(recv.type == VAL_TYPED_ARRAY && other.type == VAL_TYPED_ARRAY &&
           recv.v.typed_array->elem_type == other.v.typed_array->elem_type) {
            TypedArray* a = recv.v.typed_array;
            TypedArray* b = other.v.typed_array;
            if(!bi_is_int_et(a->elem_type) && !bi_is_float_et(a->elem_type)) {
                fprintf(stderr, "运行时错误: concat 仅支持数值 TypedArray\n");
                return 0;
            }
            size_t isz = lumyr_etype_itemsz(a->elem_type);
            char* buf = (char*)malloc(isz * (size_t)(a->len + b->len));
            if(a->len) memcpy(buf, a->items, isz * (size_t)a->len);
            if(b->len) memcpy(buf + isz * (size_t)a->len, b->items, isz * (size_t)b->len);
            *out = bi_vec_out(recv, a->elem_type, a->len + b->len, buf,
                              bi_is_float_et(a->elem_type) ? 1 : 0);
            free(buf);
            return 1;
        }
        fprintf(stderr, "运行时错误: concat 两边数组种类/元素类型不一致\n");
        return 0;
    }
    case BUILTIN_DOT: {
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY)
            return bi_type_err("dot", recv);
        if(!bi_need_args("dot", argc, 1)) return 0;
        Value other = argv[1];
        if(other.type != VAL_ARRAY && other.type != VAL_TYPED_ARRAY) {
            fprintf(stderr, "运行时错误: dot 参数必须是数组\n");
            return 0;
        }
        int n = bi_len_of(recv);
        if(n != bi_len_of(other)) {
            fprintf(stderr, "运行时错误: dot 维度不匹配（%d vs %d）\n", n, bi_len_of(other));
            return 0;
        }
        if(bi_vec_int_only(recv) && bi_vec_int_only(other)) {
            int64_t s = 0;
            for(int i = 0; i < n; i++)
                s += bi_vec_i64(recv, i) * bi_vec_i64(other, i);
            *out = lumyr_make_int64(s);
            return 1;
        }
        double s = 0.0;
        for(int i = 0; i < n; i++) s += bi_vec_dbl(recv, i) * bi_vec_dbl(other, i);
        *out = lumyr_make_double(s);
        return 1;
    }
    case BUILTIN_MATMUL: {
        if(recv.type != VAL_ARRAY) return bi_type_err("matmul", recv);
        if(!bi_need_args("matmul", argc, 1)) return 0;
        Value B = argv[1];
        int m, k1, k2, p;
        if(!bi_mat_dims(recv, &m, &k1) || !bi_mat_dims(B, &k2, &p)) {
            fprintf(stderr, "运行时错误: matmul 需要 2D 矩阵\n");
            return 0;
        }
        if(k1 != k2) {
            fprintf(stderr, "运行时错误: matmul 内维不匹配（%d vs %d）\n", k1, k2);
            return 0;
        }
        int int_only = bi_vec_int_only(recv) && bi_vec_int_only(B);
        Value r = val_array(m);
        for(int i = 0; i < m; i++) {
            Value row = val_array(p);
            for(int j = 0; j < p; j++) {
                double acc = 0.0;
                int64_t iacc = 0;
                for(int t = 0; t < k1; t++) {
                    if(int_only) {
                        iacc += bi_mat_i64(recv, i, t) * bi_mat_i64(B, t, j);
                    } else {
                        double x = bi_mat_read(recv, i, t) * bi_mat_read(B, t, j);
                        acc += x;
                    }
                }
                row.v.array->items[j] = int_only ? lumyr_make_int64(iacc)
                                                 : lumyr_make_double(acc);
            }
            r.v.array->items[i] = row;
        }
        *out = r;
        return 1;
    }
    case BUILTIN_TRANSPOSE: {
        if(recv.type != VAL_ARRAY) return bi_type_err("transpose", recv);
        int rows, cols;
        if(!bi_mat_dims(recv, &rows, &cols)) {
            fprintf(stderr, "运行时错误: transpose 需要 2D 矩阵\n");
            return 0;
        }
        Value r = val_array(cols);
        int int_only = bi_vec_int_only(recv);
        for(int i = 0; i < cols; i++) {
            Value row = val_array(rows);
            for(int j = 0; j < rows; j++) {
                row.v.array->items[j] = int_only ? lumyr_make_int64(bi_mat_i64(recv, j, i))
                                                 : lumyr_make_double(bi_mat_read(recv, j, i));
            }
            r.v.array->items[i] = row;
        }
        *out = r;
        return 1;
    }
    case BUILTIN_DET: {
        if(recv.type != VAL_ARRAY) return bi_type_err("det", recv);
        int n, m;
        if(!bi_mat_dims(recv, &n, &m) || n != m) {
            fprintf(stderr, "运行时错误: det 需要方阵\n");
            return 0;
        }
        /* 高斯消元（部分主元），double */
        double* a = (double*)malloc(sizeof(double) * (size_t)(n * n));
        for(int i = 0; i < n; i++)
            for(int j = 0; j < n; j++) a[i * n + j] = bi_mat_read(recv, i, j);
        double det = 1.0;
        for(int col = 0; col < n; col++) {
            int piv = col;
            for(int r = col + 1; r < n; r++)
                if(fabs(a[r * n + col]) > fabs(a[piv * n + col])) piv = r;
            if(fabs(a[piv * n + col]) < 1e-300) { det = 0.0; break; }
            if(piv != col) {
                for(int j = 0; j < n; j++) {
                    double t = a[col * n + j];
                    a[col * n + j] = a[piv * n + j];
                    a[piv * n + j] = t;
                }
                det = -det;
            }
            det *= a[col * n + col];
            for(int r = col + 1; r < n; r++) {
                double f = a[r * n + col] / a[col * n + col];
                for(int j = col; j < n; j++) a[r * n + j] -= f * a[col * n + j];
            }
        }
        free(a);
        *out = lumyr_make_double(det);
        return 1;
    }
    case BUILTIN_INV: {
        if(recv.type != VAL_ARRAY) return bi_type_err("inv", recv);
        int n, m;
        if(!bi_mat_dims(recv, &n, &m) || n != m) {
            fprintf(stderr, "运行时错误: inv 需要方阵\n");
            return 0;
        }
        /* Gauss-Jordan 消元求逆 */
        double* a = (double*)malloc(sizeof(double) * (size_t)(n * n));
        double* iv = (double*)malloc(sizeof(double) * (size_t)(n * n));
        for(int i = 0; i < n; i++) {
            for(int j = 0; j < n; j++) {
                a[i * n + j] = bi_mat_read(recv, i, j);
                iv[i * n + j] = (i == j) ? 1.0 : 0.0;
            }
        }
        int ok = 1;
        for(int col = 0; col < n && ok; col++) {
            int piv = col;
            for(int r = col + 1; r < n; r++)
                if(fabs(a[r * n + col]) > fabs(a[piv * n + col])) piv = r;
            if(fabs(a[piv * n + col]) < 1e-300) ok = 0;
            if(!ok) break;
            if(piv != col) {
                for(int j = 0; j < n; j++) {
                    double t = a[col * n + j]; a[col * n + j] = a[piv * n + j]; a[piv * n + j] = t;
                    t = iv[col * n + j]; iv[col * n + j] = iv[piv * n + j]; iv[piv * n + j] = t;
                }
            }
            double d = a[col * n + col];
            for(int j = 0; j < n; j++) { a[col * n + j] /= d; iv[col * n + j] /= d; }
            for(int r = 0; r < n; r++) {
                if(r == col) continue;
                double f = a[r * n + col];
                if(f == 0.0) continue;
                for(int j = 0; j < n; j++) {
                    a[r * n + j] -= f * a[col * n + j];
                    iv[r * n + j] -= f * iv[col * n + j];
                }
            }
        }
        if(!ok) {
            fprintf(stderr, "运行时错误: inv 矩阵奇异（不可逆）\n");
            free(a); free(iv);
            return 0;
        }
        Value r = val_array(n);
        for(int i = 0; i < n; i++) {
            r.v.array->items[i] = bi_inv_row_like(recv.v.array->items[i],
                                                  &iv[i * n], n);
        }
        free(a); free(iv);
        *out = r;
        return 1;
    }

    /* ===== 数学（全局形式透传） ===== */
    case BUILTIN_FLOOR:
        *out = lumyr_floor(recv);
        return 1;
    case BUILTIN_CEIL:
        *out = lumyr_ceil(recv);
        return 1;
    case BUILTIN_ABS:
        /* complex.abs() → 模 |c|（double） */
        if(recv.type == VAL_COMPLEX) {
            *out = lumyr_make_double(lumyr_complex_abs(recv));
            return 1;
        }
        *out = lumyr_abs(recv);
        return 1;
    case BUILTIN_SQRT:
        *out = lumyr_sqrt(recv);
        return 1;
    /* ===== 三角/反三角/对数/指数（1~2 参，返回 double） ===== */
    case BUILTIN_SIN:    *out = lumyr_sin(recv);    return 1;
    case BUILTIN_COS:    *out = lumyr_cos(recv);    return 1;
    case BUILTIN_TAN:    *out = lumyr_tan(recv);    return 1;
    case BUILTIN_ASIN:   *out = lumyr_asin(recv);   return 1;
    case BUILTIN_ACOS:   *out = lumyr_acos(recv);   return 1;
    case BUILTIN_ATAN:   *out = lumyr_atan(recv);   return 1;
    case BUILTIN_ATAN2:
        if(!bi_need_args("atan2", argc, 2)) return 0;
        *out = lumyr_atan2(recv, argv[1]);
        return 1;
    case BUILTIN_LOG:    *out = lumyr_ln(recv);    return 1;
    case BUILTIN_LOG10:  *out = lumyr_log10(recv);  return 1;
    case BUILTIN_LOG2:   *out = lumyr_log2(recv);   return 1;
    case BUILTIN_EXP:    *out = lumyr_exp(recv);    return 1;
    case BUILTIN_POW:
        if(!bi_need_args("pow", argc, 2)) return 0;
        *out = lumyr_pow(recv, argv[1]);
        return 1;
    case BUILTIN_ROUND:  *out = lumyr_round(recv);  return 1;
    case BUILTIN_CBRT:   *out = lumyr_cbrt(recv);   return 1;
    case BUILTIN_HYPOT:
        if(!bi_need_args("hypot", argc, 2)) return 0;
        *out = lumyr_hypot(recv, argv[1]);
        return 1;
    case BUILTIN_SIGN:   *out = lumyr_sign(recv);   return 1;
    case BUILTIN_DEGREES: *out = lumyr_degrees(recv); return 1;
    case BUILTIN_RADIANS: *out = lumyr_radians(recv); return 1;
    case BUILTIN_TRUNC:  *out = lumyr_trunc(recv);  return 1;
    case BUILTIN_RANDOM: *out = lumyr_random(); return 1;
    case BUILTIN_DEL:
        if(!bi_need_args("del", argc, 1)) return 0;
        *out = lumyr_del(&recv, argv[1]);
        return 1;

    /* ===== 生成器 ===== */
    case BUILTIN_NEXT: {
        /* next(gen)：恢复生成器执行；返回 yield 值；结束返回 null */
        if(!bi_need_args("next", argc, 1)) return 0;
        if(recv.type != VAL_GENERATOR || !recv.v.generator)
            return bi_type_err("next", recv);
        GeneratorObject* gen = (GeneratorObject*)recv.v.generator;
        Value result;
        int ok = wrapped_gen_next(gen, &result, ctx->frame, NULL);
        (void)ok;  /* 0 = 生成器结束，结果为 NONE；1 = 正常 yield */
        *out = result;
        return 1;
    }
    case BUILTIN_SEND: {
        /* socket 分支：s.send(data [, flags]) → 实际发送字节数
         * data 为 bytes 时按原始字节发送（二进制安全，可含 NUL）；否则 value_to_str */
        if(recv.type == VAL_SOCKET) {
            int nuser = is_method ? argc : argc - 1;
            if(nuser < 1) { runtime_error("send(data [, flags]) 至少需要 1 个参数"); return 0; }
            int flags = (nuser >= 2) ? (int)bi_num_i64(argv[2]) : 0;
            Value dataArg = argv[1];
            if(dataArg.type == VAL_BYTES) {
                BytesObj* b = (BytesObj*)dataArg.v.bytes_obj;
                const char* d = b ? (const char*)b->data : "";
                int dlen = b ? b->len : 0;
                *out = lumyr_socket_send(recv, d, dlen, flags);
            } else {
                char* s = value_to_str(dataArg);
                *out = lumyr_socket_send(recv, s, -1, flags);
                free(s);
            }
            return 1;
        }
        /* send(gen, val)：向生成器发送值，返回下一个 yield 值 */
        if(!bi_need_args("send", argc, 2)) return 0;
        if(recv.type != VAL_GENERATOR || !recv.v.generator)
            return bi_type_err("send", recv);
        GeneratorObject* gen = (GeneratorObject*)recv.v.generator;
        Value result;
        int ok = generator_resume(gen, &result, &argv[1], ctx->frame, NULL);
        (void)ok;
        *out = result;
        return 1;
    }
    case BUILTIN_RECEIVE: {
        /* receive()：在生成器内获取 send() 发送的值 */
        GeneratorObject* gen = vm_get_current_generator();
        if(!gen) {
            fprintf(stderr, "VM: receive() 必须在生成器函数内调用\n");
            return 0;
        }
        *out = gen->send_value;
        return 1;
    }
    case BUILTIN_CLOSE: {
        /* socket 分支：s.close() → 关闭套接字 */
        if(recv.type == VAL_SOCKET) {
            *out = lumyr_socket_close(recv);
            return 1;
        }
        /* close(gen)：关闭生成器，释放资源 */
        if(!bi_need_args("close", argc, 1)) return 0;
        if(recv.type != VAL_GENERATOR || !recv.v.generator)
            return bi_type_err("close", recv);
        GeneratorObject* gen = (GeneratorObject*)recv.v.generator;
        gen->finished = 1;
        *out = val_none();
        return 1;
    }

    case BUILTIN_ASSERT: {
        /* __assert(cond [, msg])：cond 为假时打印 msg 并 exit(1) */
        if(argc < 1) { *out = val_none(); return 1; }
        _Bool ok = lumyr_to_bool(argv[0]);
        if(!ok) {
            const char* msg = "assertion failed";
            if(argc >= 2 && argv[1].type == VAL_STRING) {
                const char* m = lumyr_str_cstr(&argv[1]);
                if(m) msg = m;
            }
            fprintf(stderr, "Assertion failed: %s\n", msg);
            exit(1);
        }
        *out = val_none();
        return 1;
    }

    /* ===== 对象二进制序列化（内部名，供 ObjectStream .lm 层调用） ===== */
    case BUILTIN_LM_SERIALIZE: {
        if(argc < 1) { runtime_error("__lmSerialize 需要 1 个参数"); return 0; }
        *out = lm_serialize_value(argv[0]);
        return 1;
    }
    case BUILTIN_LM_DESERIALIZE: {
        if(argc < 2) { runtime_error("__lmDeserialize 需要 2 个参数"); return 0; }
        if(argv[0].type != VAL_BYTES) { runtime_error("__lmDeserialize 首参必须是 bytes"); return 0; }
        Value val = val_none();
        int newOff = lm_deserialize_value_at(argv[0], (int)bi_num_i64(argv[1]), &val);
        Value r = val_array(2);
        r.v.array->items[0] = val;
        r.v.array->items[1] = val_int((long long)newOff);
        *out = r;
        return 1;
    }
    case BUILTIN_LM_BUILD_STREAM: {
        if(argc < 1) { runtime_error("__lmBuildStream 需要 1 个参数"); return 0; }
        *out = lm_build_stream(argv[0]);
        return 1;
    }
    case BUILTIN_LM_CHECK_HEADER: {
        if(argc < 1) { runtime_error("__lmCheckHeader 需要 1 个参数"); return 0; }
        *out = val_int((long long)lm_check_stream_header(argv[0]));
        return 1;
    }

    /* ===== date 族构造（接受 ISO 字符串 或 多个整数参数） ===== */
    case BUILTIN_DATE_MAKE: {
        /* 全局形式 date(...)：argv[0] 是第 1 个实参
         *   date("2026-09-22" [, tz])  → ISO 字符串构造
         *   date(epoch [, tz])         → 整数 epoch 构造
         *   date(y, m, d [, tz])       → 日历字段构造
         *   tz：时区偏移分钟（0=UTC，480=UTC+8），省略=本地时区 */
        if(argc < 1) { runtime_error("date() 至少需要 1 个参数"); return 0; }
        Value a0 = argv[0];
        int32_t tz = INT32_MIN;  // 默认本地
        if(a0.type == VAL_STRING) {
            int y = 0, mo = 0, d = 0;
            if(sscanf(lumyr_str_cstr(&a0), "%d-%d-%d", &y, &mo, &d) != 3) {
                runtime_error("date() ISO 字符串格式错误（需 YYYY-MM-DD）");
                return 0;
            }
            if(argc >= 2) tz = (int32_t)bi_num_i64(argv[1]);
            *out = lumyr_make_date_ymd_tz(y, mo, d, tz);
            return 1;
        }
        if(argc == 1) {
            *out = lumyr_make_date(bi_num_i64(a0), tz);
            return 1;
        }
        if(argc == 2) {
            // date(epoch, tz)
            tz = (int32_t)bi_num_i64(argv[1]);
            *out = lumyr_make_date(bi_num_i64(a0), tz);
            return 1;
        }
        if(argc >= 3) {
            // date(y, m, d [, tz])
            if(argc >= 4) tz = (int32_t)bi_num_i64(argv[3]);
            *out = lumyr_make_date_ymd_tz((int)bi_num_i64(a0), (int)bi_num_i64(argv[1]), (int)bi_num_i64(argv[2]), tz);
            return 1;
        }
        runtime_error("date() 参数个数错误（需 ISO 字符串 / epoch / y,m,d）");
        return 0;
    }
    case BUILTIN_DATETIME_MAKE: {
        /* datetime("..." [, tz])
         * datetime(epoch [, nsec [, tz]])
         * datetime(y, m, d, h, mi, s [, ns [, tz]]) */
        if(argc < 1) { runtime_error("datetime() 至少需要 1 个参数"); return 0; }
        Value a0 = argv[0];
        int32_t tz = INT32_MIN;
        if(a0.type == VAL_STRING) {
            int y=0,mo=0,d=0,h=0,mi=0,s=0; int ns=0;
            int n = sscanf(lumyr_str_cstr(&a0), "%d-%d-%dT%d:%d:%d.%d", &y,&mo,&d,&h,&mi,&s,&ns);
            if(n < 6) n = sscanf(lumyr_str_cstr(&a0), "%d-%d-%d %d:%d:%d.%d", &y,&mo,&d,&h,&mi,&s,&ns);
            if(n < 6) {
                runtime_error("datetime() ISO 字符串格式错误（需 YYYY-MM-DDTHH:MM:SS）");
                return 0;
            }
            if(argc >= 2) tz = (int32_t)bi_num_i64(argv[1]);
            *out = lumyr_make_datetime_ymd_tz(y, mo, d, h, mi, s, ns, tz);
            return 1;
        }
        if(argc == 1) {
            *out = lumyr_make_datetime(bi_num_i64(a0), 0, tz);
            return 1;
        }
        if(argc == 2) {
            *out = lumyr_make_datetime(bi_num_i64(a0), (int32_t)bi_num_i64(argv[1]), tz);
            return 1;
        }
        if(argc == 3) {
            // datetime(epoch, nsec, tz)
            tz = (int32_t)bi_num_i64(argv[2]);
            *out = lumyr_make_datetime(bi_num_i64(a0), (int32_t)bi_num_i64(argv[1]), tz);
            return 1;
        }
        if(argc >= 6) {
            int ns = (argc >= 7) ? (int)bi_num_i64(argv[6]) : 0;
            if(argc >= 8) tz = (int32_t)bi_num_i64(argv[7]);
            else if(argc == 7) { /* 7 = y,m,d,h,mi,s,ns → local */ }
            *out = lumyr_make_datetime_ymd_tz(
                (int)bi_num_i64(a0), (int)bi_num_i64(argv[1]), (int)bi_num_i64(argv[2]),
                (int)bi_num_i64(argv[3]), (int)bi_num_i64(argv[4]), (int)bi_num_i64(argv[5]),
                ns, tz);
            return 1;
        }
        runtime_error("datetime() 参数个数错误");
        return 0;
    }
    case BUILTIN_TIME_MAKE: {
        /* time("..." [, tz])
         * time(sec [, nsec])           → 本地时区（epoch 形式不支持 tz）
         * time(h, m, s [, ns [, tz]]) */
        if(argc < 1) { runtime_error("time() 至少需要 1 个参数"); return 0; }
        Value a0 = argv[0];
        int32_t tz = INT32_MIN;
        if(a0.type == VAL_STRING) {
            int h=0,mi=0,s=0; int ns=0;
            int n = sscanf(lumyr_str_cstr(&a0), "%d:%d:%d.%d", &h,&mi,&s,&ns);
            if(n < 3) {
                runtime_error("time() ISO 字符串格式错误（需 HH:MM:SS）");
                return 0;
            }
            if(argc >= 2) tz = (int32_t)bi_num_i64(argv[1]);
            *out = lumyr_make_time_hms_tz(h, mi, s, ns, tz);
            return 1;
        }
        if(argc == 1) {
            *out = lumyr_make_time_obj((int32_t)bi_num_i64(a0), 0, tz);
            return 1;
        }
        if(argc == 2) {
            *out = lumyr_make_time_obj((int32_t)bi_num_i64(a0), (int32_t)bi_num_i64(argv[1]), tz);
            return 1;
        }
        if(argc >= 3) {
            // time(h, m, s [, ns [, tz]])
            int ns = (argc >= 4) ? (int)bi_num_i64(argv[3]) : 0;
            if(argc >= 5) tz = (int32_t)bi_num_i64(argv[4]);
            *out = lumyr_make_time_hms_tz((int)bi_num_i64(a0), (int)bi_num_i64(argv[1]), (int)bi_num_i64(argv[2]), ns, tz);
            return 1;
        }
        runtime_error("time() 参数个数错误");
        return 0;
    }
    case BUILTIN_TIMEDELTA_MAKE: {
        /* timedelta("1 day 02:03:04") / timedelta(sec [, nsec]) */
        if(argc < 1) { runtime_error("timedelta() 至少需要 1 个参数"); return 0; }
        Value a0 = argv[0];
        if(a0.type == VAL_STRING) {
            /* 简化解析：支持 "d day(s) hh:mm:ss" / "hh:mm:ss" / 纯秒数 */
            int days=0, h=0, mi=0, s=0;
            long long sec;
            const char* str = lumyr_str_cstr(&a0);
            int n = sscanf(str, "%d day(s) %d:%d:%d", &days, &h, &mi, &s);
            if(n == 4) {
                sec = (long long)days * 86400 + h * 3600 + mi * 60 + s;
                *out = lumyr_make_timedelta(sec, 0);
                return 1;
            }
            n = sscanf(str, "%d:%d:%d", &h, &mi, &s);
            if(n == 3) {
                sec = h * 3600 + mi * 60 + s;
                *out = lumyr_make_timedelta(sec, 0);
                return 1;
            }
            if(sscanf(str, "%lld", &sec) == 1) {
                *out = lumyr_make_timedelta(sec, 0);
                return 1;
            }
            runtime_error("timedelta() ISO 字符串格式错误");
            return 0;
        }
        if(argc == 1) {
            *out = lumyr_make_timedelta(bi_num_i64(a0), 0);
            return 1;
        }
        *out = lumyr_make_timedelta(bi_num_i64(a0), (int32_t)bi_num_i64(argv[1]));
        return 1;
    }
    case BUILTIN_NOW:
        /* now()：当前 UTC 时间 → VAL_DATETIME */
        *out = lumyr_date_now();
        return 1;
    case BUILTIN_TODAY:
        /* today()：当前 UTC 日期 → VAL_DATE */
        *out = lumyr_date_today();
        return 1;

    /* ===== date 族字段访问（方法形式 d.year；全局形式 year(d)） ===== */
    case BUILTIN_YEAR:
    case BUILTIN_MONTH:
    case BUILTIN_DAY:
    case BUILTIN_HOUR:
    case BUILTIN_MINUTE:
    case BUILTIN_SECOND:
    case BUILTIN_WEEKDAY:
    case BUILTIN_YEARDAY:
    case BUILTIN_DAYS:
    case BUILTIN_SECONDS:
    case BUILTIN_TOTAL_SECONDS: {
        /* 方法形式：argv[0]=receiver；全局形式：argv[0]=参数（语义相同） */
        if(argc < 1) { runtime_error("date 族字段访问至少需要 1 个参数"); return 0; }
        Value dv = argv[0];
        if(dv.type != VAL_DATE && dv.type != VAL_DATETIME &&
           dv.type != VAL_TIME && dv.type != VAL_TIMEDELTA) {
            runtime_error("date 族字段访问的接收者必须是 date/datetime/time/timedelta");
            return 0;
        }
        const char* fname = NULL;
        switch(id) {
        case BUILTIN_YEAR:         fname = "year"; break;
        case BUILTIN_MONTH:       fname = "month"; break;
        case BUILTIN_DAY:         fname = "day"; break;
        case BUILTIN_HOUR:        fname = "hour"; break;
        case BUILTIN_MINUTE:      fname = "minute"; break;
        case BUILTIN_SECOND:      fname = "second"; break;
        case BUILTIN_WEEKDAY:     fname = "weekday"; break;
        case BUILTIN_YEARDAY:     fname = "yearday"; break;
        case BUILTIN_DAYS:        fname = "days"; break;
        case BUILTIN_SECONDS:     fname = "seconds"; break;
        case BUILTIN_TOTAL_SECONDS: fname = "totalSeconds"; break;
        default: break;
        }
        *out = lumyr_date_field(dv, fname);
        return 1;
    }
    case BUILTIN_FORMAT_DATE: {
        /* 方法形式 d.format(fmt)：argc=1（fmt）；全局形式 format_date(d, fmt)：argc=2 */
        int need = is_method ? 1 : 2;
        if(argc < need) { runtime_error("format() 参数不足"); return 0; }
        Value dv = argv[0];
        Value fmtv = argv[1];
        if(dv.type != VAL_DATE && dv.type != VAL_DATETIME &&
           dv.type != VAL_TIME && dv.type != VAL_TIMEDELTA) {
            runtime_error("format() 接收者必须是 date 族对象");
            return 0;
        }
        if(fmtv.type != VAL_STRING) { runtime_error("format() 格式串必须是字符串"); return 0; }
        char* s = lumyr_date_format(dv, lumyr_str_cstr(&fmtv));
        *out = lumyr_make_string(s ? s : "");
        free(s);
        return 1;
    }
    case BUILTIN_DATE_DIFF: {
        /* 方法形式 a.diff(b)：argc=1；全局形式 diff(a, b)：argc=2 */
        int need = is_method ? 1 : 2;
        if(argc < need) { runtime_error("diff() 参数不足"); return 0; }
        Value a = argv[0], b = argv[1];
        /* set.diff(b)：差集 a-b → 新 set */
        if(a.type == VAL_SET && b.type == VAL_SET) {
            *out = lumyr_set_diff(a, b);
            return 1;
        }
        if((a.type != VAL_DATE && a.type != VAL_DATETIME && a.type != VAL_TIME && a.type != VAL_TIMEDELTA) ||
           (b.type != VAL_DATE && b.type != VAL_DATETIME && b.type != VAL_TIME && b.type != VAL_TIMEDELTA)) {
            runtime_error("diff() 参数必须是 date 族对象或 set");
            return 0;
        }
        *out = lumyr_date_diff(a, b);
        return 1;
    }
    case BUILTIN_DATE_ADD: {
        /* 方法形式 d.add(n, "days")：argc=2；全局形式 add(d, n, "days")：argc=3 */
        int need = is_method ? 2 : 3;
        if(argc < need) { runtime_error("add() 参数不足（需 n, unit）"); return 0; }
        Value dv = argv[0];
        if(dv.type != VAL_DATE && dv.type != VAL_DATETIME &&
           dv.type != VAL_TIME && dv.type != VAL_TIMEDELTA) {
            runtime_error("add() 接收者必须是 date 族对象");
            return 0;
        }
        if(argv[2].type != VAL_STRING) { runtime_error("add() 单位参数必须是字符串"); return 0; }
        *out = lumyr_date_add(dv, bi_num_i64(argv[1]), lumyr_str_cstr(&argv[2]));
        return 1;
    }
    /* ===== tuple（VAL_TUPLE） ===== */
    case BUILTIN_TUPLE_MAKE: {
        /* tuple(...)：全局形式 argc=全部参数；方法形式不适用 */
        if(is_method) { runtime_error("tuple() 不支持方法形式"); return 0; }
        *out = lumyr_tuple_make(argc, argv);
        return 1;
    }
    /* ===== set 方法（非冲突名） ===== */
    case BUILTIN_SET_UNION: {
        Value a = argv[0], b = is_method ? argv[1] : argv[1];
        if(a.type != VAL_SET || b.type != VAL_SET) { runtime_error("union() 参数必须是 set"); return 0; }
        *out = lumyr_set_union(a, b);
        return 1;
    }
    case BUILTIN_SET_INTERSECT: {
        Value a = argv[0], b = is_method ? argv[1] : argv[1];
        if(a.type != VAL_SET || b.type != VAL_SET) { runtime_error("intersect() 参数必须是 set"); return 0; }
        *out = lumyr_set_intersect(a, b);
        return 1;
    }
    /* ===== bytes（VAL_BYTES） ===== */
    case BUILTIN_BYTES_MAKE: {
        /* bytes(...)：全局形式；单参数字符串/数组或多参数整数 */
        if(is_method) { runtime_error("bytes() 不支持方法形式"); return 0; }
        *out = lumyr_bytes_make(argc, argv);
        return 1;
    }
    case BUILTIN_BYTES_HEX: {
        if(recv.type != VAL_BYTES) return bi_type_err("hex", recv);
        char* s = lumyr_bytes_hex(recv);
        *out = lumyr_make_string(s ? s : "");
        free(s);
        return 1;
    }
    case BUILTIN_BYTES_TO_STR: {
        if(recv.type != VAL_BYTES) return bi_type_err("toStr", recv);
        char* s = lumyr_bytes_to_str(recv);
        *out = lumyr_make_string(s ? s : "");
        free(s);
        return 1;
    }
    case BUILTIN_BYTES_DECODE: {
        if(recv.type != VAL_BYTES) return bi_type_err("decode", recv);
        char* s = lumyr_bytes_to_utf8(recv);
        *out = lumyr_make_string(s ? s : "");
        free(s);
        return 1;
    }
    case BUILTIN_BYTES_FROM_HEX: {
        /* from_hex(s)：全局形式，十六进制字符串 → bytes */
        if(is_method) { runtime_error("from_hex() 不支持方法形式"); return 0; }
        if(argc < 1 || argv[0].type != VAL_STRING) { runtime_error("from_hex() 参数必须是字符串"); return 0; }
        *out = lumyr_bytes_from_hex(lumyr_str_cstr(&argv[0]));
        return 1;
    }
    /* ===== complex（VAL_COMPLEX） ===== */
    case BUILTIN_COMPLEX_MAKE: {
        /* complex(re, im)：全局形式 */
        if(is_method) { runtime_error("complex() 不支持方法形式"); return 0; }
        double re = 0.0, im = 0.0;
        if(argc >= 1) re = value_as_number(argv[0]);
        if(argc >= 2) im = value_as_number(argv[1]);
        *out = lumyr_complex_make(re, im);
        return 1;
    }
    case BUILTIN_COMPLEX_CONJUGATE: {
        if(recv.type != VAL_COMPLEX) return bi_type_err("conjugate", recv);
        *out = lumyr_complex_conjugate(recv);
        return 1;
    }
    /* ===== calendar 综合日历 ===== */
    case BUILTIN_CALENDAR_MAKE: {
        /* calendar(y, m [, tz]) / calendar(date [, tz])
         * 全局形式：argv[0]=第1实参 */
        if(argc < 1) { runtime_error("calendar() 至少需要 1 个参数"); return 0; }
        int32_t tz = INT32_MIN;  // 默认本地时区
        if(argv[0].type == VAL_DATE || argv[0].type == VAL_DATETIME) {
            /* calendar(date [, tz]) */
            if(argc >= 2) tz = (int32_t)bi_num_i64(argv[1]);
            *out = lumyr_calendar_from_date(argv[0], tz);
            return 1;
        }
        /* calendar(year, month [, tz]) */
        if(argc < 2) { runtime_error("calendar() 需要 year, month 或 date"); return 0; }
        if(argc >= 3) tz = (int32_t)bi_num_i64(argv[2]);
        *out = lumyr_calendar_make((int)bi_num_i64(argv[0]), (int)bi_num_i64(argv[1]), tz);
        return 1;
    }
    case BUILTIN_CALENDAR_FIRST_DATE: {
        if(recv.type != VAL_CALENDAR) return bi_type_err("firstDate", recv);
        *out = lumyr_calendar_first_date(recv);
        return 1;
    }
    case BUILTIN_CALENDAR_LAST_DATE: {
        if(recv.type != VAL_CALENDAR) return bi_type_err("lastDate", recv);
        *out = lumyr_calendar_last_date(recv);
        return 1;
    }
    /* ===== file 文件对象 ===== */
    case BUILTIN_FILE_MAKE: {
        /* file(path [, mode])：磁盘文件；file(name, bytes(...))：内存文件 */
        if(argc < 1 || argv[0].type != VAL_STRING) {
            runtime_error("file() 至少需要 1 个路径字符串参数");
            return 0;
        }
        if(argc >= 2 && argv[1].type == VAL_BYTES) {
            *out = lumyr_file_from_bytes(lumyr_str_cstr(&argv[0]), argv[1]);
            return 1;
        }
        const char* mode = (argc >= 2 && argv[1].type == VAL_STRING) ? lumyr_str_cstr(&argv[1]) : "r";
        *out = lumyr_file_make(lumyr_str_cstr(&argv[0]), mode);
        return 1;
    }
    case BUILTIN_FILE_READ_ALL: {
        if(recv.type != VAL_FILE) return bi_type_err("readAll", recv);
        *out = lumyr_file_read_all(recv);
        return 1;
    }
    case BUILTIN_FILE_READ_LINES: {
        if(recv.type != VAL_FILE) return bi_type_err("readLines", recv);
        if(argc >= 2) {
            /* f.readLines(from, to)：行范围 */
            *out = lumyr_file_read_lines_range(recv, bi_num_i64(argv[1]), bi_num_i64(argv[2]));
        } else {
            /* f.readLines()：所有行 */
            *out = lumyr_file_read_lines(recv);
        }
        return 1;
    }
    case BUILTIN_FILE_READ_LINE: {
        if(recv.type != VAL_FILE) return bi_type_err("readLine", recv);
        if(argc < 1) { runtime_error("readLine(n) 需要 1 个行号参数"); return 0; }
        *out = lumyr_file_read_line(recv, bi_num_i64(argv[1]));
        return 1;
    }
    case BUILTIN_FILE_READ_LINES_RANGE: {
        /* 兼容：readLinesRange 显式调用（若名称映射存在时使用） */
        if(recv.type != VAL_FILE) return bi_type_err("readLinesRange", recv);
        if(argc < 2) { runtime_error("readLinesRange(from,to) 需要 2 个参数"); return 0; }
        *out = lumyr_file_read_lines_range(recv, bi_num_i64(argv[1]), bi_num_i64(argv[2]));
        return 1;
    }
    case BUILTIN_FILE_WRITE_ALL: {
        if(recv.type != VAL_FILE) return bi_type_err("writeAll", recv);
        if(argc < 1) { runtime_error("writeAll(s) 需要 1 个参数"); return 0; }
        char* s = value_to_str(argv[1]);
        *out = lumyr_file_write_all(recv, s);
        free(s);
        return 1;
    }
    case BUILTIN_FILE_WRITE_LINE: {
        if(recv.type != VAL_FILE) return bi_type_err("writeLine", recv);
        if(argc < 2) { runtime_error("writeLine(n,s) 需要 2 个参数"); return 0; }
        char* s = value_to_str(argv[2]);
        *out = lumyr_file_write_line(recv, bi_num_i64(argv[1]), s);
        free(s);
        return 1;
    }
    case BUILTIN_FILE_INSERT_LINE: {
        if(recv.type != VAL_FILE) return bi_type_err("insertLine", recv);
        if(argc < 2) { runtime_error("insertLine(n,s) 需要 2 个参数"); return 0; }
        char* s = value_to_str(argv[2]);
        *out = lumyr_file_insert_line(recv, bi_num_i64(argv[1]), s);
        free(s);
        return 1;
    }
    case BUILTIN_FILE_WRITE_LINES: {
        if(recv.type != VAL_FILE) return bi_type_err("writeLines", recv);
        if(argc < 1) { runtime_error("writeLines(arr) 需要 1 个参数"); return 0; }
        *out = lumyr_file_write_lines(recv, argv[1]);
        return 1;
    }
    case BUILTIN_FILE_APPEND: {
        if(recv.type == VAL_FORMDATA) {
            /* formdata.append(name, value)：追加（同名允许多次，多值/多文件语义），返回 self */
            if(!bi_need_args("append", argc, 2)) return 0;
            if(!lumyr_formdata_add(recv, argv[1], argv[2])) return 0;
            *out = recv;
            return 1;
        }
        if(recv.type != VAL_FILE) return bi_type_err("append", recv);
        if(argc < 1) { runtime_error("append(s) 需要 1 个参数"); return 0; }
        char* s = value_to_str(argv[1]);
        *out = lumyr_file_append(recv, s);
        free(s);
        return 1;
    }
    case BUILTIN_FILE_APPEND_LINE: {
        if(recv.type != VAL_FILE) return bi_type_err("appendLine", recv);
        if(argc < 1) { runtime_error("appendLine(s) 需要 1 个参数"); return 0; }
        char* s = value_to_str(argv[1]);
        *out = lumyr_file_append_line(recv, s);
        free(s);
        return 1;
    }
    case BUILTIN_FILE_FLUSH: {
        if(recv.type != VAL_FILE) return bi_type_err("flush", recv);
        *out = lumyr_file_flush(recv);
        return 1;
    }
    case BUILTIN_FILE_DELETE: {
        if(recv.type == VAL_FORMDATA) {
            /* fd.delete(name)：删除全部同名字段（原地），返回 self（与 set 链式语义一致） */
            if(!bi_need_args("delete", argc, 1)) return 0;
            lumyr_formdata_delete(recv, argv[1]);
            *out = recv;
            return 1;
        }
        if(recv.type != VAL_FILE) return bi_type_err("delete", recv);
        *out = lumyr_file_delete(recv);
        return 1;
    }
    case BUILTIN_FILE_READ_BYTES: {
        if(recv.type != VAL_FILE) return bi_type_err("readBytes", recv);
        *out = lumyr_file_read_bytes(recv);
        return 1;
    }
    case BUILTIN_FILE_WRITE_BYTES: {
        if(recv.type != VAL_FILE) return bi_type_err("writeBytes", recv);
        if(argc < 1) { runtime_error("writeBytes(b) 需要 1 个 bytes 参数"); return 0; }
        *out = lumyr_file_write_bytes(recv, argv[1]);
        return 1;
    }
    case BUILTIN_FILE_TRUNCATE: {
        if(recv.type != VAL_FILE) return bi_type_err("truncate", recv);
        if(argc < 1) { runtime_error("truncate(size) 需要 1 个参数"); return 0; }
        *out = lumyr_file_truncate(recv, bi_num_i64(argv[1]));
        return 1;
    }
    case BUILTIN_FILE_RENAME_TO: {
        if(recv.type == VAL_FILE) {
            if(argc < 1 || argv[1].type != VAL_STRING) { runtime_error("renameTo(newPath) 需要 1 个字符串参数"); return 0; }
            *out = lumyr_file_rename_to(recv, lumyr_str_cstr(&argv[1]));
            return 1;
        }
        if(recv.type == VAL_FOLDER) {
            if(argc < 1 || argv[1].type != VAL_STRING) { runtime_error("renameTo(newPath) 需要 1 个字符串参数"); return 0; }
            *out = lumyr_folder_rename_to(recv, lumyr_str_cstr(&argv[1]));
            return 1;
        }
        return bi_type_err("renameTo", recv);
    }
    /* ===== folder 目录对象 ===== */
    case BUILTIN_FOLDER_MAKE: {
        if(argc < 1 || argv[0].type != VAL_STRING) {
            runtime_error("folder() 至少需要 1 个路径字符串参数");
            return 0;
        }
        *out = lumyr_folder_make(lumyr_str_cstr(&argv[0]));
        return 1;
    }
    case BUILTIN_FOLDER_LIST: {
        if(recv.type != VAL_FOLDER) return bi_type_err("list", recv);
        *out = lumyr_folder_list(recv);
        return 1;
    }
    case BUILTIN_FOLDER_FILES: {
        if(recv.type != VAL_FOLDER) return bi_type_err("files", recv);
        *out = lumyr_folder_files(recv);
        return 1;
    }
    case BUILTIN_FOLDER_DIRS: {
        if(recv.type != VAL_FOLDER) return bi_type_err("dirs", recv);
        *out = lumyr_folder_dirs(recv);
        return 1;
    }
    case BUILTIN_FOLDER_CREATE: {
        if(recv.type != VAL_FOLDER) return bi_type_err("create", recv);
        *out = lumyr_folder_create(recv);
        return 1;
    }
    case BUILTIN_FOLDER_REMOVE: {
        if(recv.type != VAL_FOLDER) return bi_type_err("remove", recv);
        *out = lumyr_folder_remove(recv);
        return 1;
    }
    case BUILTIN_FOLDER_WALK: {
        if(recv.type != VAL_FOLDER) return bi_type_err("walk", recv);
        *out = lumyr_folder_walk(recv);
        return 1;
    }
    case BUILTIN_FOLDER_COPY_TO: {
        /* copyTo 同时支持 file 和 folder */
        if(recv.type == VAL_FILE) {
            if(argc < 1 || argv[1].type != VAL_STRING) { runtime_error("copyTo(dest) 需要 1 个字符串参数"); return 0; }
            *out = lumyr_file_copy_to(recv, lumyr_str_cstr(&argv[1]));
            return 1;
        }
        if(recv.type != VAL_FOLDER) return bi_type_err("copyTo", recv);
        if(argc < 1 || argv[1].type != VAL_STRING) { runtime_error("copyTo(dest) 需要 1 个字符串参数"); return 0; }
        *out = lumyr_folder_copy_to(recv, lumyr_str_cstr(&argv[1]));
        return 1;
    }
    case BUILTIN_FOLDER_MOVE_TO: {
        if(recv.type != VAL_FOLDER) return bi_type_err("moveTo", recv);
        if(argc < 1 || argv[1].type != VAL_STRING) { runtime_error("moveTo(dest) 需要 1 个字符串参数"); return 0; }
        *out = lumyr_folder_move_to(recv, lumyr_str_cstr(&argv[1]));
        return 1;
    }
    case BUILTIN_FOLDER_RENAME_TO: {
        /* 由 BUILTIN_FILE_RENAME_TO 统一处理 file+folder，此处不会到达 */
        if(recv.type == VAL_FOLDER) {
            if(argc < 1 || argv[1].type != VAL_STRING) { runtime_error("renameTo(newPath) 需要 1 个字符串参数"); return 0; }
            *out = lumyr_folder_rename_to(recv, lumyr_str_cstr(&argv[1]));
            return 1;
        }
        return bi_type_err("renameTo", recv);
    }
    case BUILTIN_FOLDER_GLOB: {
        if(recv.type != VAL_FOLDER) return bi_type_err("glob", recv);
        if(argc < 1 || argv[1].type != VAL_STRING) { runtime_error("glob(pattern) 需要 1 个字符串参数"); return 0; }
        *out = lumyr_folder_glob(recv, lumyr_str_cstr(&argv[1]));
        return 1;
    }

    /* ===== socket 网络套接字 ===== */
    /* 构造：无参→裸 socket；1 个 map 参→按 config 自动 connect/bind+listen */
    case BUILTIN_TCP_SOCKET: {
        int ci = is_method ? 1 : 0;
        if(argc > ci && argv[ci].type == VAL_MAP)
            *out = socket_ctor_from_config(SOCK_KIND_TCP, argv[ci]);
        else
            *out = lumyr_socket_make(SOCK_KIND_TCP);
        return 1;
    }
    case BUILTIN_UDP_SOCKET: {
        int ci = is_method ? 1 : 0;
        if(argc > ci && argv[ci].type == VAL_MAP)
            *out = socket_ctor_from_config(SOCK_KIND_UDP, argv[ci]);
        else
            *out = lumyr_socket_make(SOCK_KIND_UDP);
        return 1;
    }
    case BUILTIN_UNIX_SOCKET: {
        int ci = is_method ? 1 : 0;
        if(argc > ci && argv[ci].type == VAL_MAP)
            *out = socket_ctor_from_config(SOCK_KIND_UNIX_STREAM, argv[ci]);
        else
            *out = lumyr_socket_make(SOCK_KIND_UNIX_STREAM);
        return 1;
    }
    case BUILTIN_UNIX_DGRAM_SOCKET: {
        int ci = is_method ? 1 : 0;
        if(argc > ci && argv[ci].type == VAL_MAP)
            *out = socket_ctor_from_config(SOCK_KIND_UNIX_DGRAM, argv[ci]);
        else
            *out = lumyr_socket_make(SOCK_KIND_UNIX_DGRAM);
        return 1;
    }
    case BUILTIN_SOCKET_CONNECT: {
        /* s.connect(host, port) / s.connect(path)（Unix） */
        if(recv.type != VAL_SOCKET) return bi_type_err("connect", recv);
        SocketObj* o = (SocketObj*)recv.v.socket_obj;
        int nuser = is_method ? argc : argc - 1;
        if(!o) { runtime_error("connect() 套接字对象无效"); return 0; }
        if(o->kind == SOCK_KIND_UNIX_STREAM || o->kind == SOCK_KIND_UNIX_DGRAM) {
            /* Unix：1 个路径参数 */
            if(nuser < 1 || argv[1].type != VAL_STRING) { runtime_error("connect(path) 需要 1 个字符串参数"); return 0; }
            *out = lumyr_socket_connect(recv, lumyr_str_cstr(&argv[1]), 0);
        } else {
            /* TCP/UDP：host + port */
            if(nuser < 2 || argv[1].type != VAL_STRING) { runtime_error("connect(host, port) 需要 2 个参数"); return 0; }
            *out = lumyr_socket_connect(recv, lumyr_str_cstr(&argv[1]), (int)bi_num_i64(argv[2]));
        }
        return 1;
    }
    case BUILTIN_SOCKET_BIND: {
        if(recv.type != VAL_SOCKET) return bi_type_err("bind", recv);
        SocketObj* o = (SocketObj*)recv.v.socket_obj;
        int nuser = is_method ? argc : argc - 1;
        if(!o) { runtime_error("bind() 套接字对象无效"); return 0; }
        if(o->kind == SOCK_KIND_UNIX_STREAM || o->kind == SOCK_KIND_UNIX_DGRAM) {
            if(nuser < 1 || argv[1].type != VAL_STRING) { runtime_error("bind(path) 需要 1 个字符串参数"); return 0; }
            *out = lumyr_socket_bind(recv, lumyr_str_cstr(&argv[1]), 0);
        } else {
            if(nuser < 2 || argv[1].type != VAL_STRING) { runtime_error("bind(host, port) 需要 2 个参数"); return 0; }
            *out = lumyr_socket_bind(recv, lumyr_str_cstr(&argv[1]), (int)bi_num_i64(argv[2]));
        }
        return 1;
    }
    case BUILTIN_SOCKET_LISTEN: {
        if(recv.type != VAL_SOCKET) return bi_type_err("listen", recv);
        int nuser = is_method ? argc : argc - 1;
        int backlog = (nuser >= 1) ? (int)bi_num_i64(argv[1]) : 128;
        *out = lumyr_socket_listen(recv, backlog);
        return 1;
    }
    case BUILTIN_SOCKET_ACCEPT: {
        if(recv.type != VAL_SOCKET) return bi_type_err("accept", recv);
        *out = lumyr_socket_accept(recv);
        return 1;
    }
    case BUILTIN_SOCKET_RECV: {
        /* s.recv([len [, flags [, asBytes]]) → 字符串（默认）或 bytes（asBytes=true，二进制安全） */
        if(recv.type != VAL_SOCKET) return bi_type_err("recv", recv);
        int nuser = is_method ? argc : argc - 1;
        int maxLen = (nuser >= 1) ? (int)bi_num_i64(argv[1]) : 4096;
        int flags = (nuser >= 2) ? (int)bi_num_i64(argv[2]) : 0;
        int asBytes = (nuser >= 3) ? (lumyr_to_bool(argv[3]) ? 1 : 0) : 0;
        *out = lumyr_socket_recv(recv, maxLen, flags, asBytes);
        return 1;
    }
    case BUILTIN_SOCKET_SENDTO: {
        /* s.sendTo(data, host, port) / s.sendTo(data, path) → 字节数
         * data 为 bytes 时按原始字节发送（二进制安全） */
        if(recv.type != VAL_SOCKET) return bi_type_err("sendTo", recv);
        SocketObj* o = (SocketObj*)recv.v.socket_obj;
        int nuser = is_method ? argc : argc - 1;
        if(!o) { runtime_error("sendTo() 套接字对象无效"); return 0; }
        Value dataArg = argv[1];
        const char* d = NULL; int dlen = -1; char* dalloc = NULL;
        if(dataArg.type == VAL_BYTES) {
            BytesObj* b = (BytesObj*)dataArg.v.bytes_obj;
            d = b ? (const char*)b->data : "";
            dlen = b ? b->len : 0;
        } else {
            dalloc = value_to_str(dataArg);
            d = dalloc;
        }
        if(o->kind == SOCK_KIND_UNIX_STREAM || o->kind == SOCK_KIND_UNIX_DGRAM) {
            /* Unix 数据报：data + path */
            if(nuser < 2 || argv[2].type != VAL_STRING) { runtime_error("sendTo(data, path) 需要 2 个参数"); free(dalloc); return 0; }
            *out = lumyr_socket_sendto(recv, d, dlen, lumyr_str_cstr(&argv[2]), 0, 0);
        } else {
            /* UDP：data + host + port */
            if(nuser < 3 || argv[2].type != VAL_STRING) { runtime_error("sendTo(data, host, port) 需要 3 个参数"); free(dalloc); return 0; }
            *out = lumyr_socket_sendto(recv, d, dlen, lumyr_str_cstr(&argv[2]), (int)bi_num_i64(argv[3]), 0);
        }
        free(dalloc);
        return 1;
    }
    case BUILTIN_SOCKET_RECVFROM: {
        /* s.recvFrom([len]) → [data, addr] */
        if(recv.type != VAL_SOCKET) return bi_type_err("recvFrom", recv);
        int nuser = is_method ? argc : argc - 1;
        int maxLen = (nuser >= 1) ? (int)bi_num_i64(argv[1]) : 4096;
        *out = lumyr_socket_recvfrom(recv, maxLen, 0);
        return 1;
    }
    case BUILTIN_SOCKET_SETOPT: {
        if(recv.type != VAL_SOCKET) return bi_type_err("setOption", recv);
        int nuser = is_method ? argc : argc - 1;
        if(nuser < 2 || argv[1].type != VAL_STRING) { runtime_error("setOption(name, val) 需要 2 个参数"); return 0; }
        *out = lumyr_socket_set_option(recv, lumyr_str_cstr(&argv[1]), argv[2]);
        return 1;
    }
    case BUILTIN_SOCKET_GETOPT: {
        if(recv.type != VAL_SOCKET) return bi_type_err("getOption", recv);
        int nuser = is_method ? argc : argc - 1;
        if(nuser < 1 || argv[1].type != VAL_STRING) { runtime_error("getOption(name) 需要 1 个参数"); return 0; }
        *out = lumyr_socket_get_option(recv, lumyr_str_cstr(&argv[1]));
        return 1;
    }
    case BUILTIN_SOCKET_FILENO: {
        if(recv.type != VAL_SOCKET) return bi_type_err("fileno", recv);
        *out = lumyr_socket_fileno(recv);
        return 1;
    }

    /* ===== 线程 / 锁 / 条件变量 / 线程本地存储（kit/runtime 机制层直包装，仅函数形式） ===== */
    case BUILTIN_MUTEX:    *out = lumyr_make_int64(lumyr_mutex_create()); return 1;
    case BUILTIN_RMUTEX:   *out = lumyr_make_int64(lumyr_rmutex_create()); return 1;
    case BUILTIN_RWLOCK:   *out = lumyr_make_int64(lumyr_rwlock_create()); return 1;
    case BUILTIN_SPINLOCK: *out = lumyr_make_int64(lumyr_spinlock_create()); return 1;
    case BUILTIN_CONDVAR:  *out = lumyr_make_int64(lumyr_condvar_create()); return 1;
    case BUILTIN_LOCK:
        bi_need_args_mt("lock", argc, 1); bi_need_int_mt("lock", argv[0], 1);
        lumyr_lock((int)bi_num_i64(argv[0])); *out = val_none(); return 1;
    case BUILTIN_UNLOCK:
        bi_need_args_mt("unlock", argc, 1); bi_need_int_mt("unlock", argv[0], 1);
        lumyr_unlock((int)bi_num_i64(argv[0])); *out = val_none(); return 1;
    case BUILTIN_TRYLOCK:
        bi_need_args_mt("trylock", argc, 1); bi_need_int_mt("trylock", argv[0], 1);
        *out = lumyr_make_bool(lumyr_trylock((int)bi_num_i64(argv[0]))); return 1;
    case BUILTIN_RDLOCK:
        bi_need_args_mt("rdlock", argc, 1); bi_need_int_mt("rdlock", argv[0], 1);
        lumyr_rdlock((int)bi_num_i64(argv[0])); *out = val_none(); return 1;
    case BUILTIN_WRLOCK:
        bi_need_args_mt("wrlock", argc, 1); bi_need_int_mt("wrlock", argv[0], 1);
        lumyr_wrlock((int)bi_num_i64(argv[0])); *out = val_none(); return 1;
    case BUILTIN_TRYRDLOCK:
        bi_need_args_mt("tryrdlock", argc, 1); bi_need_int_mt("tryrdlock", argv[0], 1);
        *out = lumyr_make_bool(lumyr_tryrdlock((int)bi_num_i64(argv[0]))); return 1;
    case BUILTIN_TRYWRLOCK:
        bi_need_args_mt("trywrlock", argc, 1); bi_need_int_mt("trywrlock", argv[0], 1);
        *out = lumyr_make_bool(lumyr_trywrlock((int)bi_num_i64(argv[0]))); return 1;
    case BUILTIN_COND_WAIT:
        bi_need_args_mt("cond_wait", argc, 2);
        bi_need_int_mt("cond_wait", argv[0], 1); bi_need_int_mt("cond_wait", argv[1], 2);
        lumyr_cond_wait((int)bi_num_i64(argv[0]), (int)bi_num_i64(argv[1]));
        *out = val_none(); return 1;
    case BUILTIN_COND_TIMEDWAIT:
        bi_need_args_mt("cond_wait_timeout", argc, 3);
        bi_need_int_mt("cond_wait_timeout", argv[0], 1);
        bi_need_int_mt("cond_wait_timeout", argv[1], 2);
        bi_need_int_mt("cond_wait_timeout", argv[2], 3);
        *out = lumyr_make_bool(lumyr_cond_timedwait((int)bi_num_i64(argv[0]),
                                                    (int)bi_num_i64(argv[1]),
                                                    (long long)bi_num_i64(argv[2])));
        return 1;
    case BUILTIN_COND_SIGNAL:
        bi_need_args_mt("cond_signal", argc, 1); bi_need_int_mt("cond_signal", argv[0], 1);
        lumyr_cond_signal((int)bi_num_i64(argv[0])); *out = val_none(); return 1;
    case BUILTIN_COND_BROADCAST:
        bi_need_args_mt("cond_broadcast", argc, 1); bi_need_int_mt("cond_broadcast", argv[0], 1);
        lumyr_cond_broadcast((int)bi_num_i64(argv[0])); *out = val_none(); return 1;
    case BUILTIN_THREADLOCAL_GET:
        bi_need_args_mt("threadlocal_get", argc, 1);
        *out = lumyr_tls_get(bi_need_str_mt("threadlocal_get", &argv[0], 1));
        return 1;
    case BUILTIN_THREADLOCAL_SET:
        bi_need_args_mt("threadlocal_set", argc, 2);
        lumyr_tls_set(bi_need_str_mt("threadlocal_set", &argv[0], 1), argv[1]);
        *out = argv[1]; return 1;   /* 返回 value（表达式值） */
    case BUILTIN_THREAD: {
        bi_need_args_mt("thread", argc, 1);
        if(argv[0].type != VAL_FUNC || !argv[0].v.func.func_obj) {
            fprintf(stderr, "运行时错误: thread(f, args...) 首参须为函数 / thread: first argument must be a function\n");
            exit(1);
        }
        /* 函数值堆交接给线程体（函数为引用语义：浅拷贝共享 RuntimeFunc，线程体 free 容器） */
        Value* fvp = (Value*)malloc(sizeof(Value));
        if(!fvp) { perror("thread"); exit(EXIT_FAILURE); }
        *fvp = argv[0];
        *out = lumyr_make_int64(lumyr_thread_start(vm_thread_body, fvp,
                                                   argc > 1 ? &argv[1] : NULL, argc - 1));
        return 1;
    }
    case BUILTIN_THREAD_JOIN:
        bi_need_args_mt("thread_join", argc, 1); bi_need_int_mt("thread_join", argv[0], 1);
        *out = lumyr_thread_join((int)bi_num_i64(argv[0]));
        return 1;
    case BUILTIN_SLEEP:
        /* sleep(ms)：休眠毫秒；lm_time 内部按 GC 安全点处理阻塞 */
        bi_need_args_mt("sleep", argc, 1); bi_need_int_mt("sleep", argv[0], 1);
        lumyr_sleep_ms((long long)bi_num_i64(argv[0]));
        *out = val_none(); return 1;
    case BUILTIN_TIMESTAMP:
        if(argc < 1) { /* 支持无参形式 timestamp() */ }
        *out = lumyr_make_double(lumyr_timestamp()); return 1;
    case BUILTIN_TIMESTAMP_MS:
        *out = lumyr_make_int64(lumyr_timestamp_ms()); return 1;

    default:
        fprintf(stderr, "VM: 未实现的内置函数 id=%d\n", id);
        return 0;
    }
}

/* ===== 指令入口 ===== */

/* 全局形式 m(x, ...)：VALUE 栈弹 b 个实参（argv[0] 即 receiver） */
int vm_exec_builtin(VMExecCtx* ctx, const Instruction* in) {
    (void)ctx;
    int argc = in->b;
    Value* argv = (Value*)malloc(sizeof(Value) * (size_t)(argc > 0 ? argc : 1));
    if(!argv) { perror("vm_exec_builtin"); return 0; }
    for(int i = argc - 1; i >= 0; i--) stack_vm_pop(g_stack_mgr, STACK_VALUE, &argv[i]);
    Value rv;
    int rc = builtin_dispatch(ctx, in->a, argv, argc, &rv, 0);
    free(argv);
    if(rc == 1) stack_vm_push(g_stack_mgr, STACK_VALUE, &rv);
    return rc;
}

/* 方法/属性形式 x.m(...)：VALUE 栈顶 b 个实参 + 其下 receiver */
int vm_exec_builtin_method(VMExecCtx* ctx, const Instruction* in) {
    (void)ctx;
    int argc = in->b;
    Value* argv = (Value*)malloc(sizeof(Value) * (size_t)(argc + 1));
    if(!argv) { perror("vm_exec_builtin_method"); return 0; }
    for(int i = argc - 1; i >= 0; i--) stack_vm_pop(g_stack_mgr, STACK_VALUE, &argv[i + 1]);
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &argv[0]);
    Value rv;
    int rc = builtin_dispatch(ctx, in->a, argv, argc, &rv, 1);
    free(argv);
    if(rc == 1) stack_vm_push(g_stack_mgr, STACK_VALUE, &rv);
    return rc;
}

/* ===== 反汇编名表（bytecode.c 用） ===== */

const char* builtin_id_name(int id) {
    switch(id) {
    case BUILTIN_LEN: return "len";
    case BUILTIN_TYPE: return "type";
    case BUILTIN_RANGE: return "range";
    case BUILTIN_SUBSTR: return "substr";
    case BUILTIN_TOUPPER: return "toupper";
    case BUILTIN_TOLOWER: return "tolower";
    case BUILTIN_STRIP: return "strip";
    case BUILTIN_REPEAT: return "repeat";
    case BUILTIN_SPLIT: return "split";
    case BUILTIN_REPLACE: return "replace";
    case BUILTIN_STARTSWITH: return "startswith";
    case BUILTIN_ENDSWITH: return "endswith";
    case BUILTIN_READ_FILE: return "readFile";
    case BUILTIN_WRITE_FILE: return "writeFile";
    case BUILTIN_FILE_EXISTS: return "fileExists";
    case BUILTIN_CONTAINS: return "contains";
    case BUILTIN_ARRAY_INDEXOF: return "indexOf";
    case BUILTIN_JOIN: return "join";
    case BUILTIN_FORMAT: return "format";
    case BUILTIN_ARRAY_ADD: return "add";
    case BUILTIN_DEL: return "del";
    case BUILTIN_INSERT: return "insert";
    case BUILTIN_MAP_HAS: return "has";
    case BUILTIN_KEYS: return "keys";
    case BUILTIN_VALUES: return "values";
    case BUILTIN_SUM: return "sum";
    case BUILTIN_AVG: return "avg";
    case BUILTIN_SORT: return "sort";
    case BUILTIN_REVERSE: return "reverse";
    case BUILTIN_MIN: return "min";
    case BUILTIN_MAX: return "max";
    case BUILTIN_ABS: return "abs";
    case BUILTIN_FLOOR: return "floor";
    case BUILTIN_CEIL: return "ceil";
    case BUILTIN_SQRT: return "sqrt";
    case BUILTIN_SIN: return "sin";
    case BUILTIN_COS: return "cos";
    case BUILTIN_TAN: return "tan";
    case BUILTIN_ASIN: return "asin";
    case BUILTIN_ACOS: return "acos";
    case BUILTIN_ATAN: return "atan";
    case BUILTIN_ATAN2: return "atan2";
    case BUILTIN_LOG: return "log";
    case BUILTIN_LOG10: return "log10";
    case BUILTIN_LOG2: return "log2";
    case BUILTIN_EXP: return "exp";
    case BUILTIN_POW: return "pow";
    case BUILTIN_ROUND: return "round";
    case BUILTIN_CBRT: return "cbrt";
    case BUILTIN_HYPOT: return "hypot";
    case BUILTIN_SIGN: return "sign";
    case BUILTIN_DEGREES: return "degrees";
    case BUILTIN_RADIANS: return "radians";
    case BUILTIN_TRUNC: return "trunc";
    case BUILTIN_RANDOM: return "random";
    case BUILTIN_ARRAY_FIRST: return "first";
    case BUILTIN_ARRAY_LAST: return "last";
    case BUILTIN_ARRAY_FLAT: return "flat";
    case BUILTIN_FILTER: return "filter";
    case BUILTIN_REDUCE: return "reduce";
    case BUILTIN_MAP: return "map";
    case BUILTIN_ARRAY_REMOVE: return "remove";
    case BUILTIN_ARRAY_CLEAR: return "clear";
    case BUILTIN_GET: return "get";
    case BUILTIN_SET: return "set";
    case BUILTIN_ARRAY_ADDALL: return "addAll";
    case BUILTIN_SKIP: return "skip";
    case BUILTIN_TAKE: return "take";
    case BUILTIN_ENUMERATE: return "enumerate";
    case BUILTIN_OBJECT_INDEX: return "objectIndex";
    case BUILTIN_CHAR_AT: return "charAt";
    case BUILTIN_SHAPE: return "shape";
    case BUILTIN_RESHAPE: return "reshape";
    case BUILTIN_SLICE: return "slice";
    case BUILTIN_CONCAT: return "concat";
    case BUILTIN_DOT: return "dot";
    case BUILTIN_MATMUL: return "matmul";
    case BUILTIN_TRANSPOSE: return "transpose";
    case BUILTIN_NORM: return "norm";
    case BUILTIN_DET: return "det";
    case BUILTIN_INV: return "inv";
    case BUILTIN_MEAN: return "mean";
    case BUILTIN_STD: return "std";
    case BUILTIN_VARIANCE: return "var";
    case BUILTIN_ARGMAX: return "argmax";
    case BUILTIN_ARGMIN: return "argmin";
    case BUILTIN_NORMALIZE: return "normalize";
    case BUILTIN_SOFTMAX: return "softmax";
    case BUILTIN_FROM_VALUE: return "fromValue";
    case BUILTIN_ASSERT: return "__assert";
    case BUILTIN_LM_SERIALIZE: return "__lmSerialize";
    case BUILTIN_LM_DESERIALIZE: return "__lmDeserialize";
    case BUILTIN_LM_BUILD_STREAM: return "__lmBuildStream";
    case BUILTIN_LM_CHECK_HEADER: return "__lmCheckHeader";
    case BUILTIN_DATE_MAKE: return "date";
    case BUILTIN_DATETIME_MAKE: return "datetime";
    case BUILTIN_TIME_MAKE: return "time";
    case BUILTIN_TIMEDELTA_MAKE: return "timedelta";
    case BUILTIN_TODAY: return "today";
    case BUILTIN_YEAR: return "year";
    case BUILTIN_MONTH: return "month";
    case BUILTIN_DAY: return "day";
    case BUILTIN_HOUR: return "hour";
    case BUILTIN_MINUTE: return "minute";
    case BUILTIN_SECOND: return "second";
    case BUILTIN_WEEKDAY: return "weekday";
    case BUILTIN_YEARDAY: return "yearday";
    case BUILTIN_DAYS: return "days";
    case BUILTIN_SECONDS: return "seconds";
    case BUILTIN_TOTAL_SECONDS: return "totalSeconds";
    case BUILTIN_FORMAT_DATE: return "formatDate";
    case BUILTIN_DATE_DIFF: return "diff";
    case BUILTIN_DATE_ADD: return "add";
    case BUILTIN_TUPLE_MAKE: return "tuple";
    case BUILTIN_SET_UNION: return "union";
    case BUILTIN_SET_INTERSECT: return "intersect";
    case BUILTIN_BYTES_MAKE: return "bytes";
    case BUILTIN_BYTES_HEX: return "hex";
    case BUILTIN_BYTES_TO_STR: return "toStr";
    case BUILTIN_BYTES_FROM_HEX: return "fromHex";
    case BUILTIN_COMPLEX_MAKE: return "complex";
    case BUILTIN_COMPLEX_CONJUGATE: return "conjugate";
    case BUILTIN_CALENDAR_MAKE: return "calendar";
    case BUILTIN_CALENDAR_FIRST_DATE: return "firstDate";
    case BUILTIN_CALENDAR_LAST_DATE: return "lastDate";
    case BUILTIN_FILE_MAKE: return "file";
    case BUILTIN_FILE_READ_ALL: return "readAll";
    case BUILTIN_FILE_READ_LINES: return "readLines";
    case BUILTIN_FILE_READ_LINE: return "readLine";
    case BUILTIN_FILE_READ_LINES_RANGE: return "readLinesRange";
    case BUILTIN_FILE_WRITE_ALL: return "writeAll";
    case BUILTIN_FILE_WRITE_LINE: return "writeLine";
    case BUILTIN_FILE_INSERT_LINE: return "insertLine";
    case BUILTIN_FILE_WRITE_LINES: return "writeLines";
    case BUILTIN_FILE_APPEND: return "append";
    case BUILTIN_FILE_APPEND_LINE: return "appendLine";
    case BUILTIN_FILE_FLUSH: return "flush";
    case BUILTIN_FILE_DELETE: return "delete";
    case BUILTIN_FILE_READ_BYTES: return "readBytes";
    case BUILTIN_FILE_WRITE_BYTES: return "writeBytes";
    case BUILTIN_FILE_TRUNCATE: return "truncate";
    case BUILTIN_FILE_RENAME_TO: return "renameTo";
    case BUILTIN_FOLDER_MAKE: return "folder";
    case BUILTIN_FOLDER_LIST: return "list";
    case BUILTIN_FOLDER_FILES: return "files";
    case BUILTIN_FOLDER_DIRS: return "dirs";
    case BUILTIN_FOLDER_CREATE: return "create";
    case BUILTIN_FOLDER_WALK: return "walk";
    case BUILTIN_FOLDER_COPY_TO: return "copyTo";
    case BUILTIN_FOLDER_MOVE_TO: return "moveTo";
    case BUILTIN_FOLDER_RENAME_TO: return "renameTo";
    case BUILTIN_FOLDER_GLOB: return "glob";
    case BUILTIN_HTTP_GET: return "http_get";
    case BUILTIN_HTTP_POST: return "http_post";
    case BUILTIN_HTTP_PUT: return "http_put";
    case BUILTIN_HTTP_DELETE: return "http_delete";
    case BUILTIN_HTTP_HEAD: return "http_head";
    case BUILTIN_HTTP_PATCH: return "http_patch";
    case BUILTIN_THREAD: return "thread";
    case BUILTIN_THREAD_JOIN: return "thread_join";
    case BUILTIN_SLEEP: return "sleep";
    case BUILTIN_TIMESTAMP: return "timestamp";
    case BUILTIN_TIMESTAMP_MS: return "timestamp_ms";
    case BUILTIN_MUTEX: return "mutex";
    case BUILTIN_RMUTEX: return "rmutex";
    case BUILTIN_RWLOCK: return "rwlock";
    case BUILTIN_SPINLOCK: return "spinlock";
    case BUILTIN_LOCK: return "lock";
    case BUILTIN_UNLOCK: return "unlock";
    case BUILTIN_TRYLOCK: return "trylock";
    case BUILTIN_RDLOCK: return "rdlock";
    case BUILTIN_WRLOCK: return "wrlock";
    case BUILTIN_TRYRDLOCK: return "tryrdlock";
    case BUILTIN_TRYWRLOCK: return "trywrlock";
    case BUILTIN_CONDVAR: return "condvar";
    case BUILTIN_COND_WAIT: return "cond_wait";
    case BUILTIN_COND_TIMEDWAIT: return "cond_wait_timeout";
    case BUILTIN_COND_SIGNAL: return "cond_signal";
    case BUILTIN_COND_BROADCAST: return "cond_broadcast";
    case BUILTIN_THREADLOCAL_GET: return "threadlocal_get";
    case BUILTIN_THREADLOCAL_SET: return "threadlocal_set";
    case BUILTIN_FORMDATA_NEW: return "__formdata_new";
    case BUILTIN_FORMDATA_APPEND: return "__formdata_append";
    case BUILTIN_FOREACH: return "forEach";
    case BUILTIN_GETALL: return "getAll";
    case BUILTIN_TOMAP: return "toMap";
    case BUILTIN_TOARRAY: return "toArray";
    case BUILTIN_TOJSON: return "toJSONString";
    case BUILTIN_COPY: return "copy";
    case BUILTIN_TCP_SOCKET: return "tcpSocket";
    case BUILTIN_UDP_SOCKET: return "udpSocket";
    case BUILTIN_UNIX_SOCKET: return "unixSocket";
    case BUILTIN_UNIX_DGRAM_SOCKET: return "unixDgramSocket";
    case BUILTIN_SOCKET_CONNECT: return "connect";
    case BUILTIN_SOCKET_BIND: return "bind";
    case BUILTIN_SOCKET_LISTEN: return "listen";
    case BUILTIN_SOCKET_ACCEPT: return "accept";
    case BUILTIN_SOCKET_RECV: return "recv";
    case BUILTIN_SOCKET_SENDTO: return "sendTo";
    case BUILTIN_SOCKET_RECVFROM: return "recvFrom";
    case BUILTIN_SOCKET_SETOPT: return "setOption";
    case BUILTIN_SOCKET_GETOPT: return "getOption";
    case BUILTIN_SOCKET_FILENO: return "fileno";
    default: return "?";
    }
}
