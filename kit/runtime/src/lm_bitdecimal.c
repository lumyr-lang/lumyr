/*
 * lm_bitdecimal.c - bitdecimal 高精度十进制浮点类型实现
 *
 * 基于 GMP mpf_t 实现
 */
#include "lm_bitdecimal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 创建 bitdecimal 对象 */
BitDecimal* lumyr_bitdecimal_from_string(const char* s) {
    BitDecimal* d = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!d) return NULL;

    mpf_init(d->value);
    mpf_set_str(d->value, s, 10);

    /* 计算小数位数 */
    d->precision = 0;
    const char* dot = strchr(s, '.');
    if (dot) {
        d->precision = (int)strlen(dot + 1);
    }

    return d;
}

BitDecimal* lumyr_bitdecimal_from_int64(long long i) {
    BitDecimal* d = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!d) return NULL;

    mpf_init(d->value);
    mpf_set_si(d->value, i);
    d->precision = 0;

    return d;
}

BitDecimal* lumyr_bitdecimal_from_double(double d) {
    BitDecimal* bd = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!bd) return NULL;

    mpf_init(bd->value);
    mpf_set_d(bd->value, d);
    bd->precision = 15;  /* double 默认精度 */

    return bd;
}

/* 转字符串 */
char* lumyr_bitdecimal_to_string(BitDecimal* d) {
    if (!d) return strdup("");

    /* 用 GMP 的 mpf_get_str 转换 */
    mp_exp_t exp;
    char* str = mpf_get_str(NULL, &exp, 10, 0, d->value);

    /* 处理小数点位置 */
    int len = (int)strlen(str);
    char* result = (char*)malloc(len + 2);  /* 多一个小数点和结束符 */

    if (exp <= 0) {
        /* 纯小数：0.00xxx */
        result[0] = '0';
        result[1] = '.';
        for (int i = 0; i < -exp; i++) {
            result[2 + i] = '0';
        }
        strcpy(result + 2 - exp, str);
    } else if (exp < len) {
        /* 混合：xxx.xxx */
        strncpy(result, str, exp);
        result[exp] = '.';
        strcpy(result + exp + 1, str + exp);
    } else {
        /* 纯整数：xxx000 */
        strcpy(result, str);
        for (int i = len; i < exp; i++) {
            result[i] = '0';
        }
        result[exp] = '\0';
    }

    free(str);
    return result;
}

/* 四则运算 */
BitDecimal* lumyr_bitdecimal_add(BitDecimal* a, BitDecimal* b) {
    BitDecimal* d = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!d) return NULL;

    mpf_init(d->value);
    mpf_add(d->value, a->value, b->value);
    d->precision = a->precision > b->precision ? a->precision : b->precision;

    return d;
}

BitDecimal* lumyr_bitdecimal_sub(BitDecimal* a, BitDecimal* b) {
    BitDecimal* d = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!d) return NULL;

    mpf_init(d->value);
    mpf_sub(d->value, a->value, b->value);
    d->precision = a->precision > b->precision ? a->precision : b->precision;

    return d;
}

BitDecimal* lumyr_bitdecimal_mul(BitDecimal* a, BitDecimal* b) {
    BitDecimal* d = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!d) return NULL;

    mpf_init(d->value);
    mpf_mul(d->value, a->value, b->value);
    d->precision = a->precision + b->precision;

    return d;
}

BitDecimal* lumyr_bitdecimal_div(BitDecimal* a, BitDecimal* b) {
    BitDecimal* d = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!d) return NULL;

    mpf_init(d->value);
    mpf_div(d->value, a->value, b->value);
    d->precision = a->precision + 10;  /* 除法多保留10位精度 */

    return d;
}

/* 比较 */
int lumyr_bitdecimal_cmp(BitDecimal* a, BitDecimal* b) {
    return mpf_cmp(a->value, b->value);
}

/* 打印 */
void lumyr_bitdecimal_print(BitDecimal* d) {
    char* str = lumyr_bitdecimal_to_string(d);
    printf("%s\n", str);
    free(str);
}

/* 释放 */
void lumyr_bitdecimal_free(BitDecimal* d) {
    if (!d) return;
    mpf_clear(d->value);
    free(d);
}
