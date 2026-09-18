/*
 * vm_exec_control.c - VM 控制流指令
 * JMP / JMP_IF / JMP_IF_FALSE
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "lm_value.h"

/* ========== 无条件跳转 ========== */
int vm_exec_control_jmp(VMExecCtx* ctx, Instruction* in) {
    /* in->a = 目标 pc */
    ctx->pc = in->a;
    return 1;
}

/* ========== 条件跳转（INT64 栈非零则跳转） ========== */
int vm_exec_control_jmp_if(VMExecCtx* ctx, Instruction* in) {
    int sp = --g_stack_mgr->sp[STACK_INT64];
    int64_t cond = ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp];
    if (cond != 0) {
        ctx->pc = in->a;
    }
    return 1;
}

/* ========== 条件跳转（INT64 栈为零则跳转） ========== */
int vm_exec_control_jmp_if_false(VMExecCtx* ctx, Instruction* in) {
    int sp = --g_stack_mgr->sp[STACK_INT64];
    int64_t cond = ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp];
    if (cond == 0) {
        ctx->pc = in->a;
    }
    return 1;
}

/* ========== 条件跳转（VALUE 栈非零则跳转） ========== */
int vm_exec_control_jmp_if_value(VMExecCtx* ctx, Instruction* in) {
    int sp = --g_stack_mgr->sp[STACK_VALUE];
    Value cond = ((Value*)g_stack_mgr->stacks[STACK_VALUE])[sp];
    int truthy = 0;
    switch (cond.type) {
        case VAL_INT: truthy = (cond.v.i != 0); break;
        case VAL_DOUBLE: truthy = (cond.v.d != 0.0); break;
        case VAL_BOOL: truthy = cond.v.b; break;
        case VAL_STRING: truthy = (cond.v.s && cond.v.s[0] != '\0'); break;
        default: truthy = 0; break;
    }
    if (truthy) {
        ctx->pc = in->a;
    }
    return 1;
}

/* ========== 条件跳转（VALUE 栈为零则跳转） ========== */
int vm_exec_control_jmp_if_false_value(VMExecCtx* ctx, Instruction* in) {
    int sp = --g_stack_mgr->sp[STACK_VALUE];
    Value cond = ((Value*)g_stack_mgr->stacks[STACK_VALUE])[sp];
    int truthy = 0;
    switch (cond.type) {
        case VAL_INT: truthy = (cond.v.i != 0); break;
        case VAL_DOUBLE: truthy = (cond.v.d != 0.0); break;
        case VAL_BOOL: truthy = cond.v.b; break;
        case VAL_STRING: truthy = (cond.v.s && cond.v.s[0] != '\0'); break;
        default: truthy = 0; break;
    }
    if (!truthy) {
        ctx->pc = in->a;
    }
    return 1;
}
