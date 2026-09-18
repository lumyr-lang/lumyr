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
    int int_len = len - precision;

    char* result = (char*)malloc(len + 2);  /* 多一个小数点和结束符 */

    if(int_len <= 0) {
        /* 整数部分为 0，如 0.00123 */
        strcpy(result, "0.");
        for(int i = 0; i < -int_len; i++) {
            strcat(result, "0");
        }
        strcat(result, bi_str);
    } else {
        /* 正常情况：插入小数点 */
        strncpy(result, bi_str, int_len);
        result[int_len] = '.';
        strcpy(result + int_len + 1, bi_str + int_len);
    }

    free(bi_str);

    Decimal* d = lumyr_decimal_from_string(result);
    free(result);
    return d;
}

/* 加法 */
Decimal* lumyr_decimal_add(Decimal* a, Decimal* b) {
    /* 对齐小数位数 */
    int max_prec = a->precision > b->precision ? a->precision : b->precision;

    /* 转成 bigint（乘以 10^max_prec） */
    /* 简化处理：直接用字符串拼接，不做真正的运算 */
    /* TODO: 用 bigint 实现真正的加法 */

    char buf[256];
    snprintf(buf, sizeof(buf), "(%s + %s)", a->str, b->str);

    return lumyr_decimal_from_string(buf);
}

/* 减法 */
Decimal* lumyr_decimal_sub(Decimal* a, Decimal* b) {
    /* TODO: 用 bigint 实现真正的减法 */

    char buf[256];
    snprintf(buf, sizeof(buf), "(%s - %s)", a->str, b->str);

    return lumyr_decimal_from_string(buf);
}

/* 乘法 */
Decimal* lumyr_decimal_mul(Decimal* a, Decimal* b) {
    /* TODO: 用 bigint 实现真正的乘法 */

    char buf[256];
    snprintf(buf, sizeof(buf), "(%s * %s)", a->str, b->str);

    return lumyr_decimal_from_string(buf);
}

/* 除法 */
Decimal* lumyr_decimal_div(Decimal* a, Decimal* b) {
    /* TODO: 用 bigint 实现真正的除法 */

    char buf[256];
    snprintf(buf, sizeof(buf), "(%s / %s)", a->str, b->str);

    return lumyr_decimal_from_string(buf);
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
