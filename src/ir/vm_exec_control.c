/*
 * vm_exec_control.c - VM 控制流指令
 * 通过栈管理器统一操作
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "lm_value.h"

/* ========== 无条件跳转 ========== */
int vm_exec_control_jmp(VMExecCtx* ctx, Instruction* in) {
    ctx->pc = in->a;
    return 1;
}

/* ========== 条件跳转（INT64 栈非零则跳转） ========== */
int vm_exec_control_jmp_if_true(VMExecCtx* ctx, Instruction* in) {
    int64_t cond;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &cond);
    if (cond != 0) {
        ctx->pc = in->a;
    }
    return 1;
}

/* ========== 条件跳转（INT64 栈为零则跳转） ========== */
int vm_exec_control_jmp_if_false(VMExecCtx* ctx, Instruction* in) {
    int64_t cond;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &cond);
    if (cond == 0) {
        ctx->pc = in->a;
    }
    return 1;
}

/* ========== 条件跳转（VALUE 栈 truthy 则跳转） ========== */
int vm_exec_control_jmp_if_true_value(VMExecCtx* ctx, Instruction* in) {
    Value cond;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &cond);
    if (lumyr_to_bool(cond)) {
        ctx->pc = in->a;
    }
    return 1;
}

/* ========== 条件跳转（VALUE 栈 falsy 则跳转） ========== */
int vm_exec_control_jmp_if_false_value(VMExecCtx* ctx, Instruction* in) {
    Value cond;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &cond);
    if (!lumyr_to_bool(cond)) {
        ctx->pc = in->a;
    }
    return 1;
}
