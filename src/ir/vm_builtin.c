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
#include "stack_manager.h"
#include "lumyr_value.h"
#include "gc_runtime.h"
#include "lm_value.h"
#include "lm_string.h"
#include "lm_array.h"
#include "lm_map.h"
#include "lm_math.h"
#include "lm_crypto.h"
#include "lm_regex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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

/* ===== 向量/矩阵元素读取抽象（VAL_ARRAY 直读 union / TypedArray 裸读） ===== */

static int bi_len_of(Value v) {
    switch(v.type) {
    case VAL_STRING:      return lumyr_str_len(&v);
    case VAL_ARRAY:       return v.v.array ? v.v.array->len : 0;
    case VAL_TYPED_ARRAY: return v.v.typed_array ? v.v.typed_array->len : 0;
    case VAL_MAP:         return v.v.map ? v.v.map->len : 0;
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

/* 按输入种类构造同构数值数组输出：typed → 同 et 新 TypedArray；array → val_array */
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
        if(n > 0) {
            size_t isz = lumyr_etype_itemsz(et);
            ta->items = gc_alloc_old(isz * (size_t)ta->cap, VAL_TYPED_ARRAY);
            gc_mark_internal_buf(ta->items);
            for(int i = 0; i < n; i++) {
                if(is_double) bi_write_et(et, ta->items, i, (int64_t)((const double*)vals)[i],
                                          ((const double*)vals)[i]);
                else bi_write_et(et, ta->items, i, ((const int64_t*)vals)[i],
                                 (double)((const int64_t*)vals)[i]);
            }
        } else {
            ta->items = NULL;
        }
        r.v.typed_array = ta;
        gc_enable();
        return r;
    }
    /* 未声明数组输出：每元素一次装箱（Value 表示所限） */
    Value r = val_array(n);
    for(int i = 0; i < n; i++) {
        if(is_double) r.v.array->items[i] = lumyr_make_double(((const double*)vals)[i]);
        else          r.v.array->items[i] = lumyr_make_int64(((const int64_t*)vals)[i]);
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

/* 类型错误提示 */
static int bi_type_err(const char* name, Value recv) {
    Value tn = lumyr_type(recv);
    fprintf(stderr, "运行时错误: 类型 %s 不支持方法 .%s\n",
            lumyr_str_cstr(&tn), name);
    return 0;
}

static int bi_need_args(const char* name, int argc, int need) {
    if(argc < need) {
        fprintf(stderr, "运行时错误: .%s 需要 %d 个参数，实际 %d 个\n", name, need, argc);
        return 0;
    }
    return 1;
}

/* ===== 分发核心 ===== */

/* builtin_dispatch：id=BuiltinId；argv[0]=receiver；
 * is_method=1（OPC_CALL_BUILTIN_METHOD）argc 不含 receiver；
 * is_method=0（OPC_BUILTIN 全局形式）argv[0]=首参即 receiver，argc 含它 */
int builtin_dispatch(VMExecCtx* ctx, int id, Value* argv, int argc, Value* out, int is_method) {
    *out = val_none();
    Value recv = argv[0];

    switch(id) {
    /* ===== 通用 ===== */
    case BUILTIN_LEN: {
        if(recv.type != VAL_STRING && recv.type != VAL_ARRAY &&
           recv.type != VAL_TYPED_ARRAY && recv.type != VAL_MAP)
            return bi_type_err("len", recv);
        *out = lumyr_make_int64(bi_len_of(recv));
        return 1;
    }
    case BUILTIN_TYPE:
        *out = lumyr_type(recv);
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
        if(recv.type != VAL_STRING) return bi_type_err("char_at", recv);
        if(!bi_need_args("char_at", argc, 1)) return 0;
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
        if(recv.type != VAL_STRING) return bi_type_err("regex_match", recv);
        if(!bi_need_args("regex_match", argc, 1)) return 0;
        *out = lumyr_make_bool(lumyr_regex_match(lumyr_str_cstr(&recv),
                                                 lumyr_str_cstr(&argv[1])));
        return 1;
    }
    case BUILTIN_REGEX_SEARCH: {
        if(recv.type != VAL_STRING) return bi_type_err("regex_search", recv);
        if(!bi_need_args("regex_search", argc, 1)) return 0;
        *out = lumyr_regex_search(lumyr_str_cstr(&recv), lumyr_str_cstr(&argv[1]));
        return 1;
    }
    case BUILTIN_REGEX_REPLACE: {
        if(recv.type != VAL_STRING) return bi_type_err("regex_replace", recv);
        if(!bi_need_args("regex_replace", argc, 2)) return 0;
        char* h = lumyr_regex_replace(lumyr_str_cstr(&recv), lumyr_str_cstr(&argv[1]),
                                      lumyr_str_cstr(&argv[2]));
        *out = lumyr_make_string(h ? h : "");
        free(h);
        return 1;
    }

    /* ===== 格式化（模板字符串 f"{x}" 编译目标） ===== */
    case BUILTIN_FORMAT: {
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
        /* receiver 分派：数组追加 / 字典设键值（均原地修改并返回 self） */
        if(recv.type == VAL_MAP) {
            if(!bi_need_args("add", argc, 2)) return 0;
            *out = lumyr_map_add(recv, argv[1], argv[2]);
            return 1;
        }
        if(recv.type != VAL_ARRAY) return bi_type_err("add", recv);
        if(!bi_need_args("add", argc, 1)) return 0;
        *out = lumyr_array_add(&recv, argv[1]); /* 原地追加，返回 self */
        return 1;
    }
    case BUILTIN_INSERT: {
        if(recv.type != VAL_ARRAY) return bi_type_err("insert", recv);
        if(!bi_need_args("insert", argc, 2)) return 0;
        *out = lumyr_insert(&recv, argv[1], argv[2]); /* 原地插入，返回 self */
        return 1;
    }
    case BUILTIN_ARRAY_REMOVE: {
        if(recv.type != VAL_ARRAY && recv.type != VAL_MAP)
            return bi_type_err("remove", recv);
        if(!bi_need_args("remove", argc, 1)) return 0;
        if(recv.type == VAL_MAP) *out = lumyr_map_del(&recv, argv[1]);
        else                     *out = lumyr_del(&recv, argv[1]);
        return 1;
    }
    case BUILTIN_ARRAY_CLEAR: {
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
        if(recv.type == VAL_MAP) {
            if(!bi_need_args("get", argc, 1)) return 0;
            *out = lumyr_map_get(recv, argv[1]);
            return 1;
        }
        return bi_type_err("get", recv);
    }
    case BUILTIN_SET: {
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
        return bi_type_err("set", recv);
    }
    case BUILTIN_ARRAY_INDEXOF: {
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
        if(recv.type != VAL_ARRAY) return bi_type_err("first", recv);
        *out = lumyr_array_first(recv);
        return 1;
    case BUILTIN_ARRAY_LAST:
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
        if(recv.type != VAL_ARRAY) return bi_type_err("join", recv);
        if(!bi_need_args("join", argc, 1)) return 0;
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
            for(int i = 0; i < n; i++) s += (int64_t)bi_vec_dbl(recv, i);
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
        if(recv.type != VAL_MAP) return bi_type_err("keys", recv);
        *out = lumyr_map_keys(recv);
        return 1;
    case BUILTIN_VALUES:
        if(recv.type != VAL_MAP) return bi_type_err("values", recv);
        *out = lumyr_map_values(recv);
        return 1;
    case BUILTIN_MAP_HAS:
        if(recv.type != VAL_MAP) return bi_type_err("has", recv);
        if(!bi_need_args("has", argc, 1)) return 0;
        *out = lumyr_make_bool(lumyr_map_has(recv, argv[1]));
        return 1;

    /* ===== contains/indexOf 通用（string/array/map） ===== */
    case BUILTIN_CONTAINS:
        if(!bi_need_args("contains", argc, 1)) return 0;
        *out = lumyr_contains(recv, argv[1]);
        return 1;

    /* ===== 高阶函数（仅 VAL_ARRAY；回调跨边界装箱一次） ===== */
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
        if(recv.type != VAL_ARRAY && recv.type != VAL_TYPED_ARRAY)
            return bi_type_err("slice", recv);
        if(!bi_need_args("slice", argc, 1)) return 0;
        int n = bi_len_of(recv);
        int64_t start = bi_num_i64(argv[1]);
        int64_t end = (argc >= 2) ? bi_num_i64(argv[2]) : n;
        if(start < 0) start = 0;
        if(end > n) end = n;
        if(start > end) start = end;
        int cnt = (int)(end - start);
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
                s += (int64_t)bi_vec_dbl(recv, i) * (int64_t)bi_vec_dbl(other, i);
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
                    double x = bi_mat_read(recv, i, t) * bi_mat_read(B, t, j);
                    acc += x;
                    iacc += (int64_t)x;
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
                double x = bi_mat_read(recv, j, i);
                row.v.array->items[j] = int_only ? lumyr_make_int64((int64_t)x)
                                                 : lumyr_make_double(x);
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
        *out = lumyr_abs(recv);
        return 1;
    case BUILTIN_SQRT:
        *out = lumyr_sqrt(recv);
        return 1;
    case BUILTIN_DEL:
        if(!bi_need_args("del", argc, 1)) return 0;
        *out = lumyr_del(&recv, argv[1]);
        return 1;

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
    case BUILTIN_CHAR_AT: return "char_at";
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
    default: return "?";
    }
}
