/*
 * lm_bitdecimal.c - bitdecimal 高精度十进制浮点类型实现
 *
 * 基于 GMP mpf_t 实现
 */
#include "lm_bitdecimal.h"
#include "lm_decimal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* mpf 精度（位数）：512 位 ≈ 154 位十进制有效数字，
   足以让常规十进制输入输出在小数点后 60 位内保持精确 */
#define BITDECIMAL_PREC_BITS 512

/* 创建 bitdecimal 对象 */
BitDecimal* lumyr_bitdecimal_from_string(const char* s) {
    BitDecimal* d = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!d) return NULL;

    mpf_init2(d->value, BITDECIMAL_PREC_BITS);
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

    mpf_init2(d->value, BITDECIMAL_PREC_BITS);
    mpf_set_si(d->value, i);
    d->precision = 0;

    return d;
}

BitDecimal* lumyr_bitdecimal_from_double(double d) {
    BitDecimal* bd = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!bd) return NULL;

    mpf_init2(bd->value, BITDECIMAL_PREC_BITS);
    mpf_set_d(bd->value, d);
    bd->precision = 15;  /* double 默认精度 */

    return bd;
}

/* 在十进制数字串上做 p 位小数的半偶舍入。
   digits 为纯数字串（无符号/小数点），value = 0.digits × 10^e；
   就地截断 digits 并更新 e（进位时 e+1）。 */
static void bd_round_digits(char* digits, int* e_ptr, int p) {
    int e = *e_ptr;
    int dlen = (int)strlen(digits);
    /* 保留的有效数字个数 n = e + p（e<=0 时前 -e 位是小数点后的前导零） */
    int n = e + p;
    if(n < 0) n = 0;
    if(n >= dlen) return;  /* 位数不足，无需舍入 */

    int round_up = 0;
    if(digits[n] > '5') {
        round_up = 1;
    } else if(digits[n] == '5') {
        /* 后面还有非零数字 → 必进位；恰好 5 结尾 → 半偶 */
        int rest_nonzero = 0;
        for(int i = n + 1; i < dlen; i++) {
            if(digits[i] != '0') { rest_nonzero = 1; break; }
        }
        if(rest_nonzero) {
            round_up = 1;
        } else if(n > 0 && (digits[n - 1] - '0') % 2 == 1) {
            round_up = 1;  /* 保留部分末位为奇数 → 进位成偶数 */
        }
    }
    digits[n] = '\0';
    if(round_up) {
        int i = n - 1;
        while(i >= 0 && digits[i] == '9') { digits[i] = '0'; i--; }
        if(i >= 0) {
            digits[i]++;
        } else {
            /* 全部进位：右移一位，前导 1，指数 +1 */
            memmove(digits + 1, digits, n + 1);
            digits[0] = '1';
            (*e_ptr)++;
        }
    }
}

/* 按指定小数位 p 转字符串（半偶舍入），p=0 输出整数 */
static char* bitdecimal_to_string_p(BitDecimal* d, int p) {
    /* 取全部有效数字：value = 0.digits × 10^exp（digits 可带 '-'） */
    mp_exp_t exp;
    char* str = mpf_get_str(NULL, &exp, 10, 0, d->value);

    int neg = (str[0] == '-');
    char* digits = neg ? str + 1 : str;
    int e = (int)exp;

    bd_round_digits(digits, &e, p);
    int dlen = (int)strlen(digits);

    /* 组装：符号 + 整数部分(max(e,1) 位，不足补零) + [. + 恰好 p 位小数] */
    int int_len = e > 0 ? e : 1;
    int cap = (neg ? 1 : 0) + int_len + (p > 0 ? 1 + p : 0) + 8;
    char* result = (char*)malloc(cap);
    char* w = result;

    if(neg) *w++ = '-';
    if(e <= 0) {
        *w++ = '0';
    } else {
        for(int i = 0; i < e; i++) {
            *w++ = i < dlen ? digits[i] : '0';
        }
    }
    if(p > 0) {
        *w++ = '.';
        for(int i = 0; i < p; i++) {
            char ch = '0';
            if(e >= 0) {
                int idx = e + i;
                if(idx < dlen) ch = digits[idx];
            } else {
                /* e<0：小数点后先有 -e 个前导零，再接 digits */
                int zeros = -e;
                if(i >= zeros && (i - zeros) < dlen) ch = digits[i - zeros];
            }
            *w++ = ch;
        }
    }
    *w = '\0';

    free(str);
    return result;
}

/* 转字符串：按 d->precision 输出固定小数位（半偶舍入），precision=0 输出整数 */
char* lumyr_bitdecimal_to_string(BitDecimal* d) {
    if (!d) return strdup("");
    return bitdecimal_to_string_p(d, d->precision > 0 ? d->precision : 0);
}

/* 四则运算 */
BitDecimal* lumyr_bitdecimal_add(BitDecimal* a, BitDecimal* b) {
    BitDecimal* d = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!d) return NULL;

    mpf_init2(d->value, BITDECIMAL_PREC_BITS);
    mpf_add(d->value, a->value, b->value);
    d->precision = a->precision > b->precision ? a->precision : b->precision;

    return d;
}

BitDecimal* lumyr_bitdecimal_sub(BitDecimal* a, BitDecimal* b) {
    BitDecimal* d = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!d) return NULL;

    mpf_init2(d->value, BITDECIMAL_PREC_BITS);
    mpf_sub(d->value, a->value, b->value);
    d->precision = a->precision > b->precision ? a->precision : b->precision;

    return d;
}

BitDecimal* lumyr_bitdecimal_mul(BitDecimal* a, BitDecimal* b) {
    BitDecimal* d = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!d) return NULL;

    mpf_init2(d->value, BITDECIMAL_PREC_BITS);
    mpf_mul(d->value, a->value, b->value);
    d->precision = a->precision + b->precision;

    return d;
}

BitDecimal* lumyr_bitdecimal_div(BitDecimal* a, BitDecimal* b) {
    BitDecimal* d = (BitDecimal*)malloc(sizeof(BitDecimal));
    if (!d) return NULL;

    mpf_init2(d->value, BITDECIMAL_PREC_BITS);
    mpf_div(d->value, a->value, b->value);
    d->precision = a->precision + 10;  /* 除法多保留10位精度 */

    return d;
}

/* 比较：十进制数值语义。二进制 mpf 对 0.1 类十进制值不能精确表示，
   直接 mpf_cmp 会让 1.2+0.3 != 1.5；统一按两者较大小数位做半偶舍入
   （复用 to_string 的舍入），再按十进制字符串做数值比较（复用 decimal_cmp，
   自动处理 -0 == 0 与精度对齐） */
int lumyr_bitdecimal_cmp(BitDecimal* a, BitDecimal* b) {
    if (!a || !b) return (a != NULL) - (b != NULL);
    int p = a->precision > b->precision ? a->precision : b->precision;
    char* as = bitdecimal_to_string_p(a, p);
    char* bs = bitdecimal_to_string_p(b, p);
    Decimal* da = lumyr_decimal_from_string(as);
    Decimal* db = lumyr_decimal_from_string(bs);
    int r = lumyr_decimal_cmp(da, db);
    lumyr_decimal_free(da);
    lumyr_decimal_free(db);
    free(as);
    free(bs);
    return r;
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
