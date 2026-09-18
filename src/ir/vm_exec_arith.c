/*
 * vm_exec_arith.c - VM 算术运算指令
 * 通过栈管理器统一操作，4 核心栈设计
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "lm_value.h"
#include "lm_bigint.h"
#include "lm_decimal.h"

/* ========== 算术运算（INT64 栈专用） ========== */

/* INT64 栈加法 */
int vm_exec_arith_int64_add(VMExecCtx* ctx, Instruction* in) {
    int64_t a, b;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &b);
    stack_vm_pop(g_stack_mgr, STACK_INT64, &a);
    a += b;
    stack_vm_push(g_stack_mgr, STACK_INT64, &a);
    return 1;
}

/* INT64 栈减法 */
int vm_exec_arith_int64_sub(VMExecCtx* ctx, Instruction* in) {
    int64_t a, b;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &b);
    stack_vm_pop(g_stack_mgr, STACK_INT64, &a);
    a -= b;
    stack_vm_push(g_stack_mgr, STACK_INT64, &a);
    return 1;
}

/* INT64 栈乘法 */
int vm_exec_arith_int64_mul(VMExecCtx* ctx, Instruction* in) {
    int64_t a, b;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &b);
    stack_vm_pop(g_stack_mgr, STACK_INT64, &a);
    a *= b;
    stack_vm_push(g_stack_mgr, STACK_INT64, &a);
    return 1;
}

/* INT64 栈除法 */
int vm_exec_arith_int64_div(VMExecCtx* ctx, Instruction* in) {
    int64_t a, b;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &b);
    stack_vm_pop(g_stack_mgr, STACK_INT64, &a);
    a = (b != 0) ? a / b : 0;
    stack_vm_push(g_stack_mgr, STACK_INT64, &a);
    return 1;
}

/* INT64 栈取模 */
int vm_exec_arith_int64_mod(VMExecCtx* ctx, Instruction* in) {
    int64_t a, b;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &b);
    stack_vm_pop(g_stack_mgr, STACK_INT64, &a);
    a = (b != 0) ? a % b : 0;
    stack_vm_push(g_stack_mgr, STACK_INT64, &a);
    return 1;
}

/* ========== 算术运算（DOUBLE 栈专用） ========== */

/* DOUBLE 栈加法 */
int vm_exec_arith_double_add(VMExecCtx* ctx, Instruction* in) {
    double a, b;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &b);
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &a);
    a += b;
    stack_vm_push(g_stack_mgr, STACK_DOUBLE, &a);
    return 1;
}

/* DOUBLE 栈减法 */
int vm_exec_arith_double_sub(VMExecCtx* ctx, Instruction* in) {
    double a, b;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &b);
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &a);
    a -= b;
    stack_vm_push(g_stack_mgr, STACK_DOUBLE, &a);
    return 1;
}

/* DOUBLE 栈乘法 */
int vm_exec_arith_double_mul(VMExecCtx* ctx, Instruction* in) {
    double a, b;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &b);
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &a);
    a *= b;
    stack_vm_push(g_stack_mgr, STACK_DOUBLE, &a);
    return 1;
}

/* DOUBLE 栈除法 */
int vm_exec_arith_double_div(VMExecCtx* ctx, Instruction* in) {
    double a, b;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &b);
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &a);
    a = (b != 0.0) ? a / b : 0.0;
    stack_vm_push(g_stack_mgr, STACK_DOUBLE, &a);
    return 1;
}

/* ========== 算术运算（PTR 栈专用：字符串拼接） ========== */

/* PTR 栈加法：字符串拼接 */
int vm_exec_arith_ptr_add(VMExecCtx* ctx, Instruction* in) {
    char *b, *a;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    /* 拼接字符串：a + b */
    int len_a = strlen(a);
    int len_b = strlen(b);
    char* result = (char*)malloc(len_a + len_b + 1);
    memcpy(result, a, len_a);
    memcpy(result + len_a, b, len_b);
    result[len_a + len_b] = '\0';

    /* 结果压回 PTR 栈 */
    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* ========== 栈间转换 ========== */

/* int64 → string：从 INT64 栈弹出，转字符串，压入 PTR 栈 */
int vm_exec_conv_int64_to_string(VMExecCtx* ctx, Instruction* in) {
    int64_t val;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &val);

    char* result = (char*)malloc(32);
    snprintf(result, 32, "%lld", (long long)val);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* double → string：从 DOUBLE 栈弹出，转字符串，压入 PTR 栈 */
int vm_exec_conv_double_to_string(VMExecCtx* ctx, Instruction* in) {
    double val;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &val);

    char* result = (char*)malloc(64);
    snprintf(result, 64, "%g", val);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* ========== bigint 任意精度整数 ========== */

/* 从字符串创建 bigint */
int vm_exec_bigint_from_string(VMExecCtx* ctx, Instruction* in) {
    char* s;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &s);

    BigInt* bi = lumyr_bigint_from_string(s);
    free(s);  /* 释放原字符串 */

    stack_vm_push(g_stack_mgr, STACK_PTR, &bi);
    return 1;
}

/* bigint → string */
int vm_exec_bigint_to_string(VMExecCtx* ctx, Instruction* in) {
    BigInt* bi;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &bi);

    char* s = lumyr_bigint_to_string(bi);

    stack_vm_push(g_stack_mgr, STACK_PTR, &s);
    return 1;
}

/* bigint 加法 */
int vm_exec_bigint_add(VMExecCtx* ctx, Instruction* in) {
    BigInt *a, *b;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    BigInt* result = lumyr_bigint_add(a, b);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* bigint 减法 */
int vm_exec_bigint_sub(VMExecCtx* ctx, Instruction* in) {
    BigInt *a, *b;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    BigInt* result = lumyr_bigint_sub(a, b);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* bigint 乘法 */
int vm_exec_bigint_mul(VMExecCtx* ctx, Instruction* in) {
    BigInt *a, *b;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    BigInt* result = lumyr_bigint_mul(a, b);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* bigint 除法 */
int vm_exec_bigint_div(VMExecCtx* ctx, Instruction* in) {
    BigInt *a, *b;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    BigInt* result = lumyr_bigint_div(a, b);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* ========== decimal 高精度十进制浮点 ========== */

/* 从字符串创建 decimal */
int vm_exec_decimal_from_string(VMExecCtx* ctx, Instruction* in) {
    char* s;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &s);

    Decimal* d = lumyr_decimal_from_string(s);
    free(s);  /* 释放原字符串 */

    stack_vm_push(g_stack_mgr, STACK_PTR, &d);
    return 1;
}

/* decimal → string */
int vm_exec_decimal_to_string(VMExecCtx* ctx, Instruction* in) {
    Decimal* d;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &d);

    char* s = lumyr_decimal_to_string(d);

    stack_vm_push(g_stack_mgr, STACK_PTR, &s);
    return 1;
}

/* decimal 加法 */
int vm_exec_decimal_add(VMExecCtx* ctx, Instruction* in) {
    Decimal *a, *b;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    Decimal* result = lumyr_decimal_add(a, b);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* decimal 减法 */
int vm_exec_decimal_sub(VMExecCtx* ctx, Instruction* in) {
    Decimal *a, *b;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    Decimal* result = lumyr_decimal_sub(a, b);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* decimal 乘法 */
int vm_exec_decimal_mul(VMExecCtx* ctx, Instruction* in) {
    Decimal *a, *b;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    Decimal* result = lumyr_decimal_mul(a, b);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* decimal 除法 */
int vm_exec_decimal_div(VMExecCtx* ctx, Instruction* in) {
    Decimal *a, *b;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    Decimal* result = lumyr_decimal_div(a, b);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}
