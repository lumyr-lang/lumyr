/*
 * vm_exec_type.c - VM 类型转换指令
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "ir_types.h"
#include "lm_value.h"
#include "lm_bigint.h"
#include "lm_decimal.h"
#include "lm_bitdecimal.h"
#include "gc_runtime.h"
#include <string.h>

/* ===== 类型转换 ===== */

/* INT64_TO_DOUBLE：INT64 栈 → DOUBLE 栈 */
int vm_exec_type_int64_to_double(VMExecCtx* ctx, Instruction* in) {
    int64_t val;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &val);
    double dval = (double)val;
    stack_vm_push(g_stack_mgr, STACK_DOUBLE, &dval);
    return 1;
}

/* DOUBLE_TO_INT64：DOUBLE 栈 → INT64 栈（截断） */
int vm_exec_type_double_to_int64(VMExecCtx* ctx, Instruction* in) {
    double val;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &val);
    int64_t ival = (int64_t)val;
    stack_vm_push(g_stack_mgr, STACK_INT64, &ival);
    return 1;
}

/* NEG：负号，a 字段存表达式类型（INT / DOUBLE / PTR）。
 * PTR 时 b 字段存精确 CastKind（bigint/decimal/bitdecimal），
 * 值在 PTR 栈：此前误从 INT64 栈弹，高精度取负静默失效。 */
int vm_exec_type_neg(VMExecCtx* ctx, Instruction* in) {
    if(in->a == (int)EXPR_TYPE_DOUBLE) {
        double val;
        stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &val);
        val = -val;
        stack_vm_push(g_stack_mgr, STACK_DOUBLE, &val);
    } else if(in->a == (int)EXPR_TYPE_PTR) {
        void* p;
        stack_vm_pop(g_stack_mgr, STACK_PTR, &p);
        void* neg = NULL;
        switch((CastKind)in->b) {
        case CAST_BIGINT: {
            BigInt* zero = lumyr_bigint_from_int64(0);
            neg = lumyr_bigint_sub(zero, (BigInt*)p);
            lumyr_bigint_free(zero);
            break;
        }
        case CAST_DECIMAL: {
            Decimal* zero = lumyr_decimal_from_int64(0);
            neg = lumyr_decimal_sub(zero, (Decimal*)p);
            lumyr_decimal_free(zero);
            break;
        }
        case CAST_BITDECIMAL: {
            BitDecimal* zero = lumyr_bitdecimal_from_int64(0);
            neg = lumyr_bitdecimal_sub(zero, (BitDecimal*)p);
            lumyr_bitdecimal_free(zero);
            break;
        }
        default:
            break;
        }
        stack_vm_push(g_stack_mgr, STACK_PTR, &neg);
    } else {
        int64_t val;
        stack_vm_pop(g_stack_mgr, STACK_INT64, &val);
        val = -val;
        stack_vm_push(g_stack_mgr, STACK_INT64, &val);
    }
    return 1;
}

/* 一元负号可处理的类型白名单：全部数值族 + 三种高精度类型。
 * VAL_INT8(101)..VAL_SSIZE_T(118) 连续（VAL_VOID=100 不在范围）。
 * 其余类型（string/array/map/set/tuple/func/generator/bytes/对象等）取负非法。 */
static int vneg_is_numeric(ValueType t) {
    if(t == VAL_INT || t == VAL_DOUBLE || t == VAL_BOOL ||
       t == VAL_CHAR || t == VAL_BYTE ||
       t == VAL_FLOAT || t == VAL_LONG_DOUBLE ||
       t == VAL_BIGINT || t == VAL_DECIMAL || t == VAL_BITDECIMAL) return 1;
    if(t >= VAL_INT8 && t <= VAL_SSIZE_T) return 1;
    return 0;
}

/* VNEG：通用 Value 一元负（动态兜底：VALUE 栈） */
int vm_exec_vneg(VMExecCtx* ctx, Instruction* in) {
    Value val;
    (void)in;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &val);
    if(val.type == VAL_NONE) {
        /* 无兜底：-null 抛 TypeError，不再静默得 -0.0 */
        vm_except_raise_str(ctx, "TypeError",
            "null 不能参与算术运算 / null cannot participate in arithmetic");
        return 1;
    }
    if(!vneg_is_numeric(val.type)) {
        /* 无兜底：字符串等非数值取负抛 TypeError（可被 try/catch 捕获），
         * 与 lumyr_unary_minus 库层 runtime_error 防线消息一致 */
        vm_except_raise_str(ctx, "TypeError",
            "一元负号要求数值操作数，不能用于字符串等非数值类型 / unary minus requires a numeric operand, cannot apply to non-numeric types such as string");
        return 1;
    }
    Value r = lumyr_unary_minus(val);
    stack_vm_push(g_stack_mgr, STACK_VALUE, &r);
    return 1;
}

/* ASSERT_NONNULL：VALUE 栈弹 1，为 null(VAL_NONE) 则抛 NullError，否则原样压回。
 * 用于非空类型 T 的写入/实参绑定校验；可被 try/catch 捕获。净栈变化 0。 */
int vm_exec_assert_nonnull(VMExecCtx* ctx, Instruction* in) {
    (void)in;
    Value v;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &v);
    if(v.type == VAL_NONE) {
        vm_except_raise_str(ctx, "NullError", "非空类型不允许 null 值");
        return 1;   /* 抛出后不再压回 */
    }
    stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
    return 1;
}

/* ===== typed 栈 -> VALUE 栈装箱（实参 typed、形参动态 NONE 时绑定用） ===== */

/* ===== Value 通用解箱助手（动态调用实参绑定与 UNBOX 指令共用） ===== */

/* Value → int64：整型族/bool/char/byte 直接取，string 按数值解析，
 * 其余经 lumyr_cast_int64 兜底（long/long long/short/int8/int16/int32/
 * uint 族/size_t 等整型宽类型全覆盖）。
 * 此前 vm_call_func_value 实参绑定内联分支漏掉整型宽类型，
 * 动态调用传 long 值给带类型形参被静默绑 0。 */
int64_t vm_value_to_i64(Value v) {
    /* 全部整型族按 union 成员直接取值：禁止经 lumyr_cast_int64 兜底——
     * 其对 VAL_LONG 等宽整型返回 VAL_INT（32 位截断），9007199254740993 → 1 */
    if (v.type == VAL_INT64) return v.v.i64;
    if (v.type == VAL_LONG) return (int64_t)v.v.l;
    if (v.type == VAL_LONG_LONG) return v.v.ll;
    if (v.type == VAL_INT) return (int64_t)v.v.i;
    if (v.type == VAL_INT8) return (int64_t)v.v.i8;
    if (v.type == VAL_INT16) return (int64_t)v.v.i16;
    if (v.type == VAL_SHORT) return (int64_t)v.v.sh;
    if (v.type == VAL_INT32) return (int64_t)v.v.i32;
    if (v.type == VAL_UINT8) return (int64_t)v.v.u8;
    if (v.type == VAL_UCHAR) return (int64_t)v.v.uc;
    if (v.type == VAL_BYTE) return (int64_t)v.v.by;
    if (v.type == VAL_UINT16) return (int64_t)v.v.u16;
    if (v.type == VAL_USHORT) return (int64_t)v.v.us;
    if (v.type == VAL_UINT32) return (int64_t)v.v.u32;
    if (v.type == VAL_UINT) return (int64_t)v.v.ui;
    if (v.type == VAL_UINT64) return (int64_t)v.v.u64;
    if (v.type == VAL_ULONG) return (int64_t)v.v.ul;
    if (v.type == VAL_SIZE_T) return (int64_t)v.v.st;
    if (v.type == VAL_SSIZE_T) return (int64_t)v.v.sst;
    if (v.type == VAL_BOOL) return (int64_t)(v.v.b ? 1 : 0);
    if (v.type == VAL_CHAR) return (int64_t)(unsigned char)v.v.c;
    if (v.type == VAL_DOUBLE) return (int64_t)v.v.d;
    if (v.type == VAL_FLOAT) return (int64_t)v.v.f;
    if (v.type == VAL_LONG_DOUBLE) return (int64_t)v.v.ld;
    if (v.type == VAL_NONE) return 0;
    if (v.type == VAL_STRING) {
        const char* s = v.str_inline ? v.v.sso.data : v.v.s;
        return s ? strtoll(s, NULL, 10) : 0;
    }
    /* 高精度族：经字符串中转取整（超长幅值 strtoll 钳到 int64 极值，C 风格精度丢失，不报错） */
    if (v.type == VAL_BIGINT) {
        char* s = v.v.bigint ? lumyr_bigint_to_string(v.v.bigint) : NULL;
        int64_t r = s ? strtoll(s, NULL, 10) : 0;
        free(s);
        return r;
    }
    if (v.type == VAL_DECIMAL) {
        char* s = v.v.decimal ? lumyr_decimal_to_string(v.v.decimal) : NULL;
        int64_t r = s ? strtoll(s, NULL, 10) : 0;
        free(s);
        return r;
    }
    if (v.type == VAL_BITDECIMAL) {
        char* s = v.v.bitdecimal ? lumyr_bitdecimal_to_string(v.v.bitdecimal) : NULL;
        int64_t r = s ? strtoll(s, NULL, 10) : 0;
        free(s);
        return r;
    }
    /* 非数值类型维持原兜底（其余 Value 种类不属于整型族） */
    Value c = lumyr_cast_int64(v);
    if (c.type == VAL_INT64) return c.v.i64;
    if (c.type == VAL_INT) return (int64_t)c.v.i;
    return 0;
}

/* Value → double：浮点/整型族直接取，string 按浮点解析 */
double vm_value_to_f64(Value v) {
    if (v.type == VAL_DOUBLE) return v.v.d;
    if (v.type == VAL_FLOAT) return (double)v.v.f;
    if (v.type == VAL_LONG_DOUBLE) return (double)v.v.ld;
    if (v.type == VAL_INT64) return (double)v.v.i64;
    if (v.type == VAL_INT) return (double)v.v.i;
    if (v.type == VAL_BOOL) return (double)(v.v.b ? 1 : 0);
    if (v.type == VAL_CHAR) return (double)(unsigned char)v.v.c;
    if (v.type == VAL_BYTE) return (double)v.v.by;
    if (v.type == VAL_NONE) return 0.0;
    if (v.type == VAL_STRING) {
        const char* s = v.str_inline ? v.v.sso.data : v.v.s;
        return s ? strtod(s, NULL) : 0.0;
    }
    /* 高精度族：经字符串中转取浮点（精度随 double 表示丢失，不报错） */
    if (v.type == VAL_BIGINT) {
        char* s = v.v.bigint ? lumyr_bigint_to_string(v.v.bigint) : NULL;
        double r = s ? strtod(s, NULL) : 0.0;
        free(s);
        return r;
    }
    if (v.type == VAL_DECIMAL) {
        char* s = v.v.decimal ? lumyr_decimal_to_string(v.v.decimal) : NULL;
        double r = s ? strtod(s, NULL) : 0.0;
        free(s);
        return r;
    }
    if (v.type == VAL_BITDECIMAL) {
        char* s = v.v.bitdecimal ? lumyr_bitdecimal_to_string(v.v.bitdecimal) : NULL;
        double r = s ? strtod(s, NULL) : 0.0;
        free(s);
        return r;
    }
    return (double)vm_value_to_i64(v);
}


/* 按声明类型把机器 int64 装箱为 Value（BOX_INT64 与动态分派返回共用）。
 * ck 决定整型子类型标签：CAST_INT → VAL_INT（"int"）、CAST_BOOL → VAL_BOOL...
 * CAST_NONE/未列出 → VAL_INT64。动态分派若不看被调方 ret 标注一律 make_int64，
 * 会造成同一 `: int` 返回值静态路径报 "int"、动态路径报 "int64" 的不一致。 */
Value vm_box_int64_as(int64_t val, CastKind ck) {
    switch (ck) {
    case CAST_BOOL: return lumyr_make_bool(val);
    case CAST_CHAR:    return lumyr_make_char((char)val);
    case CAST_BYTE:    return lumyr_make_byte((unsigned char)val);
    case CAST_INT8:    return lumyr_make_int8((int8_t)val);
    case CAST_INT16:   return lumyr_make_int16((int16_t)val);
    case CAST_SHORT:   return lumyr_make_short((int16_t)val);
    case CAST_INT32:   return lumyr_make_int32((int32_t)val);
    case CAST_INT_INFER:
        /* 推断软 int：溢出 int32 时装箱为 int64（重赋值类型迁移，大值不被截断） */
        return (val >= INT32_MIN && val <= INT32_MAX) ? lumyr_make_int((int)val)
                                                      : lumyr_make_int64(val);
    case CAST_INT:
    case CAST_ASCII:
        /* 显式 int：严格 C 风格截断（即使溢出也保持 VAL_INT） */
        return lumyr_make_int((int)val);
    case CAST_UINT8:   return lumyr_make_uint8((uint8_t)val);
    case CAST_UCHAR:   return lumyr_make_uchar((unsigned char)val);
    case CAST_UINT16:  return lumyr_make_uint16((uint16_t)val);
    case CAST_USHORT:  return lumyr_make_ushort((unsigned short)val);
    case CAST_UINT32:  return lumyr_make_uint32((uint32_t)val);
    case CAST_UINT:    return lumyr_make_uint((unsigned int)val);
    case CAST_UINT64:  return lumyr_make_uint64((uint64_t)val);
    case CAST_SIZE_T:  return lumyr_make_size_t((size_t)val);
    case CAST_SSIZE_T: return lumyr_make_ssize_t((ssize_t)val);
    case CAST_LONG:    return lumyr_make_long((long)val);
    case CAST_LONGLONG: return lumyr_make_long_long(val);
    case CAST_ULONG:   return lumyr_make_ulong((unsigned long)val);
    default:           return lumyr_make_int64(val);
    }
}

/* BOX_INT64：INT64 栈弹 1 -> Value -> VALUE 栈
   a=CastKind 决定整型子类型（bool/char/byte/int8.../uint64） */
int vm_exec_box_int64(VMExecCtx* ctx, Instruction* in) {
    (void)ctx;
    int64_t val;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &val);
    Value v = vm_box_int64_as(val, (CastKind)in->a);
    stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
    return 1;
}

/* BOX_DOUBLE：DOUBLE 栈弹 1 -> Value -> VALUE 栈
   a=CastKind 决定浮点子类型（float/long double）；CAST_NONE 或未列出 → double */
int vm_exec_box_double(VMExecCtx* ctx, Instruction* in) {
    (void)ctx;
    double val;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &val);
    Value v;
    switch ((CastKind)in->a) {
    case CAST_FLOAT:       v = lumyr_make_float((float)val); break;
    case CAST_LONG_DOUBLE: v = lumyr_make_long_double((long double)val); break;
    default:               v = lumyr_make_double(val); break;
    }
    stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
    return 1;
}

/* BOX_PTR：PTR 栈弹 1，a=CastKind 决定包装成何种 Value -> VALUE 栈 */
int vm_exec_box_ptr(VMExecCtx* ctx, Instruction* in) {
    (void)ctx;
    void* val;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &val);
    Value v;
    memset(&v, 0, sizeof(v));
    switch ((CastKind)in->a) {
    case CAST_STRING:
        v = lumyr_make_string((const char*)val);
        break;
    case CAST_BIGINT:
        v.type = VAL_BIGINT;    v.v.bigint = val;      break;
    case CAST_DECIMAL:
        v.type = VAL_DECIMAL;   v.v.decimal = val;     break;
    case CAST_BITDECIMAL:
        v.type = VAL_BITDECIMAL; v.v.bitdecimal = val; break;
    case CAST_STRUCT_PTR:
        v.type = VAL_STRUCT_PTR; v.v.struct_ptr = val; break;
    case CAST_CLASS_PTR:
        v.type = VAL_CLASS_PTR; v.v.struct_ptr = val; break;
    case CAST_BYTES:
        v.type = VAL_BYTES; v.v.bytes_obj = val; break;
    /* 容器裸指针：装回对应容器 Value，后续动态 INDEX_GET/print 才能识别。
     * 根因修复：装箱身份以 GC 头运行时 vtype 为准，声明 CastKind 仅是静态提示。
     * 此前按声明 CastKind 打标——`value: array` 字段实际持有 TypedArray* 时
     * 被打成 VAL_ARRAY，后续按 ValueArray* 解引用 → SIGSEGV/元素类型丢失。
     * 容器类（array/map/typed_array）互相覆盖；GC 头不可读时回退声明类型。 */
    case CAST_ARRAY:
    case CAST_MAP:
    case CAST_TYPED_ARRAY: {
        int rt = gc_obj_vtype(val);
        if(rt == VAL_ARRAY)       { v.type = VAL_ARRAY;       v.v.array = val;        break; }
        if(rt == VAL_MAP)         { v.type = VAL_MAP;         v.v.map = val;          break; }
        if(rt == VAL_TYPED_ARRAY) { v.type = VAL_TYPED_ARRAY; v.v.typed_array = val;  break; }
        if((CastKind)in->a == CAST_ARRAY)
            v.type = VAL_ARRAY,       v.v.array = val;
        else if((CastKind)in->a == CAST_MAP)
            v.type = VAL_MAP,         v.v.map = val;
        else
            v.type = VAL_TYPED_ARRAY, v.v.typed_array = val;
        break;
    }
    default:
        v.type = VAL_PTR;       v.v.struct_ptr = val;  break;
    }
    stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
    return 1;
}

/* UNBOX_INT64：VALUE 栈弹 1 -> 取 i64 -> INT64 栈
 * 字符串走数值解析（如 (int)"5" -> 5），与 INT64_FROM_STRING 语义一致。
 * 整型族统一走 vm_value_to_i64（直接按 union 成员取值，无 32 位截断） */
int vm_exec_unbox_int64(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    Value v;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &v);
    int64_t val = vm_value_to_i64(v);
    stack_vm_push(g_stack_mgr, STACK_INT64, &val);
    return 1;
}

/* UNBOX_DOUBLE：VALUE 栈弹 1 -> 取 double -> DOUBLE 栈
 * 字符串走浮点解析（如 (double)"1.5" -> 1.5） */
int vm_exec_unbox_double(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    Value v;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &v);
    double val = vm_value_to_f64(v);
    stack_vm_push(g_stack_mgr, STACK_DOUBLE, &val);
    return 1;
}

/* UNBOX_PTR：VALUE 栈弹 1 -> 取裸指针 -> PTR 栈
 * 字符串值需脱离 Value（SSO 内联在 Value 内，弹出后失效）→ 复制为独立 malloc 串，
 * 所有权与 INT64_TO_STRING 等 PTR 栈字符串一致 */
int vm_exec_unbox_ptr(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    Value v;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &v);
    void* p = NULL;
    switch (v.type) {
    case VAL_PTR: case VAL_STRUCT_PTR: case VAL_CLASS_PTR:
        p = v.v.struct_ptr; break;
    case VAL_BIGINT:     p = v.v.bigint; break;
    case VAL_DECIMAL:    p = v.v.decimal; break;
    case VAL_BITDECIMAL: p = v.v.bitdecimal; break;
    case VAL_ARRAY:      p = v.v.array; break;
    case VAL_TYPED_ARRAY: p = v.v.typed_array; break;
    case VAL_MAP:        p = v.v.map; break;
    case VAL_BYTES:      p = v.v.bytes_obj; break;
    case VAL_STRING:
        p = v.str_inline ? strdup(v.v.sso.data) : strdup(v.v.s);
        break;
    case VAL_INT64:  p = (void*)(intptr_t)v.v.i64; break;
    case VAL_INT:    p = (void*)(intptr_t)v.v.i; break;
    case VAL_DOUBLE: p = (void*)(intptr_t)(int64_t)v.v.d; break;
    default: p = NULL; break;
    }
    stack_vm_push(g_stack_mgr, STACK_PTR, &p);
    return 1;
}

/* CAST_STRING：VALUE 栈弹 1 -> 转字符串（malloc，PTR 栈所有）-> PTR 栈
 * 动态值 → 字符串强转/标注的跨栈通路（与 bc_stack 记账 "Value → ptr (string)" 一致） */
int vm_exec_cast_string(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    Value v;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &v);
    char* s = value_to_str(v);   /* malloc，调用方（PTR 栈消费方）持有 */
    stack_vm_push(g_stack_mgr, STACK_PTR, &s);
    return 1;
}

/* TO_BOOL：VALUE 栈弹 1 -> 真值判定 -> 压 VAL_BOOL 到 VALUE 栈
 * 逻辑 && / || 结果归一为 bool，保证运行时类型与静态推断一致 */
int vm_exec_to_bool(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    Value v;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &v);
    Value r = lumyr_make_bool(lumyr_to_bool(v));
    stack_vm_push(g_stack_mgr, STACK_VALUE, &r);
    return 1;
}

/* STR_TO_INT64：PTR 栈弹字符串 -> strtoll -> INT64 栈（不 free，同 FROM_STRING 惯例） */
int vm_exec_str_to_int64(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    char* s;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &s);
    int64_t v = s ? strtoll(s, NULL, 10) : 0;
    stack_vm_push(g_stack_mgr, STACK_INT64, &v);
    return 1;
}

/* STR_TO_DOUBLE：PTR 栈弹字符串 -> strtod -> DOUBLE 栈 */
int vm_exec_str_to_double(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    char* s;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &s);
    double v = s ? strtod(s, NULL) : 0.0;
    stack_vm_push(g_stack_mgr, STACK_DOUBLE, &v);
    return 1;
}
