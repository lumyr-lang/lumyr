/*
 * lm_decimal.h - decimal 高精度十进制浮点类型
 *
 * 存储方式：整数部分 + 小数部分
 * - 用两个 bigint 存储整数部分和小数部分
 * - 或者直接用字符串存储，解析时处理
 *
 * 简化实现：用字符串存储，运算时转成 bigint 处理
 */
#ifndef LM_DECIMAL_H
#define LM_DECIMAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decimal 结构体：用字符串存储，运算时解析 */
typedef struct {
    char* str;        /* 字符串表示，如 "3.14"、"123.456" */
    int precision;    /* 小数位数 */
    int sign;         /* 符号：1 正，-1 负 */
} Decimal;

/* 创建 decimal 对象 */
Decimal* lumyr_decimal_from_string(const char* s);
Decimal* lumyr_decimal_from_int64(long long i);

/* 转字符串 */
char* lumyr_decimal_to_string(Decimal* d);

/* 四则运算 */
Decimal* lumyr_decimal_add(Decimal* a, Decimal* b);
Decimal* lumyr_decimal_sub(Decimal* a, Decimal* b);
Decimal* lumyr_decimal_mul(Decimal* a, Decimal* b);
Decimal* lumyr_decimal_div(Decimal* a, Decimal* b);

/* 比较 */
int lumyr_decimal_cmp(Decimal* a, Decimal* b);

/* 打印 */
void lumyr_decimal_print(Decimal* d);

/* 释放 */
void lumyr_decimal_free(Decimal* d);

#ifdef __cplusplus
}
#endif

#endif /* LM_DECIMAL_H */
