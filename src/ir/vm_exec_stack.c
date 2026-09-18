/*
 * vm_exec_stack.c - VM 栈操作指令
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "stack_manager.h"

/* ===== 栈操作 ===== */

/* POP：弹出 VALUE 栈顶 */
int vm_exec_stack_pop(VMExecCtx* ctx, Instruction* in) {
    Value val;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &val);
    return 1;
}

/* DUP：复制 VALUE 栈顶 */
int vm_exec_stack_dup(VMExecCtx* ctx, Instruction* in) {
    Value val;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &val);
    stack_vm_push(g_stack_mgr, STACK_VALUE, &val);
    stack_vm_push(g_stack_mgr, STACK_VALUE, &val);
    return 1;
}
