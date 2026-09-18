/*
 * vm_exec_compare.c - VM 比较运算指令
 * 通过栈管理器统一操作，比较结果压入 INT64 栈
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "lm_value.h"

/* ========== 比较运算（INT64 栈专用） ========== */

/* INT64 栈等于 */
int vm_exec_compare_int64_eq(VMExecCtx* ctx, Instruction* in) {
    int64_t a, b;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &b);
    stack_vm_pop(g_stack_mgr, STACK_INT64, &a);
    int64_t result = (a == b) ? 1 : 0;
    stack_vm_push(g_stack_mgr, STACK_INT64, &result);
    return 1;
}

/* INT64 栈不等于 */
int vm_exec_compare_int64_ne(VMExecCtx* ctx, Instruction* in) {
    int64_t a, b;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &b);
    stack_vm_pop(g_stack_mgr, STACK_INT64, &a);
    int64_t result = (a != b) ? 1 : 0;
    stack_vm_push(g_stack_mgr, STACK_INT64, &result);
    return 1;
}

/* INT64 栈大于 */
int vm_exec_compare_int64_gt(VMExecCtx* ctx, Instruction* in) {
    int64_t a, b;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &b);
    stack_vm_pop(g_stack_mgr, STACK_INT64, &a);
    int64_t result = (a > b) ? 1 : 0;
    stack_vm_push(g_stack_mgr, STACK_INT64, &result);
    return 1;
}

/* INT64 栈小于 */
int vm_exec_compare_int64_lt(VMExecCtx* ctx, Instruction* in) {
    int64_t a, b;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &b);
    stack_vm_pop(g_stack_mgr, STACK_INT64, &a);
    int64_t result = (a < b) ? 1 : 0;
    stack_vm_push(g_stack_mgr, STACK_INT64, &result);
    return 1;
}

/* INT64 栈大于等于 */
int vm_exec_compare_int64_ge(VMExecCtx* ctx, Instruction* in) {
    int64_t a, b;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &b);
    stack_vm_pop(g_stack_mgr, STACK_INT64, &a);
    int64_t result = (a >= b) ? 1 : 0;
    stack_vm_push(g_stack_mgr, STACK_INT64, &result);
    return 1;
}

/* INT64 栈小于等于 */
int vm_exec_compare_int64_le(VMExecCtx* ctx, Instruction* in) {
    int64_t a, b;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &b);
    stack_vm_pop(g_stack_mgr, STACK_INT64, &a);
    int64_t result = (a <= b) ? 1 : 0;
    stack_vm_push(g_stack_mgr, STACK_INT64, &result);
    return 1;
}

/* ========== 比较运算（DOUBLE 栈专用） ========== */

/* DOUBLE 栈等于 */
int vm_exec_compare_double_eq(VMExecCtx* ctx, Instruction* in) {
    double a, b;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &b);
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &a);
    int64_t result = (a == b) ? 1 : 0;
    stack_vm_push(g_stack_mgr, STACK_INT64, &result);
    return 1;
}

/* DOUBLE 栈大于 */
int vm_exec_compare_double_gt(VMExecCtx* ctx, Instruction* in) {
    double a, b;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &b);
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &a);
    int64_t result = (a > b) ? 1 : 0;
    stack_vm_push(g_stack_mgr, STACK_INT64, &result);
    return 1;
}

/* DOUBLE 栈小于 */
int vm_exec_compare_double_lt(VMExecCtx* ctx, Instruction* in) {
    double a, b;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &b);
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &a);
    int64_t result = (a < b) ? 1 : 0;
    stack_vm_push(g_stack_mgr, STACK_INT64, &result);
    return 1;
}
