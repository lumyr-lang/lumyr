/*
 * lm_bitdecimal.h - bitdecimal 高精度十进制浮点类型
 *
 * 基于 GMP mpf_t 实现，支持任意精度的十进制浮点运算
 * 比现有 decimal 性能更高，精度更高
 */
#ifndef LM_BITDECIMAL_H
#define LM_BITDECIMAL_H

#include <stddef.h>
#include <gmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BitDecimal 结构体：基于 GMP mpf_t */
typedef struct {
    mpf_t value;       /* GMP 多精度浮点值 */
    int precision;     /* 小数位数（用于显示） */
} BitDecimal;

/* 创建 bitdecimal 对象 */
BitDecimal* lumyr_bitdecimal_from_string(const char* s);
BitDecimal* lumyr_bitdecimal_from_int64(long long i);
BitDecimal* lumyr_bitdecimal_from_double(double d);

/* 转字符串 */
char* lumyr_bitdecimal_to_string(BitDecimal* d);

/* 四则运算 */
BitDecimal* lumyr_bitdecimal_add(BitDecimal* a, BitDecimal* b);
BitDecimal* lumyr_bitdecimal_sub(BitDecimal* a, BitDecimal* b);
BitDecimal* lumyr_bitdecimal_mul(BitDecimal* a, BitDecimal* b);
BitDecimal* lumyr_bitdecimal_div(BitDecimal* a, BitDecimal* b);

/* 比较 */
int lumyr_bitdecimal_cmp(BitDecimal* a, BitDecimal* b);

/* 打印 */
void lumyr_bitdecimal_print(BitDecimal* d);

/* 释放 */
void lumyr_bitdecimal_free(BitDecimal* d);

#ifdef __cplusplus
}
#endif

#endif /* LM_BITDECIMAL_H */
