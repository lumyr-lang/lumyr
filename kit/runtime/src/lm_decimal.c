/*
 * lm_decimal.c - decimal 高精度十进制浮点类型实现
 *
 * 简化实现：用字符串存储，运算时转成 bigint 处理
 */
#include "lm_decimal.h"
#include "lm_bigint.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* 创建 decimal 对象 */
Decimal* lumyr_decimal_from_string(const char* s) {
    if(!s) return NULL;

    Decimal* d = (Decimal*)malloc(sizeof(Decimal));
    d->str = strdup(s);

    /* 计算小数位数 */
    d->precision = 0;
    const char* dot = strchr(s, '.');
    if(dot) {
        d->precision = strlen(dot + 1);
    }

    /* 计算符号 */
    d->sign = 1;
    if(s[0] == '-') {
        d->sign = -1;
    }

    return d;
}

/* 从 int64 创建 decimal */
Decimal* lumyr_decimal_from_int64(long long i) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", i);
    return lumyr_decimal_from_string(buf);
}

/* 转字符串 */
char* lumyr_decimal_to_string(Decimal* d) {
    if(!d) return strdup("(null)");
    return strdup(d->str);
}

/* 辅助函数：把 decimal 转成 bigint（乘以 10^precision） */
static BigInt* decimal_to_bigint(Decimal* d) {
    /* 去掉小数点，转成 bigint */
    char* no_dot = strdup(d->str);
    char* dot = strchr(no_dot, '.');
    if(dot) {
        /* 把小数点后面的字符移到前面 */
        memmove(dot, dot + 1, strlen(dot + 1) + 1);
    }

    BigInt* bi = lumyr_bigint_from_string(no_dot);
    free(no_dot);

    /* 应用符号 */
    if(d->sign < 0) {
        bi->sign = -1;
    }

    return bi;
}

/* 辅助函数：把 bigint 转成 decimal（除以 10^precision） */
static Decimal* bigint_to_decimal(BigInt* bi, int precision) {
    /* 把 bigint 转成字符串 */
    char* bi_str = lumyr_bigint_to_string(bi);
    
    /* 如果 precision 为 0，直接返回 */
    if(precision == 0) {
        Decimal* d = lumyr_decimal_from_string(bi_str);
        free(bi_str);
        return d;
    }

    /* 在合适位置插入小数点 */
    int len = strlen(bi_str);
    
    /* 处理符号 */
    int sign_offset = 0;
    if(bi_str[0] == '-') {
        sign_offset = 1;
    }
    
    /* 去掉符号后的长度 */
    int abs_len = len - sign_offset;
    int int_len = abs_len - precision;
    
    /* 计算所需缓冲区大小 */
    int result_len;
    if(int_len <= 0) {
        /* 符号 + "0." + 补零 + 绝对值 + 结束符 */
        result_len = sign_offset + 2 + (-int_len) + abs_len + 1;
    } else {
        /* 符号 + 绝对值 + "." + 结束符 */
        result_len = len + 2;
    }
    
    char* result = (char*)malloc(result_len);
    
    if(int_len <= 0) {
        /* 整数部分为 0，如 -0.00123 */
        int pos = 0;
        if(sign_offset) {
            result[pos++] = '-';
        }
        result[pos++] = '0';
        result[pos++] = '.';
        for(int i = 0; i < -int_len; i++) {
            result[pos++] = '0';
        }
        strcpy(result + pos, bi_str + sign_offset);
    } else {
        /* 正常情况：插入小数点 */
        strcpy(result, bi_str);
        result[int_len + sign_offset] = '.';
        strcpy(result + int_len + sign_offset + 1, bi_str + int_len + sign_offset);
    }

    
    /* 直接创建 Decimal 对象，不调用 lumyr_decimal_from_string，避免重新计算 precision */
    Decimal* d = (Decimal*)malloc(sizeof(Decimal));
    d->str = result;
    d->precision = precision;
    d->sign = 1;
    if(result[0] == '-') {
        d->sign = -1;
    }


    free(bi_str);

    return d;
}

/* 加法 */
Decimal* lumyr_decimal_add(Decimal* a, Decimal* b) {
    /* 对齐小数位数 */
    int max_prec = a->precision > b->precision ? a->precision : b->precision;

    /* 把 decimal 转成 bigint（去掉小数点，补齐 0 对齐精度） */
    /* 例如：3.14 + 2.718 → 3140 + 2718 = 5858 → 5.8580 */

    /* 构造 a 的整数形式字符串（去掉小数点，补齐到 max_prec 位） */
    char* a_int = strdup(a->str);
    char* a_dot = strchr(a_int, '.');
    if(a_dot) {
        /* 去掉小数点 */
        memmove(a_dot, a_dot + 1, strlen(a_dot + 1) + 1);
    }
    /* 无论有没有小数点，都补齐到 max_prec 位 */
    int a_frac_len = a_dot ? a->precision : 0;
    int pad_a = max_prec - a_frac_len;
    if(pad_a > 0) {
        int len = strlen(a_int);
        a_int = (char*)realloc(a_int, len + pad_a + 1);
        for(int i = 0; i < pad_a; i++) {
            a_int[len + i] = '0';
        }
        a_int[len + pad_a] = '\0';
    }

    /* 构造 b 的整数形式字符串（去掉小数点，补齐到 max_prec 位） */
    char* b_int = strdup(b->str);
    char* b_dot = strchr(b_int, '.');
    if(b_dot) {
        /* 去掉小数点 */
        memmove(b_dot, b_dot + 1, strlen(b_dot + 1) + 1);
    }
    /* 无论有没有小数点，都补齐到 max_prec 位 */
    int b_frac_len = b_dot ? b->precision : 0;
    int pad_b = max_prec - b_frac_len;
    if(pad_b > 0) {
        int len = strlen(b_int);
        b_int = (char*)realloc(b_int, len + pad_b + 1);
        for(int i = 0; i < pad_b; i++) {
            b_int[len + i] = '0';
        }
        b_int[len + pad_b] = '\0';
    }

    /* 用 bigint 做加法 */
    BigInt* a_bi = lumyr_bigint_from_string(a_int);
    BigInt* b_bi = lumyr_bigint_from_string(b_int);
    BigInt* result_bi = lumyr_bigint_add(a_bi, b_bi);

    /* 把结果转成 decimal（插入小数点） */
    Decimal* result = bigint_to_decimal(result_bi, max_prec);

    /* 清理 */
    free(a_int);
    free(b_int);

    return result;
}

/* 减法 */
Decimal* lumyr_decimal_sub(Decimal* a, Decimal* b) {
    /* 对齐小数位数 */
    int max_prec = a->precision > b->precision ? a->precision : b->precision;

    /* 把 decimal 转成 bigint（去掉小数点，补齐 0 对齐精度） */
    char* a_int = strdup(a->str);
    char* a_dot = strchr(a_int, '.');
    if(a_dot) {
        memmove(a_dot, a_dot + 1, strlen(a_dot + 1) + 1);
    }
    /* 无论有没有小数点（如 int 转来的 precision=0 decimal），都补齐到 max_prec 位 */
    int a_frac_len = a_dot ? a->precision : 0;
    int pad_a = max_prec - a_frac_len;
    if(pad_a > 0) {
        int len = strlen(a_int);
        a_int = (char*)realloc(a_int, len + pad_a + 1);
        for(int i = 0; i < pad_a; i++) a_int[len + i] = '0';
        a_int[len + pad_a] = '\0';
    }

    char* b_int = strdup(b->str);
    char* b_dot = strchr(b_int, '.');
    if(b_dot) {
        memmove(b_dot, b_dot + 1, strlen(b_dot + 1) + 1);
    }
    /* 无论有没有小数点，都补齐到 max_prec 位 */
    int b_frac_len = b_dot ? b->precision : 0;
    int pad_b = max_prec - b_frac_len;
    if(pad_b > 0) {
        int len = strlen(b_int);
        b_int = (char*)realloc(b_int, len + pad_b + 1);
        for(int i = 0; i < pad_b; i++) b_int[len + i] = '0';
        b_int[len + pad_b] = '\0';
    }

    /* 用 bigint 做减法 */
    BigInt* a_bi = lumyr_bigint_from_string(a_int);
    BigInt* b_bi = lumyr_bigint_from_string(b_int);
    BigInt* result_bi = lumyr_bigint_sub(a_bi, b_bi);

    /* 把结果转成 decimal（插入小数点） */
    Decimal* result = bigint_to_decimal(result_bi, max_prec);

    free(a_int);
    free(b_int);

    return result;
}

/* 乘法 */
Decimal* lumyr_decimal_mul(Decimal* a, Decimal* b) {
    /* 把 decimal 转成 bigint（去掉小数点） */
    char* a_int = strdup(a->str);
    char* a_dot = strchr(a_int, '.');
    if(a_dot) {
        memmove(a_dot, a_dot + 1, strlen(a_dot + 1) + 1);
    }

    char* b_int = strdup(b->str);
    char* b_dot = strchr(b_int, '.');
    if(b_dot) {
        memmove(b_dot, b_dot + 1, strlen(b_dot + 1) + 1);
    }

    /* 用 bigint 做乘法 */
    BigInt* a_bi = lumyr_bigint_from_string(a_int);
    BigInt* b_bi = lumyr_bigint_from_string(b_int);
    BigInt* result_bi = lumyr_bigint_mul(a_bi, b_bi);

    /* 结果的精度 = a->precision + b->precision */
    int result_prec = a->precision + b->precision;

    /* 把结果转成 decimal（插入小数点） */
    Decimal* result = bigint_to_decimal(result_bi, result_prec);

    free(a_int);
    free(b_int);

    return result;
}

/* 除法 */
Decimal* lumyr_decimal_div(Decimal* a, Decimal* b) {
        
    /* 保留 20 位小数 */
    const int result_prec = 20;

    /* 把 decimal 转成 bigint（去掉小数点） */
    char* a_int = strdup(a->str);
        char* a_dot = strchr(a_int, '.');
    if(a_dot) {
        memmove(a_dot, a_dot + 1, strlen(a_dot + 1) + 1);
            }

    char* b_int = strdup(b->str);
        char* b_dot = strchr(b_int, '.');
    if(b_dot) {
        memmove(b_dot, b_dot + 1, strlen(b_dot + 1) + 1);
            }

    /* a / b = (a_int / 10^a_prec) / (b_int / 10^b_prec) = (a_int / b_int) * 10^(b_prec - a_prec) */
    /* 如果 a_prec = b_prec，那么 a / b = a_int / b_int */
    /* 如果我们想要保留 result_prec 位小数，那么我们需要：
       a_int * 10^result_prec / b_int，然后把结果除以 10^result_prec */
    /* 所以 total_pad = result_prec + (b_prec - a_prec) */
    int total_pad = result_prec + (b->precision - a->precision);
    if(total_pad < 0) total_pad = 0;
    
    int a_len = strlen(a_int);
    a_int = (char*)realloc(a_int, a_len + total_pad + 1);
    for(int i = 0; i < total_pad; i++) {
        a_int[a_len + i] = '0';
    }
    a_int[a_len + total_pad] = '\0';
    
    /* 用 bigint 做除法 */
    BigInt* a_bi = lumyr_bigint_from_string(a_int);
        BigInt* b_bi = lumyr_bigint_from_string(b_int);
        BigInt* result_bi = lumyr_bigint_div(a_bi, b_bi);
    
    /* 把结果转成 decimal（插入小数点） */
    Decimal* result = bigint_to_decimal(result_bi, result_prec);
    
    free(a_int);
    free(b_int);

    return result;
}

/* 比较 */
int lumyr_decimal_cmp(Decimal* a, Decimal* b) {
    /* TODO: 实现真正的比较 */
    return strcmp(a->str, b->str);
}

/* 打印 */
void lumyr_decimal_print(Decimal* d) {
    if(!d) {
        printf("(null)\n");
        return;
    }
    printf("%s\n", d->str);
}

/* 释放 */
void lumyr_decimal_free(Decimal* d) {
    if(!d) return;
    free(d->str);
    free(d);
}
