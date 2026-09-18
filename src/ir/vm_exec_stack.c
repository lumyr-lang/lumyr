/*
 * vm_exec_stack.c - VM 栈操作指令
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "stack_manager.h"

/* ===== 栈操作 ===== */

/* POP：弹出 VALUE 栈顶 */
int vm_exec_stack_pop(VMExecCtx* ctx, Instruction* in) {
    g_stack_mgr->sp[STACK_VALUE]--;
    return 1;
}

/* DUP：复制 VALUE 栈顶 */
int vm_exec_stack_dup(VMExecCtx* ctx, Instruction* in) {
    int sp = g_stack_mgr->sp[STACK_VALUE];
    Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
    stk[sp] = stk[sp - 1];
    g_stack_mgr->sp[STACK_VALUE]++;
    return 1;
}
