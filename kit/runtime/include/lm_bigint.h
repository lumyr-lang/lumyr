/*
 * lm_bigint.h - 任意精度整数类型
 *
 * 设计：
 * - 用数组存储十进制数字（每个元素存 0-9，逆序存储，方便运算）
 * - 符号：正负标记
 * - 堆分配，GC 管理
 * - PTR 栈存储指针
 */

#ifndef LM_BIGINT_H
#define LM_BIGINT_H

#include <stdint.h>
#include <stddef.h>
#include "lumyr_value_type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* bigint 结构体：任意精度整数 */
typedef struct {
    uint8_t* digits;   /* 十进制数字数组，逆序存储（低位在前） */
    int len;           /* 当前长度（有效数字个数） */
    int cap;           /* 容量 */
    int sign;          /* 符号：1=正，-1=负，0=零 */
} BigInt;

/* ===== 创建/销毁 ===== */
BigInt* lumyr_bigint_from_int64(int64_t v);           /* 从 int64 创建 */
BigInt* lumyr_bigint_from_string(const char* s);      /* 从字符串创建 */
char*   lumyr_bigint_to_string(BigInt* bi);           /* 转成字符串（malloc，调用方 free） */
void    lumyr_bigint_free(BigInt* bi);                /* 释放（GC 用） */

/* ===== 加减乘除 ===== */
BigInt* lumyr_bigint_add(BigInt* a, BigInt* b);       /* a + b */
BigInt* lumyr_bigint_sub(BigInt* a, BigInt* b);       /* a - b */
BigInt* lumyr_bigint_mul(BigInt* a, BigInt* b);       /* a * b */
BigInt* lumyr_bigint_div(BigInt* a, BigInt* b);       /* a / b（整数除法） */

/* ===== 比较 ===== */
int lumyr_bigint_cmp(BigInt* a, BigInt* b);          /* -1, 0, 1 */

/* ===== 打印 ===== */
void lumyr_bigint_print(BigInt* bi);                   /* 打印到 stdout */

#ifdef __cplusplus
}
#endif

#endif /* LM_BIGINT_H */
