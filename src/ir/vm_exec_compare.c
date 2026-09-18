/*
 * vm_exec_compare.c - VM 比较运算指令
 * 比较结果压入 INT64 栈（1=真，0=假）
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "lm_value.h"

/* ========== 比较运算（INT64 栈专用） ========== */

/* INT64 栈等于 */
int vm_exec_compare_int64_eq(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_INT64];
    int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
    int64_t b = stk[sp - 1];
    int64_t a = stk[sp - 2];
    stk[sp - 2] = (a == b) ? 1 : 0;
    g_stack_mgr->sp[STACK_INT64] -= 1;
    return 1;
}

/* INT64 栈不等于 */
int vm_exec_compare_int64_ne(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_INT64];
    int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
    int64_t b = stk[sp - 1];
    int64_t a = stk[sp - 2];
    stk[sp - 2] = (a != b) ? 1 : 0;
    g_stack_mgr->sp[STACK_INT64] -= 1;
    return 1;
}

/* INT64 栈大于 */
int vm_exec_compare_int64_gt(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_INT64];
    int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
    int64_t b = stk[sp - 1];
    int64_t a = stk[sp - 2];
    stk[sp - 2] = (a > b) ? 1 : 0;
    g_stack_mgr->sp[STACK_INT64] -= 1;
    return 1;
}

/* INT64 栈小于 */
int vm_exec_compare_int64_lt(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_INT64];
    int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
    int64_t b = stk[sp - 1];
    int64_t a = stk[sp - 2];
    stk[sp - 2] = (a < b) ? 1 : 0;
    g_stack_mgr->sp[STACK_INT64] -= 1;
    return 1;
}

/* INT64 栈大于等于 */
int vm_exec_compare_int64_ge(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_INT64];
    int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
    int64_t b = stk[sp - 1];
    int64_t a = stk[sp - 2];
    stk[sp - 2] = (a >= b) ? 1 : 0;
    g_stack_mgr->sp[STACK_INT64] -= 1;
    return 1;
}

/* INT64 栈小于等于 */
int vm_exec_compare_int64_le(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_INT64];
    int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
    int64_t b = stk[sp - 1];
    int64_t a = stk[sp - 2];
    stk[sp - 2] = (a <= b) ? 1 : 0;
    g_stack_mgr->sp[STACK_INT64] -= 1;
    return 1;
}

/* ========== 比较运算（DOUBLE 栈专用） ========== */

/* DOUBLE 栈等于 */
int vm_exec_compare_double_eq(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_DOUBLE];
    double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
    double b = stk[sp - 1];
    double a = stk[sp - 2];
    int result = (a == b) ? 1 : 0;
    g_stack_mgr->sp[STACK_DOUBLE] -= 1;
    int sp_i = g_stack_mgr->sp[STACK_INT64]++;
    ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp_i] = result;
    return 1;
}

/* DOUBLE 栈大于 */
int vm_exec_compare_double_gt(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_DOUBLE];
    double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
    double b = stk[sp - 1];
    double a = stk[sp - 2];
    int result = (a > b) ? 1 : 0;
    g_stack_mgr->sp[STACK_DOUBLE] -= 1;
    int sp_i = g_stack_mgr->sp[STACK_INT64]++;
    ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp_i] = result;
    return 1;
}

/* DOUBLE 栈小于 */
int vm_exec_compare_double_lt(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_DOUBLE];
    double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
    double b = stk[sp - 1];
    double a = stk[sp - 2];
    int result = (a < b) ? 1 : 0;
    g_stack_mgr->sp[STACK_DOUBLE] -= 1;
    int sp_i = g_stack_mgr->sp[STACK_INT64]++;
    ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp_i] = result;
    return 1;
}
