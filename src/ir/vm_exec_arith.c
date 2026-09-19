/*
 * vm_exec_arith.c - VM 算术运算指令
 * 通过栈管理器统一操作，4 核心栈设计
 */
#include "vm_types.h"
#include "stack_manager.h"
#include "lm_value.h"
#include "lm_bigint.h"
#include "lm_decimal.h"
#include "lm_bitdecimal.h"
#include <ctype.h>

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
    snprintf(result, 64, "%f", val);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* ========== bigint 任意精度整数 ========== */

/* 从字符串创建 bigint */
int vm_exec_bigint_from_string(VMExecCtx* ctx, Instruction* in) {
    char* s;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &s);

    BigInt* bi = lumyr_bigint_from_string(s);
    /* 不要释放 s，因为 s 可能是常量池中的字符串，不应该被释放 */
    /* 如果 s 是动态分配的，由 GC 管理 */

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
    /* 不要释放 s，因为 s 可能是常量池中的字符串，不应该被释放 */
    /* 如果 s 是动态分配的，由 GC 管理 */

    stack_vm_push(g_stack_mgr, STACK_PTR, &d);
    return 1;
}

/* 从字符串创建 bitdecimal（基于 GMP mpf_t） */
int vm_exec_bitdecimal_from_string(VMExecCtx* ctx, Instruction* in) {
    char* s;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &s);

    BitDecimal* bd = lumyr_bitdecimal_from_string(s);
    /* 不要释放 s，因为 s 可能是常量池中的字符串，不应该被释放 */
    /* 如果 s 是动态分配的，由 GC 管理 */

    stack_vm_push(g_stack_mgr, STACK_PTR, &bd);
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


/* ========== 字符串运算 ========== */

/* 字符串乘法："abc" * 3 = "abcabcabc" */
int vm_exec_arith_ptr_mul(VMExecCtx* ctx, Instruction* in) {
    char *b, *a;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    /* 判断哪个是字符串，哪个是数字 */
    char* str;
    int n;

    /* 判断 a 是不是数字（允许小数点和负号） */
    int a_is_num = 1;
    for(char* p = a; *p; p++) {
        if(!isdigit(*p) && *p != '-' && *p != '.') {
            a_is_num = 0;
            break;
        }
    }

    /* 判断 b 是不是数字（允许小数点和负号） */
    int b_is_num = 1;
    for(char* p = b; *p; p++) {
        if(!isdigit(*p) && *p != '-' && *p != '.') {
            b_is_num = 0;
            break;
        }
    }

    if(a_is_num && !b_is_num) {
        /* 左操作数是数字，右操作数是字符串：3 * "abc" = "abcabcabc" */
        str = b;
        n = atoi(a);
    } else if(!a_is_num && b_is_num) {
        /* 左操作数是字符串，右操作数是数字："abc" * 3 = "abcabcabc" */
        str = a;
        n = atoi(b);
    } else {
        /* 两个都是字符串或两个都是数字：按左操作数是字符串处理 */
        str = a;
        n = atoi(b);
    }

    if(n <= 0) {
        char* result = (char*)malloc(1);
        result[0] = '\0';
        stack_vm_push(g_stack_mgr, STACK_PTR, &result);
        return 1;
    }

    int len_str = strlen(str);
    
    /* 溢出检查：防止内存爆炸 */
    if(n > 1000000) {
        /* 超过 100 万次重复，截断为 100 万次 */
        n = 1000000;
    }
    
    size_t total_size = (size_t)len_str * (size_t)n + 1;
    if(total_size > 256 * 1024 * 1024) {
        /* 超过 256MB，截断 */
        n = 256 * 1024 * 1024 / len_str;
        total_size = (size_t)len_str * (size_t)n + 1;
    }
    
    char* result = (char*)malloc(total_size);
    if(!result) {
        /* 内存分配失败，返回空字符串 */
        char* empty = (char*)malloc(1);
        empty[0] = '\0';
        stack_vm_push(g_stack_mgr, STACK_PTR, &empty);
        return 1;
    }
    
    for(int i = 0; i < n; i++) {
        memcpy(result + i * len_str, str, len_str);
    }
    result[len_str * n] = '\0';

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* 字符串除法："abcabcabc" / 3 = "abc" */
int vm_exec_arith_ptr_div(VMExecCtx* ctx, Instruction* in) {
    char *b, *a;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    /* a 是字符串，b 是数字（字符串形式） */
    int n = atoi(b);
    if(n <= 0) {
        char* result = (char*)malloc(1);
        result[0] = '\0';
        stack_vm_push(g_stack_mgr, STACK_PTR, &result);
        return 1;
    }

    int len_a = strlen(a);
    int len_result = len_a / n;
    char* result = (char*)malloc(len_result + 1);
    memcpy(result, a, len_result);
    result[len_result] = '\0';

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* 字符串减法："abcabc" - 3 = "abc"（尾部截取），3 - "abcabc" = "abc"（首部截取） */
int vm_exec_arith_ptr_sub(VMExecCtx* ctx, Instruction* in) {
    char *b, *a;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    /* 判断哪个是数字，哪个是字符串 */
    char* str;
    int n;
    int is_left_str = 1;  // 默认左操作数是字符串

    /* 判断 a 是不是数字（允许小数点和负号） */
    int a_is_num = 1;
    for(char* p = a; *p; p++) {
        if(!isdigit(*p) && *p != '-' && *p != '.') {
            a_is_num = 0;
            break;
        }
    }

    /* 判断 b 是不是数字（允许小数点和负号） */
    int b_is_num = 1;
    for(char* p = b; *p; p++) {
        if(!isdigit(*p) && *p != '-' && *p != '.') {
            b_is_num = 0;
            break;
        }
    }

    if(a_is_num && !b_is_num) {
        /* 左操作数是数字，右操作数是字符串：3 - "abcabc" = "abc"（首部截取） */
        str = b;
        n = atoi(a);
        is_left_str = 0;
    } else if(!a_is_num && b_is_num) {
        /* 左操作数是字符串，右操作数是数字："abcabc" - 3 = "abc"（尾部截取） */
        str = a;
        n = atoi(b);
        is_left_str = 1;
    } else {
        /* 两个都是字符串或两个都是数字：按尾部截取处理 */
        str = a;
        n = atoi(b);
        is_left_str = 1;
    }

    int len_str = strlen(str);
    if(n <= 0 || n >= len_str) {
        char* result = (char*)malloc(1);
        result[0] = '\0';
        stack_vm_push(g_stack_mgr, STACK_PTR, &result);
        return 1;
    }

    char* result;
    if(is_left_str) {
        /* 尾部截取："abcabc" - 3 = "abc" */
        int len_result = len_str - n;
        result = (char*)malloc(len_result + 1);
        memcpy(result, str, len_result);
        result[len_result] = '\0';
    } else {
        /* 首部截取：3 - "abcabc" = "abc" */
        int len_result = len_str - n;
        result = (char*)malloc(len_result + 1);
        memcpy(result, str + n, len_result);
        result[len_result] = '\0';
    }

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* ========== bitdecimal 高精度十进制浮点（基于 GMP mpf_t） ========== */

/* bitdecimal → string */
int vm_exec_bitdecimal_to_string(VMExecCtx* ctx, Instruction* in) {
    BitDecimal* d;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &d);

    char* s = lumyr_bitdecimal_to_string(d);

    stack_vm_push(g_stack_mgr, STACK_PTR, &s);
    return 1;
}

/* int64 → bitdecimal */
int vm_exec_bitdecimal_from_int64(VMExecCtx* ctx, Instruction* in) {
    int64_t v;
    stack_vm_pop(g_stack_mgr, STACK_INT64, &v);

    BitDecimal* d = lumyr_bitdecimal_from_int64((long long)v);

    stack_vm_push(g_stack_mgr, STACK_PTR, &d);
    return 1;
}

/* double → bitdecimal */
int vm_exec_bitdecimal_from_double(VMExecCtx* ctx, Instruction* in) {
    double v;
    stack_vm_pop(g_stack_mgr, STACK_DOUBLE, &v);

    BitDecimal* d = lumyr_bitdecimal_from_double(v);

    stack_vm_push(g_stack_mgr, STACK_PTR, &d);
    return 1;
}

/* bitdecimal 加法 */
int vm_exec_bitdecimal_add(VMExecCtx* ctx, Instruction* in) {
    BitDecimal *a, *b;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    BitDecimal* result = lumyr_bitdecimal_add(a, b);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* bitdecimal 减法 */
int vm_exec_bitdecimal_sub(VMExecCtx* ctx, Instruction* in) {
    BitDecimal *a, *b;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    BitDecimal* result = lumyr_bitdecimal_sub(a, b);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* bitdecimal 乘法 */
int vm_exec_bitdecimal_mul(VMExecCtx* ctx, Instruction* in) {
    BitDecimal *a, *b;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    BitDecimal* result = lumyr_bitdecimal_mul(a, b);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* bitdecimal 除法 */
int vm_exec_bitdecimal_div(VMExecCtx* ctx, Instruction* in) {
    BitDecimal *a, *b;
    stack_vm_pop(g_stack_mgr, STACK_PTR, &b);
    stack_vm_pop(g_stack_mgr, STACK_PTR, &a);

    BitDecimal* result = lumyr_bitdecimal_div(a, b);

    stack_vm_push(g_stack_mgr, STACK_PTR, &result);
    return 1;
}

/* ============================================================
 * 通用 Value 运算（动态类型兜底：VALUE 栈，运行时按 Value.type 分派）
 * ============================================================ */

typedef Value (*VBinFn)(Value, Value);

/* 弹 b、a，调 fn，压结果（弹栈顺序：后压的 b 先弹） */
static int vbin_exec(VBinFn fn) {
    Value a, b;
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &b);
    stack_vm_pop(g_stack_mgr, STACK_VALUE, &a);
    Value r = fn(a, b);
    stack_vm_push(g_stack_mgr, STACK_VALUE, &r);
    return 1;
}

/* 通用算术：VADD/VSUB/VMUL/VDIV/VMOD */
int vm_exec_vadd(VMExecCtx* ctx, Instruction* in) { (void)ctx;(void)in; return vbin_exec(lumyr_add); }
int vm_exec_vsub(VMExecCtx* ctx, Instruction* in) { (void)ctx;(void)in; return vbin_exec(lumyr_sub); }
int vm_exec_vmul(VMExecCtx* ctx, Instruction* in) { (void)ctx;(void)in; return vbin_exec(lumyr_mul); }
int vm_exec_vdiv(VMExecCtx* ctx, Instruction* in) { (void)ctx;(void)in; return vbin_exec(lumyr_div); }
int vm_exec_vmod(VMExecCtx* ctx, Instruction* in) { (void)ctx;(void)in; return vbin_exec(lumyr_mod); }

/* 通用比较：结果为 bool Value（压 VALUE 栈） */
int vm_exec_vgt(VMExecCtx* ctx, Instruction* in) { (void)ctx;(void)in; return vbin_exec(lumyr_gt); }
int vm_exec_vlt(VMExecCtx* ctx, Instruction* in) { (void)ctx;(void)in; return vbin_exec(lumyr_lt); }
int vm_exec_vge(VMExecCtx* ctx, Instruction* in) { (void)ctx;(void)in; return vbin_exec(lumyr_ge); }
int vm_exec_vle(VMExecCtx* ctx, Instruction* in) { (void)ctx;(void)in; return vbin_exec(lumyr_le); }
int vm_exec_veq(VMExecCtx* ctx, Instruction* in) { (void)ctx;(void)in; return vbin_exec(lumyr_eq); }
int vm_exec_vne(VMExecCtx* ctx, Instruction* in) { (void)ctx;(void)in; return vbin_exec(lumyr_ne); }
