/*
 * vm_exec_load.c - VM 常量加载指令
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "stack_manager.h"
#include <string.h>

/* ===== 常量加载 ===== */

/* LOAD_CONST：从常量池加载 Value 到 VALUE 栈 */
int vm_exec_load_const(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
    int sp = g_stack_mgr->sp[STACK_VALUE]++;
    stk[sp] = ctx->consts[idx];
    return 1;
}

/* PUSH_INT64_CONST：压入 int64 常量到 INT64 栈 */
int vm_exec_load_int64_const(VMExecCtx* ctx, Instruction* in) {
    int64_t val = ((int64_t)in->a) | ((int64_t)in->b << 32);
    int sp = g_stack_mgr->sp[STACK_INT64]++;
    ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp] = val;
    return 1;
}

/* PUSH_DOUBLE_CONST：压入 double 常量到 DOUBLE 栈 */
int vm_exec_load_double_const(VMExecCtx* ctx, Instruction* in) {
    uint64_t bits = ((uint64_t)in->a) | ((uint64_t)in->b << 32);
    double val;
    memcpy(&val, &bits, sizeof(double));
    int sp = g_stack_mgr->sp[STACK_DOUBLE]++;
    ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp] = val;
    return 1;
}

/* PUSH_PTR_CONST：压入指针常量到 PTR 栈 */
int vm_exec_load_ptr_const(VMExecCtx* ctx, Instruction* in) {
    void* val = (void*)(intptr_t)in->a;
    int sp = g_stack_mgr->sp[STACK_PTR]++;
    ((void**)g_stack_mgr->stacks[STACK_PTR])[sp] = val;
    return 1;
}

/* LOAD_STRING_CONST：从字符串常量池加载指针到 PTR 栈 */
int vm_exec_load_string_const(VMExecCtx* ctx, Instruction* in) {
    const char* s = ctx->string_consts[in->a];
    int sp = g_stack_mgr->sp[STACK_PTR]++;
    ((void**)g_stack_mgr->stacks[STACK_PTR])[sp] = (void*)s;
    return 1;
}
