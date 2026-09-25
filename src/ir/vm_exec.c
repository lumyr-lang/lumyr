/*
 * vm_exec.c - VM 主执行循环
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 * 指令按功能模块拆分：stack / load / var / arith / compare / control / call / io / type
 */
#include "vm_types.h"
#include "vm_exec.h"
#include "vm.h"
#include "vm_generator.h"
#include "ir_types.h"
#include "stack_manager.h"
#include "ast/stackframe.h"
#include "lumyr_value_type.h"
#include "lumyr_value.h"
#include "lm_value.h"
#include "gc_runtime.h"
#include "lm_type.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== 模块函数声明 ========== */

/* 栈操作 */
int vm_exec_stack_pop(VMExecCtx* ctx, Instruction* in);
int vm_exec_stack_dup(VMExecCtx* ctx, Instruction* in);
int vm_exec_array_lit(VMExecCtx* ctx, Instruction* in);
int vm_exec_index_get(VMExecCtx* ctx, Instruction* in);
int vm_exec_index_set(VMExecCtx* ctx, Instruction* in);
int vm_exec_typed_array_lit(VMExecCtx* ctx, Instruction* in);
int vm_exec_typed_index_set(VMExecCtx* ctx, Instruction* in);
int vm_exec_int64_to_ptr(VMExecCtx* ctx, Instruction* in);
int vm_exec_ptr_to_int64(VMExecCtx* ctx, Instruction* in);
int vm_exec_map_lit(VMExecCtx* ctx, Instruction* in);
int vm_exec_typed_bytes(VMExecCtx* ctx, Instruction* in);
int vm_exec_generic_bind(VMExecCtx* ctx, Instruction* in);

/* 常量加载 */
int vm_exec_load_int64_const(VMExecCtx* ctx, Instruction* in);
int vm_exec_load_const_idx(VMExecCtx* ctx, Instruction* in);
int vm_exec_load_double_const(VMExecCtx* ctx, Instruction* in);
int vm_exec_load_ptr_const(VMExecCtx* ctx, Instruction* in);
int vm_exec_load_string_const(VMExecCtx* ctx, Instruction* in);
int vm_exec_load_int_val(VMExecCtx* ctx, Instruction* in);
int vm_exec_load_const_val(VMExecCtx* ctx, Instruction* in);

/* 变量存取 */
int vm_exec_var_load(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_store(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_load_int64(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_store_int64(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_load_double(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_store_double(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_load_ptr(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_store_ptr(VMExecCtx* ctx, Instruction* in);
int vm_exec_var_load_global(VMExecCtx* ctx, Instruction* in);

/* 算术运算 */
int vm_exec_arith_int64_add(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_sub(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_mul(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_div(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_mod(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_band(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_bor(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_bxor(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_bnot(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_trunc(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_shl(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_shr(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_int64_pow(VMExecCtx* ctx, Instruction* in);
int vm_exec_arith_double_pow(VMExecCtx* ctx, Instruction* in);
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
int vm_exec_bitdecimal_to_string(VMExecCtx* ctx, Instruction* in);
int vm_exec_bitdecimal_from_int64(VMExecCtx* ctx, Instruction* in);
int vm_exec_bitdecimal_from_double(VMExecCtx* ctx, Instruction* in);
int vm_exec_bitdecimal_add(VMExecCtx* ctx, Instruction* in);
int vm_exec_bitdecimal_sub(VMExecCtx* ctx, Instruction* in);
int vm_exec_bitdecimal_mul(VMExecCtx* ctx, Instruction* in);
int vm_exec_bitdecimal_div(VMExecCtx* ctx, Instruction* in);

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
int vm_exec_compare_double_ge(VMExecCtx* ctx, Instruction* in);
int vm_exec_compare_double_le(VMExecCtx* ctx, Instruction* in);
int vm_exec_compare_double_ne(VMExecCtx* ctx, Instruction* in);

/* 控制流 */
int vm_exec_control_jmp(VMExecCtx* ctx, Instruction* in);
int vm_exec_control_jmp_if_true(VMExecCtx* ctx, Instruction* in);
int vm_exec_control_jmp_if_false(VMExecCtx* ctx, Instruction* in);

/* 函数调用 */
int vm_exec_call(VMExecCtx* ctx, Instruction* in);
int vm_exec_return(VMExecCtx* ctx, Instruction* in);
int vm_exec_getfunc(VMExecCtx* ctx, Instruction* in);
int vm_exec_callv(VMExecCtx* ctx, Instruction* in);
int vm_exec_mkclosure(VMExecCtx* ctx, Instruction* in);

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
int vm_exec_box_int64(VMExecCtx* ctx, Instruction* in);
int vm_exec_box_double(VMExecCtx* ctx, Instruction* in);
int vm_exec_box_ptr(VMExecCtx* ctx, Instruction* in);
int vm_exec_assert_nonnull(VMExecCtx* ctx, Instruction* in);
int vm_exec_unbox_int64(VMExecCtx* ctx, Instruction* in);
int vm_exec_unbox_double(VMExecCtx* ctx, Instruction* in);
int vm_exec_unbox_ptr(VMExecCtx* ctx, Instruction* in);
int vm_exec_cast_string(VMExecCtx* ctx, Instruction* in);
int vm_exec_str_to_int64(VMExecCtx* ctx, Instruction* in);
int vm_exec_str_to_double(VMExecCtx* ctx, Instruction* in);

/* 异常处理（vm_except.c） */
int vm_exec_try(VMExecCtx* ctx, Instruction* in);
int vm_exec_endtry(VMExecCtx* ctx, Instruction* in);
int vm_exec_get_err(VMExecCtx* ctx, Instruction* in);
int vm_exec_throw(VMExecCtx* ctx, Instruction* in);
int vm_exec_finish(VMExecCtx* ctx, Instruction* in);
int vm_exec_pend_return(VMExecCtx* ctx, Instruction* in);
int vm_exec_fin_push(VMExecCtx* ctx, Instruction* in);
int vm_exec_catch_match(VMExecCtx* ctx, Instruction* in);

/* 通用 Value 运算（动态兜底） */
int vm_exec_vadd(VMExecCtx* ctx, Instruction* in);
int vm_exec_vsub(VMExecCtx* ctx, Instruction* in);
int vm_exec_vmul(VMExecCtx* ctx, Instruction* in);
int vm_exec_vdiv(VMExecCtx* ctx, Instruction* in);
int vm_exec_vmod(VMExecCtx* ctx, Instruction* in);
int vm_exec_vpow(VMExecCtx* ctx, Instruction* in);
int vm_exec_vneg(VMExecCtx* ctx, Instruction* in);
int vm_exec_vgt(VMExecCtx* ctx, Instruction* in);
int vm_exec_vlt(VMExecCtx* ctx, Instruction* in);
int vm_exec_vge(VMExecCtx* ctx, Instruction* in);
int vm_exec_vle(VMExecCtx* ctx, Instruction* in);
int vm_exec_veq(VMExecCtx* ctx, Instruction* in);
int vm_exec_vne(VMExecCtx* ctx, Instruction* in);
int vm_exec_vband(VMExecCtx* ctx, Instruction* in);
int vm_exec_vbor(VMExecCtx* ctx, Instruction* in);
int vm_exec_vbxor(VMExecCtx* ctx, Instruction* in);
int vm_exec_vbnot(VMExecCtx* ctx, Instruction* in);
int vm_exec_vshl(VMExecCtx* ctx, Instruction* in);
int vm_exec_vshr(VMExecCtx* ctx, Instruction* in);
int vm_exec_control_jmp_if_true_value(VMExecCtx* ctx, Instruction* in);
int vm_exec_control_jmp_if_false_value(VMExecCtx* ctx, Instruction* in);

/* struct/class 字段访问 */
int vm_exec_load_field(VMExecCtx* ctx, Instruction* in);
int vm_exec_store_field(VMExecCtx* ctx, Instruction* in);
int vm_exec_class_new(VMExecCtx* ctx, Instruction* in);
int vm_exec_call_method(VMExecCtx* ctx, Instruction* in);

/* ========== 返回值独立化：堆字符串拷贝为独立副本（帧销毁后仍有效）；SSO/数值按值拷贝即可 ========== */
Value ret_value_detach(Value v) {
    if (v.type == VAL_STRING && !v.str_inline && v.v.s) {
        char* p = strdup(v.v.s);
        if (p) v.v.s = p;
    }
    return v;
}

/* ========== 可重入执行循环：运行 ctx->fn 直到 RETURN/RETURN_NIL 或自然结束 ==========
   返回值写入 *ret_out。每层函数调用（vm_exec_call）都会新建帧并再次进入本循环，
   故 RETURN 只结束当前层，而非直接终止整个程序。 */
int vm_exec_loop(VMExecCtx* ctx, RetSlot* ret) {
    Instruction* code = ctx->code;
    ret->et = EXPR_TYPE_NONE;
    ret->v  = val_none();
    ret->i  = 0;
    ret->d  = 0.0;
    ret->p  = NULL;

    int total_instr = 0;
    /* 访问控制上下文：执行某 class 的字节码方法期间，g_current_class 标记属主类，
     * 使同类内动态字段访问（非 self 快路径，如静态方法里 other.privateField）
     * 能通过访问检查。可重入：进入保存、每个出口恢复调用者类。 */
    const char* prev_class = lumyr_get_current_class();
    if(ctx->fn->class_name) lumyr_set_current_class(ctx->fn->class_name);
    /* 程序计数器统一使用 ctx->pc：控制流指令（JMP/JMP_IF_*）直接改写它。
       取指后自增；若指令是跳转，会在执行时覆盖为目标地址。 */
    while (ctx->pc < ctx->fn->code_len) {
        Instruction in = code[ctx->pc++];
        total_instr++;
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
        case OPC_ARRAY_LIT: handled = vm_exec_array_lit(ctx, &in); break;
        case OPC_INDEX_GET: handled = vm_exec_index_get(ctx, &in); break;
        case OPC_INDEX_SET: handled = vm_exec_index_set(ctx, &in); break;
        case OPC_INT64_ARRAY_LIT:
        case OPC_DOUBLE_ARRAY_LIT:
        case OPC_PTR_ARRAY_LIT: handled = vm_exec_typed_array_lit(ctx, &in); break;
        case OPC_TYPED_INDEX_SET: handled = vm_exec_typed_index_set(ctx, &in); break;
        case OPC_INT64_TO_PTR: handled = vm_exec_int64_to_ptr(ctx, &in); break;
        case OPC_PTR_TO_INT64: handled = vm_exec_ptr_to_int64(ctx, &in); break;
        case OPC_MAP_LIT: handled = vm_exec_map_lit(ctx, &in); break;
    case OPC_TYPED_BYTES: handled = vm_exec_typed_bytes(ctx, &in); break;

        /* ===== 常量加载 ===== */
        case OPC_PUSH_INT64_CONST: handled = vm_exec_load_int64_const(ctx, &in); break;
        case OPC_PUSH_CONST_IDX: handled = vm_exec_load_const_idx(ctx, &in); break;
        case OPC_PUSH_DOUBLE_CONST: handled = vm_exec_load_double_const(ctx, &in); break;
        case OPC_PUSH_PTR_CONST: handled = vm_exec_load_ptr_const(ctx, &in); break;
        case OPC_LOAD_STRING_CONST: handled = vm_exec_load_string_const(ctx, &in); break;
        case OPC_PUSH_INT_VAL:   handled = vm_exec_load_int_val(ctx, &in); break;
        case OPC_PUSH_CONST_VAL: handled = vm_exec_load_const_val(ctx, &in); break;

        /* ===== 变量存取 ===== */
        case OPC_LOAD_VAR: handled = vm_exec_var_load(ctx, &in); break;
        case OPC_STORE_VAR: handled = vm_exec_var_store(ctx, &in); break;
        case OPC_LOAD_INT64_VAR: handled = vm_exec_var_load_int64(ctx, &in); break;
        case OPC_STORE_INT64_VAR: handled = vm_exec_var_store_int64(ctx, &in); break;
        case OPC_LOAD_DOUBLE_VAR: handled = vm_exec_var_load_double(ctx, &in); break;
        case OPC_STORE_DOUBLE_VAR: handled = vm_exec_var_store_double(ctx, &in); break;
        case OPC_LOAD_PTR_VAR: handled = vm_exec_var_load_ptr(ctx, &in); break;
        case OPC_STORE_PTR_VAR: handled = vm_exec_var_store_ptr(ctx, &in); break;
        case OPC_LOAD_GLOBAL: handled = vm_exec_var_load_global(ctx, &in); break;

        /* ===== 算术运算（INT64 栈） ===== */
        case OPC_INT64_ADD: handled = vm_exec_arith_int64_add(ctx, &in); break;
        case OPC_INT64_SUB: handled = vm_exec_arith_int64_sub(ctx, &in); break;
        case OPC_INT64_MUL: handled = vm_exec_arith_int64_mul(ctx, &in); break;
        case OPC_INT64_DIV: handled = vm_exec_arith_int64_div(ctx, &in); break;
        case OPC_INT64_MOD: handled = vm_exec_arith_int64_mod(ctx, &in); break;

        /* ===== 位运算（INT64 栈） ===== */
        case OPC_INT64_BAND: handled = vm_exec_arith_int64_band(ctx, &in); break;
        case OPC_INT64_BOR:  handled = vm_exec_arith_int64_bor(ctx, &in); break;
        case OPC_INT64_BXOR: handled = vm_exec_arith_int64_bxor(ctx, &in); break;
        case OPC_INT64_BNOT: handled = vm_exec_arith_int64_bnot(ctx, &in); break;
        case OPC_INT64_TRUNC: handled = vm_exec_arith_int64_trunc(ctx, &in); break;
        case OPC_INT64_SHL:  handled = vm_exec_arith_int64_shl(ctx, &in); break;
        case OPC_INT64_SHR:  handled = vm_exec_arith_int64_shr(ctx, &in); break;
        case OPC_INT64_POW:  handled = vm_exec_arith_int64_pow(ctx, &in); break;

        /* ===== 类型转换 ===== */
        case OPC_INT64_TO_DOUBLE: handled = vm_exec_type_int64_to_double(ctx, &in); break;
        case OPC_DOUBLE_TO_INT64: handled = vm_exec_type_double_to_int64(ctx, &in); break;
        case OPC_NEG: handled = vm_exec_type_neg(ctx, &in); break;
        case OPC_BOX_INT64:  handled = vm_exec_box_int64(ctx, &in); break;
        case OPC_BOX_DOUBLE: handled = vm_exec_box_double(ctx, &in); break;
        case OPC_BOX_PTR:    handled = vm_exec_box_ptr(ctx, &in); break;
        case OPC_ASSERT_NONNULL: handled = vm_exec_assert_nonnull(ctx, &in); break;
        case OPC_PUSH_NONE: {
            Value v; v.type = VAL_NONE; v.v.i = 0;
            stack_vm_push(g_stack_mgr, STACK_VALUE, &v);
            handled = 1; break;
        }
        case OPC_UNBOX_INT64:  handled = vm_exec_unbox_int64(ctx, &in); break;
        case OPC_UNBOX_DOUBLE: handled = vm_exec_unbox_double(ctx, &in); break;
        case OPC_UNBOX_PTR:    handled = vm_exec_unbox_ptr(ctx, &in); break;
        case OPC_CAST_STRING:  handled = vm_exec_cast_string(ctx, &in); break;
        case OPC_STR_TO_INT64:  handled = vm_exec_str_to_int64(ctx, &in); break;
        case OPC_STR_TO_DOUBLE: handled = vm_exec_str_to_double(ctx, &in); break;

        /* ===== 异常处理 ===== */
        case OPC_TRY:         handled = vm_exec_try(ctx, &in); break;
        case OPC_ENDTRY:      handled = vm_exec_endtry(ctx, &in); break;
        case OPC_GET_ERR:     handled = vm_exec_get_err(ctx, &in); break;
        case OPC_THROW:       handled = vm_exec_throw(ctx, &in); break;
        case OPC_FINISH:      handled = vm_exec_finish(ctx, &in); break;
        case OPC_PEND_RETURN: handled = vm_exec_pend_return(ctx, &in); break;
        case OPC_FIN_PUSH:    handled = vm_exec_fin_push(ctx, &in); break;
        case OPC_CATCH_MATCH: handled = vm_exec_catch_match(ctx, &in); break;

        /* ===== 算术运算（DOUBLE 栈） ===== */
        case OPC_DOUBLE_ADD: handled = vm_exec_arith_double_add(ctx, &in); break;
        case OPC_DOUBLE_SUB: handled = vm_exec_arith_double_sub(ctx, &in); break;
        case OPC_DOUBLE_MUL: handled = vm_exec_arith_double_mul(ctx, &in); break;
        case OPC_DOUBLE_DIV: handled = vm_exec_arith_double_div(ctx, &in); break;
        case OPC_DOUBLE_POW: handled = vm_exec_arith_double_pow(ctx, &in); break;

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
        case OPC_BITDECIMAL_TO_STRING: handled = vm_exec_bitdecimal_to_string(ctx, &in); break;
        case OPC_BITDECIMAL_FROM_INT64: handled = vm_exec_bitdecimal_from_int64(ctx, &in); break;
        case OPC_BITDECIMAL_FROM_DOUBLE: handled = vm_exec_bitdecimal_from_double(ctx, &in); break;
        case OPC_BITDECIMAL_ADD: handled = vm_exec_bitdecimal_add(ctx, &in); break;
        case OPC_BITDECIMAL_SUB: handled = vm_exec_bitdecimal_sub(ctx, &in); break;
        case OPC_BITDECIMAL_MUL: handled = vm_exec_bitdecimal_mul(ctx, &in); break;
        case OPC_BITDECIMAL_DIV: handled = vm_exec_bitdecimal_div(ctx, &in); break;

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
        case OPC_DOUBLE_GE: handled = vm_exec_compare_double_ge(ctx, &in); break;
        case OPC_DOUBLE_LE: handled = vm_exec_compare_double_le(ctx, &in); break;
        case OPC_DOUBLE_NE: handled = vm_exec_compare_double_ne(ctx, &in); break;

        /* ===== 控制流 ===== */
        case OPC_JMP: handled = vm_exec_control_jmp(ctx, &in); break;
        case OPC_JMP_IF_TRUE: handled = vm_exec_control_jmp_if_true(ctx, &in); break;
        case OPC_JMP_IF_FALSE: handled = vm_exec_control_jmp_if_false(ctx, &in); break;
        case OPC_JMP_IF_TRUE_V: handled = vm_exec_control_jmp_if_true_value(ctx, &in); break;
        case OPC_JMP_IF_FALSE_V: handled = vm_exec_control_jmp_if_false_value(ctx, &in); break;

        /* ===== 通用 Value 运算（动态兜底） ===== */
        case OPC_VADD: handled = vm_exec_vadd(ctx, &in); break;
        case OPC_VSUB: handled = vm_exec_vsub(ctx, &in); break;
        case OPC_VMUL: handled = vm_exec_vmul(ctx, &in); break;
        case OPC_VDIV: handled = vm_exec_vdiv(ctx, &in); break;
        case OPC_VMOD: handled = vm_exec_vmod(ctx, &in); break;
        case OPC_VPOW: handled = vm_exec_vpow(ctx, &in); break;
        case OPC_VNEG: handled = vm_exec_vneg(ctx, &in); break;
        case OPC_VGT: handled = vm_exec_vgt(ctx, &in); break;
        case OPC_VLT: handled = vm_exec_vlt(ctx, &in); break;
        case OPC_VGE: handled = vm_exec_vge(ctx, &in); break;
        case OPC_VLE: handled = vm_exec_vle(ctx, &in); break;
        case OPC_VEQ: handled = vm_exec_veq(ctx, &in); break;
        case OPC_VNE: handled = vm_exec_vne(ctx, &in); break;
        case OPC_VBAND: handled = vm_exec_vband(ctx, &in); break;
        case OPC_VBOR:  handled = vm_exec_vbor(ctx, &in); break;
        case OPC_VBXOR: handled = vm_exec_vbxor(ctx, &in); break;
        case OPC_VBNOT: handled = vm_exec_vbnot(ctx, &in); break;
        case OPC_VSHL:  handled = vm_exec_vshl(ctx, &in); break;
        case OPC_VSHR:  handled = vm_exec_vshr(ctx, &in); break;

        /* ===== 函数调用 ===== */
        case OPC_CALL: handled = vm_exec_call(ctx, &in); break;
        case OPC_BUILTIN: handled = vm_exec_builtin(ctx, &in); break;
        case OPC_CALL_BUILTIN_METHOD: handled = vm_exec_builtin_method(ctx, &in); break;
        case OPC_GETFUNC: handled = vm_exec_getfunc(ctx, &in); break;
        case OPC_CALLV: handled = vm_exec_callv(ctx, &in); break;
        case OPC_MKCLOSURE: handled = vm_exec_mkclosure(ctx, &in); break;

        /* ===== struct/class ===== */
        case OPC_LOAD_FIELD: handled = vm_exec_load_field(ctx, &in); break;
        case OPC_STORE_FIELD: handled = vm_exec_store_field(ctx, &in); break;
        case OPC_CLASS_NEW: handled = vm_exec_class_new(ctx, &in); break;
        case OPC_CALL_METHOD: handled = vm_exec_call_method(ctx, &in); break;
        case OPC_CALL_METHODV: handled = vm_exec_call_method_dyn(ctx, &in); break;
        case OPC_GENERIC_BIND: handled = vm_exec_generic_bind(ctx, &in); break;

        /* ===== 打印 ===== */
        case OPC_PRINT: handled = vm_exec_io_print(ctx, &in); break;
        case OPC_PRINT_INT64: handled = vm_exec_io_print_int64(ctx, &in); break;
        case OPC_PRINT_DOUBLE: handled = vm_exec_io_print_double(ctx, &in); break;
        case OPC_PRINT_PTR: handled = vm_exec_io_print_ptr(ctx, &in); break;
        case OPC_PRINT_BIGINT: handled = vm_exec_io_print_bigint(ctx, &in); break;
        case OPC_PRINT_DECIMAL: handled = vm_exec_io_print_decimal(ctx, &in); break;

        /* ===== 返回：只结束当前层；按 ExprType 从对应栈弹原始值入返回槽 ===== */
        case OPC_RETURN: {
            ExprType et = (ExprType)in.a;
            ret->et = (int)et;
            switch (et) {
            case EXPR_TYPE_INT:
                ret->i = POP_INT64();
                break;
            case EXPR_TYPE_DOUBLE:
                ret->d = POP_DOUBLE();
                break;
            case EXPR_TYPE_PTR: {
                void* p = POP_PTR();
                /* b=CastKind：字符串深拷贝，使其独立于即将销毁的帧 */
                if ((CastKind)in.b == CAST_STRING && p) {
                    char* cp = strdup((char*)p);
                    if (cp) p = cp;
                }
                ret->p = p;
                break;
            }
            default:
                stack_vm_pop(g_stack_mgr, STACK_VALUE, &ret->v);
                ret->v = ret_value_detach(ret->v);
                break;
            }
            return 0;
        }

        case OPC_RETURN_NIL:
            ret->et = EXPR_TYPE_NONE;
            ret->v  = val_none();
            return 0;

        /* ===== 生成器 yield：弹 yield 值写入线程局部变量，结束 vm_exec_loop =====
         * a=1 表示有 yield 值（从 VALUE 栈弹）；a=0 表示无值 yield（值为 NONE）。
         * s_current_gen 必须由 generator_resume 在进入前设置；yield 后由其负责
         * 保存栈状态/pc，恢复调用方 sp。 */
        case OPC_YIELD: {
            if(in.a) {
                Value yv;
                stack_vm_pop(g_stack_mgr, STACK_VALUE, &yv);
                s_gen_yield_result = ret_value_detach(yv);
            } else {
                s_gen_yield_result = val_none();
            }
            s_gen_yielded = 1;
            if(s_current_gen) {
                s_current_gen->pc = ctx->pc;  /* 下次 resume 从 yield 下一条指令开始 */
            }
            return 0;
        }

        default:
            /* 指令流损坏（未知操作码）：不可恢复，立即中止（无兜底） */
            fprintf(stderr, "VM: unknown opcode %d at pc %d\n", (int)in.op, ctx->pc-1);
            exit(1);
        }

        if (!handled) {
            /* 执行函数返回 0 = VM 层硬错误（错误详情已由执行函数打印）。
             * 无兜底：不可恢复错误必须中止进程并给出非零退出码，
             * 禁止打印后继续执行（否则 CI/脚本会误判成功）。 */
            fprintf(stderr, "VM: instruction not handled %d at pc %d\n", (int)in.op, ctx->pc-1);
            exit(1);
        }

        /* try 内 return 且所有 finally 已执行完：用挂起值结束当前帧 */
        if (vm_except_take_pending_return(ret))
            return VM_LOOP_NORMAL;

        /* 异常跨帧展开：捕获帧由 check 内部设 current_error 并重定位 pc；
           非捕获帧结束当前层，向调用者传播 VM_LOOP_UNWIND。 */
        if (vm_except_check_unwind(ctx) < 0) {
            lumyr_set_current_class(prev_class);
            return VM_LOOP_UNWIND;
        }
    }

    /* 函数自然走到末尾（无显式 return）：返回 nil */
    ret->et = EXPR_TYPE_NONE;
    ret->v  = val_none();
    return 0;
}

/* ========== 顶层入口：初始化栈管理器并进入执行循环 ========== */
Value vm_execute(VMExecCtx* ctx) {
    if (!g_stack_mgr) {
        stack_global_init(256);
    }
    ctx->pc = 0;
    RetSlot ret;
    vm_exec_loop(ctx, &ret);
    /* 顶层 main 一般无 typed 返回值；有则退化为 nil（主流程不消费） */
    return ret.et == EXPR_TYPE_NONE ? ret.v : val_none();
}
