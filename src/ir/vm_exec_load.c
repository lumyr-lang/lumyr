/*
 * vm_exec_load.c - VM 常量加载指令
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "stack_manager.h"
#include <string.h>

/* ===== 常量加载 ===== */

/* PUSH_INT64_CONST：压入 int64 常量到 INT64 栈（小常量内嵌，a=值） */
int vm_exec_load_int64_const(VMExecCtx* ctx, Instruction* in) {
    int64_t val = (int64_t)in->a;  /* 小常量：a 直接存值（int32 范围） */
    stack_vm_push(g_stack_mgr, STACK_INT64, &val);
    return 1;
}

/* PUSH_CONST_IDX：从统一常量池加载大常量（int64/uint64/double/string） */
int vm_exec_load_const_idx(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    ConstEntry* e = &ctx->const_pool[idx];
    switch(e->type) {
        case CONST_INT64: {
            stack_vm_push(g_stack_mgr, STACK_INT64, &e->i64);
            break;
        }
        case CONST_UINT64: {
            int64_t val = (int64_t)e->u64;
            stack_vm_push(g_stack_mgr, STACK_INT64, &val);
            break;
        }
        case CONST_DOUBLE: {
            stack_vm_push(g_stack_mgr, STACK_DOUBLE, &e->d);
            break;
        }
        case CONST_STRING: {
            void* s = (void*)e->s;
            stack_vm_push(g_stack_mgr, STACK_PTR, &s);
            break;
        }
    }
    return 1;
}

/* PUSH_DOUBLE_CONST：压入 double 常量到 DOUBLE 栈 */
int vm_exec_load_double_const(VMExecCtx* ctx, Instruction* in) {
    uint64_t bits = ((uint64_t)in->a) | ((uint64_t)in->b << 32);
    double val;
    memcpy(&val, &bits, sizeof(double));
    stack_vm_push(g_stack_mgr, STACK_DOUBLE, &val);
    return 1;
}

/* PUSH_PTR_CONST：压入指针常量到 PTR 栈 */
int vm_exec_load_ptr_const(VMExecCtx* ctx, Instruction* in) {
    void* val = (void*)(intptr_t)in->a;
    stack_vm_push(g_stack_mgr, STACK_PTR, &val);
    return 1;
}

/* LOAD_STRING_CONST：从字符串常量池加载字符串，压入 PTR 栈 */
int vm_exec_load_string_const(VMExecCtx* ctx, Instruction* in) {
    int idx = in->a;
    const char* s = ctx->const_pool[idx].s;
    void* ptr = (void*)s;
    stack_vm_push(g_stack_mgr, STACK_PTR, &ptr);
    return 1;
}
