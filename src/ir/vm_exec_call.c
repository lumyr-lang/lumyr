/*
 * vm_exec_call.c - VM 函数调用指令
 * CALL / RETURN
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "ast/stackframe.h"
#include "lm_value.h"
#include <stdio.h>
#include <stdlib.h>

/* ========== 函数调用 ========== */
int vm_exec_call(VMExecCtx* ctx, Instruction* in) {
    /* in->a = 函数索引，in->b = 参数个数 */
    int func_idx = in->a;
    int argc = in->b;
    
    /* TODO: 从函数表获取函数 */
    /* 当前简化：打印调试信息 */
    fprintf(stderr, "VM: CALL func_idx=%d argc=%d\n", func_idx, argc);
    
    return 1;
}

/* ========== 函数返回 ========== */
int vm_exec_return(VMExecCtx* ctx, Instruction* in) {
    /* 从栈顶获取返回值 */
    if (g_stack_mgr->sp[STACK_VALUE] > 0) {
        int sp = --g_stack_mgr->sp[STACK_VALUE];
        Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
    }
    return 1;
}

/* ========== 调用内置函数 ========== */
int vm_exec_builtin(VMExecCtx* ctx, Instruction* in) {
    /* in->a = 内置函数 ID，in->b = 参数个数 */
    int builtin_id = in->a;
    int argc = in->b;
    
    /* TODO: 从内置函数表获取函数 */
    /* 当前简化：打印调试信息 */
    fprintf(stderr, "VM: BUILTIN id=%d argc=%d\n", builtin_id, argc);
    
    return 1;
}
