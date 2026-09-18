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

    switch(builtin_id) {
    case BUILTIN_TYPE: {
        /* type(x)：从 INT64 栈或 DOUBLE 栈或 PTR 栈弹出一个值，返回类型名字符串 */
        /* 简化：从 VALUE 栈弹出 */
        if(g_stack_mgr->sp[STACK_VALUE] > 0) {
            int sp = --g_stack_mgr->sp[STACK_VALUE];
            Value v = ((Value*)g_stack_mgr->stacks[STACK_VALUE])[sp];
            /* 根据值的类型返回类型名 */
            const char* type_name = "unknown";
            switch(v.type) {
            case VAL_INT: type_name = "int"; break;
            case VAL_DOUBLE: type_name = "double"; break;
            case VAL_STRING: type_name = "string"; break;
            case VAL_BOOL: type_name = "bool"; break;
            case VAL_CHAR: type_name = "char"; break;
            case VAL_BYTE: type_name = "byte"; break;
            case VAL_NONE: type_name = "null"; break;
            default: type_name = "unknown"; break;
            }
            /* 把类型名字符串压入 PTR 栈 */
            const char* s = strdup(type_name);
            int new_sp = g_stack_mgr->sp[STACK_PTR]++;
            ((void**)g_stack_mgr->stacks[STACK_PTR])[new_sp] = (void*)s;
        }
        return 1;
    }
    default:
        fprintf(stderr, "VM: unknown builtin %d\n", builtin_id);
        return 0;
    }
}
