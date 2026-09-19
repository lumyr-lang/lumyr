/*
 * vm_exec_type.c - VM 类型转换指令
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "ir_types.h"

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
