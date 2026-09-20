/*
 * vm_exec_type.c - VM 类型转换指令
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "ir_types.h"
#include "lm_value.h"
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

/* NEG：负号，a 字段存表达式类型（INT 或 DOUBLE） */
int vm_exec_type_neg(VMExecCtx* ctx, Instruction* in) {
    if(in->a == (int)EXPR_TYPE_DOUBLE) {
        double val;
        stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &val);
        val = -val;
        stack_vm_push(g_stack_mgr, STACK_DOUBLE, &val);
    } else {
        int64_t val;
        stack_vm_pop(g_stack_mgr, STACK_INT64, &val);
        val = -val;
        stack_vm_push(g_stack_mgr, STACK_INT64, &val);
    }
    return 1;
}

/* VNEG：通用 Value 一元负（动态兜底：VALUE 栈） */
int vm_exec_vneg(VMExecCtx* ctx, Instruction* in) {
    Value val;
    (void)ctx; (void)in;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &val);
    Value r = lumyr_unary_minus(val);
    stack_vm_push(g_stack_mgr, STACK_VALUE, &r);
    return 1;
}

/* ===== typed 栈 -> VALUE 栈装箱（实参 typed、形参动态 NONE 时绑定用） ===== */

/* BOX_INT64：INT64 栈弹 1 -> Value -> VALUE 栈 */
int vm_exec_box_int64(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    int64_t val;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &val);
    Value v = lumyr_make_int64(val);
    stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
    return 1;
}

/* BOX_DOUBLE：DOUBLE 栈弹 1 -> Value -> VALUE 栈 */
int vm_exec_box_double(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    double val;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &val);
    Value v = lumyr_make_double(val);
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
    default:
        v.type = VAL_PTR;       v.v.struct_ptr = val;  break;
    }
    stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
    return 1;
}

/* UNBOX_INT64：VALUE 栈弹 1 -> 取 i64 -> INT64 栈
 * 字符串走数值解析（如 (int)"5" -> 5），与 INT64_FROM_STRING 语义一致 */
int vm_exec_unbox_int64(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    Value v;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &v);
    int64_t val = 0;
    if (v.type == VAL_INT64) val = v.v.i64;
    else if (v.type == VAL_INT) val = (int64_t)v.v.i;
    else if (v.type == VAL_BOOL) val = (int64_t)(v.v.i ? 1 : 0);
    else if (v.type == VAL_DOUBLE) val = (int64_t)v.v.d;
    else if (v.type == VAL_NONE) val = 0;
    else if (v.type == VAL_STRING) {
        const char* s = v.str_inline ? v.v.sso.data : v.v.s;
        val = s ? strtoll(s, NULL, 10) : 0;
    }
    else {
        Value c = lumyr_cast_int64(v);
        if (c.type == VAL_INT64) val = c.v.i64;
    }
    stack_vm_push(g_stack_mgr, STACK_INT64, &val);
    return 1;
}

/* UNBOX_DOUBLE：VALUE 栈弹 1 -> 取 double -> DOUBLE 栈
 * 字符串走浮点解析（如 (double)"1.5" -> 1.5） */
int vm_exec_unbox_double(VMExecCtx* ctx, Instruction* in) {
    (void)ctx; (void)in;
    Value v;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &v);
    double val = 0.0;
    if (v.type == VAL_DOUBLE) val = v.v.d;
    else if (v.type == VAL_INT64) val = (double)v.v.i64;
    else if (v.type == VAL_INT) val = (double)v.v.i;
    else if (v.type == VAL_BOOL) val = (double)(v.v.i ? 1 : 0);
    else if (v.type == VAL_NONE) val = 0.0;
    else if (v.type == VAL_STRING) {
        const char* s = v.str_inline ? v.v.sso.data : v.v.s;
        val = s ? strtod(s, NULL) : 0.0;
    }
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
    case VAL_MAP:        p = v.v.map; break;
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
