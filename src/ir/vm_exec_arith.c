/*
 * vm_exec_arith.c - VM 算术运算指令
 * 4 核心栈设计：INT64 栈 / DOUBLE 栈 / VALUE 栈
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "lm_value.h"
#include <string.h>

/* ========== 算术运算（INT64 栈专用） ========== */

/* INT64 栈加法 */
int vm_exec_arith_int64_add(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_INT64];
    int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
    int64_t b = stk[sp - 1];
    int64_t a = stk[sp - 2];
    stk[sp - 2] = a + b;
    g_stack_mgr->sp[STACK_INT64] -= 1;
    return 1;
}

/* INT64 栈减法 */
int vm_exec_arith_int64_sub(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_INT64];
    int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
    int64_t b = stk[sp - 1];
    int64_t a = stk[sp - 2];
    stk[sp - 2] = a - b;
    g_stack_mgr->sp[STACK_INT64] -= 1;
    return 1;
}

/* INT64 栈乘法 */
int vm_exec_arith_int64_mul(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_INT64];
    int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
    int64_t b = stk[sp - 1];
    int64_t a = stk[sp - 2];
    stk[sp - 2] = a * b;
    g_stack_mgr->sp[STACK_INT64] -= 1;
    return 1;
}

/* INT64 栈除法 */
int vm_exec_arith_int64_div(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_INT64];
    int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
    int64_t b = stk[sp - 1];
    int64_t a = stk[sp - 2];
    if (b != 0) {
        stk[sp - 2] = a / b;
    } else {
        stk[sp - 2] = 0;
    }
    g_stack_mgr->sp[STACK_INT64] -= 1;
    return 1;
}

/* INT64 栈取模 */
int vm_exec_arith_int64_mod(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_INT64];
    int64_t* stk = (int64_t*)g_stack_mgr->stacks[STACK_INT64];
    int64_t b = stk[sp - 1];
    int64_t a = stk[sp - 2];
    if (b != 0) {
        stk[sp - 2] = a % b;
    } else {
        stk[sp - 2] = 0;
    }
    g_stack_mgr->sp[STACK_INT64] -= 1;
    return 1;
}

/* ========== 算术运算（DOUBLE 栈专用） ========== */

/* DOUBLE 栈加法 */
int vm_exec_arith_double_add(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_DOUBLE];
    double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
    double b = stk[sp - 1];
    double a = stk[sp - 2];
    stk[sp - 2] = a + b;
    g_stack_mgr->sp[STACK_DOUBLE] -= 1;
    return 1;
}

/* DOUBLE 栈减法 */
int vm_exec_arith_double_sub(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_DOUBLE];
    double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
    double b = stk[sp - 1];
    double a = stk[sp - 2];
    stk[sp - 2] = a - b;
    g_stack_mgr->sp[STACK_DOUBLE] -= 1;
    return 1;
}

/* DOUBLE 栈乘法 */
int vm_exec_arith_double_mul(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_DOUBLE];
    double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
    double b = stk[sp - 1];
    double a = stk[sp - 2];
    stk[sp - 2] = a * b;
    g_stack_mgr->sp[STACK_DOUBLE] -= 1;
    return 1;
}

/* DOUBLE 栈除法 */
int vm_exec_arith_double_div(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_DOUBLE];
    double* stk = (double*)g_stack_mgr->stacks[STACK_DOUBLE];
    double b = stk[sp - 1];
    double a = stk[sp - 2];
    if (b != 0.0) {
        stk[sp - 2] = a / b;
    } else {
        stk[sp - 2] = 0.0;
    }
    g_stack_mgr->sp[STACK_DOUBLE] -= 1;
    return 1;
}
