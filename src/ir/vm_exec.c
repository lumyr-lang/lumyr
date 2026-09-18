/*
 * vm_exec.c - VM 主执行循环
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 * 指令按功能模块拆分：stack / load / var / arith / compare / control / call / io / type
 */
#include "vm_types.h"
#include "vm.h"
#include "stack_manager.h"
#include "ast/stackframe.h"
#include "lumyr_value_type.h"
#include "lumyr_value.h"
#include "lm_value.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== 模块函数声明 ========== */

/* 栈操作 */
int vm_exec_stack_pop(VMExecCtx* ctx, Instruction* in);
int vm_exec_stack_dup(VMExecCtx* ctx, Instruction* in);

/* 常量加载 */
int vm_exec_load_int64_const(VMExecCtx* ctx, Instruction* in);
int vm_exec_load_const_idx(VMExecCtx* ctx, Instruction* in);
int vm_exec_load_double_const(VMExecCtx* ctx, Instruction* in);
int vm_exec_load_ptr_const(VMExecCtx* ctx, Instruction* in);
int vm_exec_load_string_const(VMExecCtx* ctx, Instruction* in);

/* 变量存取 */
int vm_exec_var_load(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_store(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_load_int64(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_store_int64(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_load_double(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_store_double(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_load_ptr(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_store_ptr(VMExecCtx* ctx, Instruction* in);

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
int vm_exec_arith_ptr_add(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_ptr_mul(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_ptr_div(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_ptr_sub(VMExecCtx* ctx, Instruction* in);
int vm_exec_conv_int64_to_string(VMExecCtx* ctx, Instruction* in);
int vm_exec_conv_double_to_string(VMExecCtx* ctx, Instruction* in);

/* bigint 任意精度整数 */
int vm_exec_bigint_from_string(VMExecCtx* ctx, Instruction* in);
int vm_exec_bigint_to_string(VMExecCtx* ctx, Instruction* in);
int vm_exec_bigint_add(VMExecCtx* ctx, Instruction* in);
int vm_exec_bigint_sub(VMExecCtx* ctx, Instruction* in);
int vm_exec_bigint_mul(VMExecCtx* ctx, Instruction* in);
int vm_exec_bigint_div(VMExecCtx* ctx, Instruction* in);

/* decimal 高精度十进制浮点 */
int vm_exec_decimal_from_string(VMExecCtx* ctx, Instruction* in);
int vm_exec_decimal_to_string(VMExecCtx* ctx, Instruction* in);
int vm_exec_decimal_add(VMExecCtx* ctx, Instruction* in);
int vm_exec_decimal_sub(VMExecCtx* ctx, Instruction* in);
int vm_exec_decimal_mul(VMExecCtx* ctx, Instruction* in);
int vm_exec_decimal_div(VMExecCtx* ctx, Instruction* in);

/* bitdecimal 高精度十进制浮点（基于 GMP mpf_t） */
int vm_exec_bitdecimal_from_string(VMExecCtx* ctx, Instruction* in);

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
int vm_exec_control_jmp_if_true(VMExecCtx* ctx, Instruction* in);
int vm_exec_control_jmp_if_false(VMExecCtx* ctx, Instruction* in);

/* 函数调用 */
int vm_exec_call(VMExecCtx* ctx, Instruction* in);
int vm_exec_return(VMExecCtx* ctx, Instruction* in);
int vm_exec_builtin(VMExecCtx* ctx, Instruction* in);

/* 输入输出 */
int vm_exec_io_print(VMExecCtx* ctx, Instruction* in);
int vm_exec_io_print_int64(VMExecCtx* ctx, Instruction* in);
int vm_exec_io_print_double(VMExecCtx* ctx, Instruction* in);
int vm_exec_io_print_ptr(VMExecCtx* ctx, Instruction* in);
int vm_exec_io_print_bigint(VMExecCtx* ctx, Instruction* in);
int vm_exec_io_print_decimal(VMExecCtx* ctx, Instruction* in);

/* 类型转换 */
int vm_exec_type_int64_to_double(VMExecCtx* ctx, Instruction* in);
int vm_exec_type_double_to_int64(VMExecCtx* ctx, Instruction* in);
int vm_exec_type_neg(VMExecCtx* ctx, Instruction* in);

/* ========== 主执行循环 ========== */
Value vm_execute(VMExecCtx* ctx) {
    Instruction* code = ctx->code;
    int pc = 0;
    Value result = val_none();


    /* 初始化全局栈管理器 */
    if (!g_stack_mgr) {
        stack_global_init(256);
    }

    int total_instr = 0;
    while (pc < ctx->fn->code_len) {
        Instruction in = code[pc++];
        total_instr++;
        if (total_instr <= 50) {
            fprintf(stderr, "DEBUG: pc=%d, op=%d, total=%d\n", pc-1, (int)in.op, total_instr);
        }
        if (pc <= 5) {
        }
        int handled = 0;
        if((int)in.op >= 115 && (int)in.op <= 120) {
        }

        switch (in.op) {
        if(in.op == OPC_PRINT_BIGINT || in.op == OPC_PRINT_PTR) {
        }
        case OPC_NOP:
            handled = 1;
            break;

        /* ===== 栈操作 ===== */
        case OPC_POP: handled = vm_exec_stack_pop(ctx, &in); break;
        case OPC_DUP: handled = vm_exec_stack_dup(ctx, &in); break;

        /* ===== 常量加载 ===== */
        case OPC_PUSH_INT64_CONST: handled = vm_exec_load_int64_const(ctx, &in); break;
        case OPC_PUSH_CONST_IDX: handled = vm_exec_load_const_idx(ctx, &in); break;
        case OPC_PUSH_DOUBLE_CONST: handled = vm_exec_load_double_const(ctx, &in); break;
        case OPC_PUSH_PTR_CONST: handled = vm_exec_load_ptr_const(ctx, &in); break;
        case OPC_LOAD_STRING_CONST: handled = vm_exec_load_string_const(ctx, &in); break;

        /* ===== 变量存取 ===== */
        case OPC_LOAD_VAR: handled = vm_exec_var_load(ctx, &in); break;
        case OPC_STORE_VAR: handled = vm_exec_var_store(ctx, &in); break;
        case OPC_LOAD_INT64_VAR: handled = vm_exec_var_load_int64(ctx, &in); break;
        case OPC_STORE_INT64_VAR: handled = vm_exec_var_store_int64(ctx, &in); break;
        case OPC_LOAD_DOUBLE_VAR: handled = vm_exec_var_load_double(ctx, &in); break;
        case OPC_STORE_DOUBLE_VAR: handled = vm_exec_var_store_double(ctx, &in); break;
        case OPC_LOAD_PTR_VAR: handled = vm_exec_var_load_ptr(ctx, &in); break;
        case OPC_STORE_PTR_VAR: handled = vm_exec_var_store_ptr(ctx, &in); break;

        /* ===== 算术运算（INT64 栈） ===== */
        case OPC_INT64_ADD: handled = vm_exec_arith_int64_add(ctx, &in); break;
        case OPC_INT64_SUB: handled = vm_exec_arith_int64_sub(ctx, &in); break;
        case OPC_INT64_MUL: handled = vm_exec_arith_int64_mul(ctx, &in); break;
        case OPC_INT64_DIV: handled = vm_exec_arith_int64_div(ctx, &in); break;
        case OPC_INT64_MOD: handled = vm_exec_arith_int64_mod(ctx, &in); break;

        /* ===== 类型转换 ===== */
        case OPC_INT64_TO_DOUBLE: handled = vm_exec_type_int64_to_double(ctx, &in); break;
        case OPC_DOUBLE_TO_INT64: handled = vm_exec_type_double_to_int64(ctx, &in); break;
        case OPC_NEG: handled = vm_exec_type_neg(ctx, &in); break;

        /* ===== 算术运算（DOUBLE 栈） ===== */
        case OPC_DOUBLE_ADD: handled = vm_exec_arith_double_add(ctx, &in); break;
        case OPC_DOUBLE_SUB: handled = vm_exec_arith_double_sub(ctx, &in); break;
        case OPC_DOUBLE_MUL: handled = vm_exec_arith_double_mul(ctx, &in); break;
        case OPC_DOUBLE_DIV: handled = vm_exec_arith_double_div(ctx, &in); break;

        /* ===== 算术运算（PTR 栈：字符串拼接等） ===== */
        case OPC_PTR_ADD: handled = vm_exec_arith_ptr_add(ctx, &in); break;
        case OPC_PTR_MUL: handled = vm_exec_arith_ptr_mul(ctx, &in); break;
        case OPC_PTR_DIV: handled = vm_exec_arith_ptr_div(ctx, &in); break;
        case OPC_PTR_SUB: handled = vm_exec_arith_ptr_sub(ctx, &in); break;

        /* ===== 栈间转换 ===== */
        case OPC_INT64_TO_STRING: handled = vm_exec_conv_int64_to_string(ctx, &in); break;
        case OPC_DOUBLE_TO_STRING: handled = vm_exec_conv_double_to_string(ctx, &in); break;

        /* ===== bigint 任意精度整数 ===== */
        case OPC_BIGINT_FROM_STRING: handled = vm_exec_bigint_from_string(ctx, &in); break;
        case OPC_BIGINT_ADD: handled = vm_exec_bigint_add(ctx, &in); break;
        case OPC_BIGINT_SUB: handled = vm_exec_bigint_sub(ctx, &in); break;
        case OPC_BIGINT_MUL: handled = vm_exec_bigint_mul(ctx, &in); break;
        case OPC_BIGINT_DIV: handled = vm_exec_bigint_div(ctx, &in); break;
        case OPC_BIGINT_TO_STRING: handled = vm_exec_bigint_to_string(ctx, &in); break;

        /* ===== decimal 高精度十进制浮点 ===== */
        case OPC_DECIMAL_FROM_STRING: handled = vm_exec_decimal_from_string(ctx, &in); break;
        case OPC_DECIMAL_ADD: handled = vm_exec_decimal_add(ctx, &in); break;
        case OPC_DECIMAL_SUB: handled = vm_exec_decimal_sub(ctx, &in); break;
        case OPC_DECIMAL_MUL: handled = vm_exec_decimal_mul(ctx, &in); break;
        case OPC_DECIMAL_DIV: handled = vm_exec_decimal_div(ctx, &in); break;
        case OPC_DECIMAL_TO_STRING: handled = vm_exec_decimal_to_string(ctx, &in); break;

        /* ===== bitdecimal 高精度十进制浮点（基于 GMP mpf_t） ===== */
        case OPC_BITDECIMAL_FROM_STRING: handled = vm_exec_bitdecimal_from_string(ctx, &in); break;

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
        case OPC_JMP_IF_TRUE: handled = vm_exec_control_jmp_if_true(ctx, &in); break;
        case OPC_JMP_IF_FALSE: handled = vm_exec_control_jmp_if_false(ctx, &in); break;

        /* ===== 函数调用 ===== */
        case OPC_CALL: handled = vm_exec_call(ctx, &in); break;
        case OPC_BUILTIN: handled = vm_exec_builtin(ctx, &in); break;

        /* ===== 打印 ===== */
        case OPC_PRINT: handled = vm_exec_io_print(ctx, &in); break;
        case OPC_PRINT_INT64: handled = vm_exec_io_print_int64(ctx, &in); break;
        case OPC_PRINT_DOUBLE: handled = vm_exec_io_print_double(ctx, &in); break;
        case OPC_PRINT_PTR: handled = vm_exec_io_print_ptr(ctx, &in); break;
        case OPC_PRINT_BIGINT: handled = vm_exec_io_print_bigint(ctx, &in); break;
        case OPC_PRINT_DECIMAL: handled = vm_exec_io_print_decimal(ctx, &in); break;

        /* ===== 返回 ===== */
        case OPC_RETURN: {
            stack_vm_pop(g_stack_mgr, STACK_VALUE, &result);
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
