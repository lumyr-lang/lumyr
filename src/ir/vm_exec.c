/*
 * vm_exec.c - VM 主执行循环
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 * 指令按功能模块拆分：arith / compare / control / call
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

/* ========== 模块函数声明 ========== */

/* 算术运算 */
int vm_exec_arith_int64_add(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_sub(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_mul(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_div(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_mod(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_double_add(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_double_sub(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_double_mul(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_double_div(VMExecCtx* ctx, Instruction* in);

/* 比较运算 */
int vm_exec_compare_int64_eq(VMExecCtx* ctx, Instruction* in);
int vm_exec_compare_int64_ne(VMExecCtx* ctx, Instruction* in);
int vm_exec_compare_int64_gt(VMExecCtx* ctx, Instruction* in);
int vm_exec_compare_int64_lt(VMExecCtx* ctx, Instruction* in);
int vm_exec_compare_int64_ge(VMExecCtx* ctx, Instruction* in);
int vm_exec_compare_int64_le(VMExecCtx* ctx, Instruction* in);
int vm_exec_compare_double_eq(VMExecCtx* ctx, Instruction* in);
int vm_exec_compare_double_gt(VMExecCtx* ctx, Instruction* in);
int vm_exec_compare_double_lt(VMExecCtx* ctx, Instruction* in);

/* 控制流 */
int vm_exec_control_jmp(VMExecCtx* ctx, Instruction* in);
int vm_exec_control_jmp_if(VMExecCtx* ctx, Instruction* in);
int vm_exec_control_jmp_if_false(VMExecCtx* ctx, Instruction* in);
int vm_exec_control_jmp_if_value(VMExecCtx* ctx, Instruction* in);
int vm_exec_control_jmp_if_false_value(VMExecCtx* ctx, Instruction* in);

/* 函数调用 */
int vm_exec_call(VMExecCtx* ctx, Instruction* in);
int vm_exec_return(VMExecCtx* ctx, Instruction* in);
int vm_exec_builtin(VMExecCtx* ctx, Instruction* in);

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
        int handled = 0;

        switch (in.op) {
        case OPC_NOP:
            handled = 1;
            break;

        /* ===== 栈操作 ===== */
        case OPC_POP:
            g_stack_mgr->sp[STACK_VALUE]--;
            handled = 1;
            break;

        case OPC_DUP: {
            int sp = g_stack_mgr->sp[STACK_VALUE];
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            stk[sp] = stk[sp - 1];
            g_stack_mgr->sp[STACK_VALUE]++;
            handled = 1;
            break;
        }

        /* ===== 常量加载 ===== */
        case OPC_LOAD_CONST: {
            int idx = in.a;
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            int sp = g_stack_mgr->sp[STACK_VALUE]++;
            stk[sp] = ctx->consts[idx];
            handled = 1;
            break;
        }

        case OPC_PUSH_INT64_CONST: {
            int64_t val = ((int64_t)in.a) | ((int64_t)in.b << 32);
            int sp = g_stack_mgr->sp[STACK_INT64]++;
            ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp] = val;
            handled = 1;
            break;
        }

        case OPC_PUSH_DOUBLE_CONST: {
            uint64_t bits = ((uint64_t)in.a) | ((uint64_t)in.b << 32);
            double val;
            memcpy(&val, &bits, sizeof(double));
            int sp = g_stack_mgr->sp[STACK_DOUBLE]++;
            ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp] = val;
            handled = 1;
            break;
        }

        case OPC_PUSH_PTR_CONST: {
            void* val = (void*)(intptr_t)in.a;
            int sp = g_stack_mgr->sp[STACK_PTR]++;
            ((void**)g_stack_mgr->stacks[STACK_PTR])[sp] = val;
            handled = 1;
            break;
        }

        /* ===== 变量加载/存储 ===== */
        case OPC_LOAD_VAR: {
            int idx = in.a;
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            int sp = g_stack_mgr->sp[STACK_VALUE]++;
            stk[sp] = ctx->frame->vals[idx];
            handled = 1;
            break;
        }

        case OPC_STORE_VAR: {
            int idx = in.a;
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            ctx->frame->vals[idx] = stk[--g_stack_mgr->sp[STACK_VALUE]];
            handled = 1;
            break;
        }

        case OPC_LOAD_INT64_VAR: {
            int idx = in.a;
            int sp = g_stack_mgr->sp[STACK_INT64]++;
            ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp] = ctx->frame->int_slots[idx];
            handled = 1;
            break;
        }

        case OPC_STORE_INT64_VAR: {
            int idx = in.a;
            int sp = --g_stack_mgr->sp[STACK_INT64];
            ctx->frame->int_slots[idx] = ((int64_t*)g_stack_mgr->stacks[STACK_INT64])[sp];
            handled = 1;
            break;
        }

        case OPC_LOAD_DOUBLE_VAR: {
            int idx = in.a;
            int sp = g_stack_mgr->sp[STACK_DOUBLE]++;
            ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp] = ctx->frame->flt_slots[idx];
            handled = 1;
            break;
        }

        case OPC_STORE_DOUBLE_VAR: {
            int idx = in.a;
            int sp = --g_stack_mgr->sp[STACK_DOUBLE];
            ctx->frame->flt_slots[idx] = ((double*)g_stack_mgr->stacks[STACK_DOUBLE])[sp];
            handled = 1;
            break;
        }

        case OPC_LOAD_PTR_VAR: {
            int idx = in.a;
            int sp = g_stack_mgr->sp[STACK_PTR]++;
            ((void**)g_stack_mgr->stacks[STACK_PTR])[sp] = ctx->frame->ptr_slots[idx];
            handled = 1;
            break;
        }

        case OPC_STORE_PTR_VAR: {
            int idx = in.a;
            int sp = --g_stack_mgr->sp[STACK_PTR];
            ctx->frame->ptr_slots[idx] = ((void**)g_stack_mgr->stacks[STACK_PTR])[sp];
            handled = 1;
            break;
        }

        /* ===== 算术运算（INT64 栈） ===== */
        case OPC_INT64_ADD: handled = vm_exec_arith_int64_add(ctx, &in); break;
        case OPC_INT64_SUB: handled = vm_exec_arith_int64_sub(ctx, &in); break;
        case OPC_INT64_MUL: handled = vm_exec_arith_int64_mul(ctx, &in); break;
        case OPC_INT64_DIV: handled = vm_exec_arith_int64_div(ctx, &in); break;
        case OPC_INT64_MOD: handled = vm_exec_arith_int64_mod(ctx, &in); break;

        /* ===== 算术运算（DOUBLE 栈） ===== */
        case OPC_DOUBLE_ADD: handled = vm_exec_arith_double_add(ctx, &in); break;
        case OPC_DOUBLE_SUB: handled = vm_exec_arith_double_sub(ctx, &in); break;
        case OPC_DOUBLE_MUL: handled = vm_exec_arith_double_mul(ctx, &in); break;
        case OPC_DOUBLE_DIV: handled = vm_exec_arith_double_div(ctx, &in); break;

        /* ===== 比较运算（INT64 栈） ===== */
        case OPC_INT64_EQ: handled = vm_exec_compare_int64_eq(ctx, &in); break;
        case OPC_INT64_NE: handled = vm_exec_compare_int64_ne(ctx, &in); break;
        case OPC_INT64_GT: handled = vm_exec_compare_int64_gt(ctx, &in); break;
        case OPC_INT64_LT: handled = vm_exec_compare_int64_lt(ctx, &in); break;
        case OPC_INT64_GE: handled = vm_exec_compare_int64_ge(ctx, &in); break;
        case OPC_INT64_LE: handled = vm_exec_compare_int64_le(ctx, &in); break;

        /* ===== 比较运算（DOUBLE 栈） ===== */
        case OPC_DOUBLE_EQ: handled = vm_exec_compare_double_eq(ctx, &in); break;
        case OPC_DOUBLE_GT: handled = vm_exec_compare_double_gt(ctx, &in); break;
        case OPC_DOUBLE_LT: handled = vm_exec_compare_double_lt(ctx, &in); break;

        /* ===== 控制流 ===== */
        case OPC_JMP: handled = vm_exec_control_jmp(ctx, &in); break;
        case OPC_JMP_IF_TRUE: handled = vm_exec_control_jmp_if(ctx, &in); break;
        case OPC_JMP_IF_FALSE: handled = vm_exec_control_jmp_if_false(ctx, &in); break;

        /* ===== 函数调用 ===== */
        case OPC_CALL: handled = vm_exec_call(ctx, &in); break;
        case OPC_BUILTIN: handled = vm_exec_builtin(ctx, &in); break;

        /* ===== 打印 ===== */
        case OPC_PRINT: {
            Value* stk = (Value*)g_stack_mgr->stacks[STACK_VALUE];
            int sp = --g_stack_mgr->sp[STACK_VALUE];
            lumyr_print(stk[sp]);
            handled = 1;
            break;
        }

        /* ===== 返回 ===== */
        case OPC_RETURN: {
            if (g_stack_mgr->sp[STACK_VALUE] > 0) {
                int sp = --g_stack_mgr->sp[STACK_VALUE];
                result = ((Value*)g_stack_mgr->stacks[STACK_VALUE])[sp];
            }
            goto done;
        }

        default:
            fprintf(stderr, "VM: unknown opcode %d at pc %d\n", (int)in.op, pc-1);
            goto done;
        }

        if (!handled) {
            fprintf(stderr, "VM: instruction not handled %d at pc %d\n", (int)in.op, pc-1);
        }
    }

done:
    return result;
}
