/*
 * vm_exec.c - VM 主执行循环
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */
#include "vm_types.h"
#include "vm.h"
#include "stack_manager.h"
#include "lumyr_value_type.h"
#include "lumyr_value.h"
#include "lm_value.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== 主执行循环 ========== */
Value vm_execute(VMExecCtx* ctx) {
    Instruction* code = ctx->code;
    int pc = 0;
    Value result = val_none();

    /* 初始化全局栈管理器 */
    if (!g_stack_mgr) {
        stack_global_init(256);
    }

    while (pc < ctx->fn->code_len) {
        Instruction in = code[pc++];
        switch (in.op) {
        case OPC_NOP:
            break;

        /* ===== 栈操作 ===== */
        case OPC_POP:
            g_stack_mgr->sp[STACK_VALUE]--;
            break;

        case OPC_DUP: {
            int sp = g_stack_mgr->sp[STACK_VALUE];
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            stk[sp] = stk[sp - 1];
            g_stack_mgr->sp[STACK_VALUE]++;
            break;
        }

        /* ===== 常量加载 ===== */
        case OPC_LOAD_CONST: {
            int idx = in.a;
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            int sp = g_stack_mgr->sp[STACK_VALUE]++;
            stk[sp] = ctx->consts[idx];
            break;
        }

        case OPC_PUSH_INT64_CONST: {
            int64_t val = ((int64_t)in.a) | ((int64_t)in.b << 32);
            int sp = g_stack_mgr->sp[STACK_INT64]++;
            ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp] = val;
            break;
        }

        case OPC_PUSH_DOUBLE_CONST: {
            uint64_t bits = ((uint64_t)in.a) | ((uint64_t)in.b << 32);
            double val;
            memcpy(&val, &bits, sizeof(double));
            int sp = g_stack_mgr->sp[STACK_DOUBLE]++;
            ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp] = val;
            break;
        }

        case OPC_PUSH_PTR_CONST: {
            void* val = (void*)(intptr_t)in.a;
            int sp = g_stack_mgr->sp[STACK_PTR]++;
            ((void**)g_stack_mgr->stacks[STACK_PTR])[sp] = val;
            break;
        }

        /* ===== 变量加载/存储 ===== */
        case OPC_LOAD_VAR: {
            int idx = in.a;
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            int sp = g_stack_mgr->sp[STACK_VALUE]++;
            stk[sp] = ctx->frame->vals[idx];
            break;
        }

        case OPC_STORE_VAR: {
            int idx = in.a;
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            ctx->frame->vals[idx] = stk[--g_stack_mgr->sp[STACK_VALUE]];
            break;
        }

        case OPC_LOAD_INT64_VAR: {
            int idx = in.a;
            int sp = g_stack_mgr->sp[STACK_INT64]++;
            ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp] = ctx->frame->int_slots[idx];
            break;
        }

        case OPC_STORE_INT64_VAR: {
            int idx = in.a;
            int sp = --g_stack_mgr->sp[STACK_INT64];
            ctx->frame->int_slots[idx] = ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp];
            break;
        }

        case OPC_LOAD_DOUBLE_VAR: {
            int idx = in.a;
            int sp = g_stack_mgr->sp[STACK_DOUBLE]++;
            ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp] = ctx->frame->flt_slots[idx];
            break;
        }

        case OPC_STORE_DOUBLE_VAR: {
            int idx = in.a;
            int sp = --g_stack_mgr->sp[STACK_DOUBLE];
            ctx->frame->flt_slots[idx] = ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp];
            break;
        }

        case OPC_LOAD_PTR_VAR: {
            int idx = in.a;
            int sp = g_stack_mgr->sp[STACK_PTR]++;
            ((void**)g_stack_mgr->stacks[STACK_PTR])[sp] = ctx->frame->ptr_slots[idx];
            break;
        }

        case OPC_STORE_PTR_VAR: {
            int idx = in.a;
            int sp = --g_stack_mgr->sp[STACK_PTR];
            ctx->frame->ptr_slots[idx] = ((void**)g_stack_mgr->stacks[STACK_PTR])[sp];
            break;
        }

        /* ===== 算术运算 ===== */
        case OPC_ADD: {
            /* 简化：Value 栈加法 */
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            int sp = --g_stack_mgr->sp[STACK_VALUE];
            Value b = stk[sp];
            Value a = stk[sp - 1];
            stk[sp - 1] = lumyr_add(a, b);
            g_stack_mgr->sp[STACK_VALUE]--;
            break;
        }

        /* ===== 打印 ===== */
        case OPC_PRINT: {
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            int sp = --g_stack_mgr->sp[STACK_VALUE];
            lumyr_print(stk[sp]);
            break;
        }

        /* ===== 返回 ===== */
        case OPC_RETURN: {
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            if(g_stack_mgr->sp[STACK_VALUE] > 0) {
                result = stk[--g_stack_mgr->sp[STACK_VALUE]];
            }
            goto done;
        }

        default:
            fprintf(stderr, "VM: unknown opcode %d at pc %d\n", (int)in.op, pc-1);
            goto done;
        }
    }

done:
    return result;
}
