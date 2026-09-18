/*
 * lm_bigint.c - 任意精度整数类型实现
 */

#include "lm_bigint.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ===== 内部辅助函数 ===== */

/* 去除前导零（逆序存储，所以是尾部零） */
static void strip_leading_zeros(BigInt* bi) {
    while (bi->len > 1 && bi->digits[bi->len - 1] == 0) {
        bi->len--;
    }
    if (bi->len == 1 && bi->digits[0] == 0) {
        bi->sign = 0;  /* 零 */
    }
}

/* 分配容量 */
static void ensure_cap(BigInt* bi, int need) {
    if (bi->cap >= need) return;
    int new_cap = bi->cap > 0 ? bi->cap * 2 : 8;
    while (new_cap < need) new_cap *= 2;
    bi->digits = (uint8_t*)realloc(bi->digits, new_cap);
    bi->cap = new_cap;
}

/* 比较绝对值大小：|a| > |b| 返回 1，等于返回 0，小于返回 -1 */
static int abs_cmp(BigInt* a, BigInt* b) {
    if (a->len != b->len) {
        return a->len > b->len ? 1 : -1;
    }
    for (int i = a->len - 1; i >= 0; i--) {
        if (a->digits[i] != b->digits[i]) {
            return a->digits[i] > b->digits[i] ? 1 : -1;
        }
    }
    return 0;
}

/* 绝对值加法：|a| + |b|，结果存入 result（调用方分配） */
static void abs_add(BigInt* a, BigInt* b, BigInt* result) {
    int max_len = a->len > b->len ? a->len : b->len;
    ensure_cap(result, max_len + 1);

    int carry = 0;
    for (int i = 0; i < max_len; i++) {
        int da = i < a->len ? a->digits[i] : 0;
        int db = i < b->len ? b->digits[i] : 0;
        int sum = da + db + carry;
        result->digits[i] = sum % 10;
        carry = sum / 10;
    }
    if (carry > 0) {
        result->digits[max_len] = carry;
        result->len = max_len + 1;
    } else {
        result->len = max_len;
    }
}

/* 绝对值减法：|a| - |b|，要求 |a| >= |b|，结果存入 result（调用方分配） */
static void abs_sub(BigInt* a, BigInt* b, BigInt* result) {
    ensure_cap(result, a->len);

    int borrow = 0;
    for (int i = 0; i < a->len; i++) {
        int da = a->digits[i];
        int db = i < b->len ? b->digits[i] : 0;
        int diff = da - db - borrow;
        if (diff < 0) {
            diff += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        result->digits[i] = diff;
    }
    result->len = a->len;
    strip_leading_zeros(result);
}

/* ===== 创建/销毁 ===== */

BigInt* lumyr_bigint_from_int64(int64_t v) {
    BigInt* bi = (BigInt*)calloc(1, sizeof(BigInt));
    if (v == 0) {
        bi->sign = 0;
        bi->len = 1;
        bi->cap = 8;
        bi->digits = (uint8_t*)calloc(bi->cap, 1);
        return bi;
    }

    bi->sign = v < 0 ? -1 : 1;
    uint64_t uv = v < 0 ? (uint64_t)(-v) : (uint64_t)v;

    bi->cap = 8;
    bi->digits = (uint8_t*)calloc(bi->cap, 1);
    bi->len = 0;

    while (uv > 0) {
        ensure_cap(bi, bi->len + 1);
        bi->digits[bi->len++] = uv % 10;
        uv /= 10;
    }
    return bi;
}

BigInt* lumyr_bigint_from_string(const char* s) {
    BigInt* bi = (BigInt*)calloc(1, sizeof(BigInt));
    if (!s || *s == '\0') {
        bi->sign = 0;
        bi->len = 1;
        bi->cap = 8;
        bi->digits = (uint8_t*)calloc(bi->cap, 1);
        return bi;
    }

    int start = 0;
    if (s[0] == '-') {
        bi->sign = -1;
        start = 1;
    } else if (s[0] == '+') {
        bi->sign = 1;
        start = 1;
    } else {
        bi->sign = 1;
    }

    int slen = strlen(s + start);
    bi->cap = slen + 2;
    bi->digits = (uint8_t*)calloc(bi->cap, 1);
    bi->len = slen;

    /* 逆序存储：字符串高位在前，digits 低位在前 */
    for (int i = 0; i < slen; i++) {
        char c = s[start + slen - 1 - i];
        if (isdigit((unsigned char)c)) {
            bi->digits[i] = c - '0';
        }
    }
    strip_leading_zeros(bi);
    if (bi->sign != 0 && bi->len == 1 && bi->digits[0] == 0) {
        bi->sign = 0;
    }
    return bi;
}

char* lumyr_bigint_to_string(BigInt* bi) {
    if (!bi || bi->sign == 0) {
        return strdup("0");
    }

    /* 符号 + 数字 + 结束符 */
    int buf_len = bi->len + 2;
    char* buf = (char*)malloc(buf_len);
    int pos = 0;

    if (bi->sign < 0) {
        buf[pos++] = '-';
    }

    for (int i = bi->len - 1; i >= 0; i--) {
        buf[pos++] = bi->digits[i] + '0';
    }
    buf[pos] = '\0';
    return buf;
}

void lumyr_bigint_free(BigInt* bi) {
    if (!bi) return;
    free(bi->digits);
    free(bi);
}

/* ===== 加减乘除 ===== */

BigInt* lumyr_bigint_add(BigInt* a, BigInt* b) {
    if (!a || !b) return NULL;

    /* 零的情况 */
    if (a->sign == 0) return lumyr_bigint_from_string(lumyr_bigint_to_string(b));
    if (b->sign == 0) return lumyr_bigint_from_string(lumyr_bigint_to_string(a));

    BigInt* result = (BigInt*)calloc(1, sizeof(BigInt));

    if (a->sign == b->sign) {
        /* 同号：绝对值相加，符号不变 */
        result->sign = a->sign;
        abs_add(a, b, result);
    } else {
        /* 异号：绝对值相减，符号取绝对值大的 */
        int cmp = abs_cmp(a, b);
        if (cmp == 0) {
            /* 相等，结果为零 */
            result->sign = 0;
            result->len = 1;
            result->cap = 8;
            result->digits = (uint8_t*)calloc(result->cap, 1);
        } else if (cmp > 0) {
            /* |a| > |b|，结果符号同 a */
            result->sign = a->sign;
            abs_sub(a, b, result);
        } else {
            /* |a| < |b|，结果符号同 b */
            result->sign = b->sign;
            abs_sub(b, a, result);
        }
    }
    return result;
}

BigInt* lumyr_bigint_sub(BigInt* a, BigInt* b) {
    if (!a || !b) return NULL;
    /* a - b = a + (-b) */
    BigInt neg_b = *b;
    neg_b.sign = -b->sign;
    return lumyr_bigint_add(a, &neg_b);
}

BigInt* lumyr_bigint_mul(BigInt* a, BigInt* b) {
    if (!a || !b) return NULL;
    if (a->sign == 0 || b->sign == 0) {
        return lumyr_bigint_from_int64(0);
    }

    BigInt* result = (BigInt*)calloc(1, sizeof(BigInt));
    result->sign = a->sign * b->sign;
    result->cap = a->len + b->len;
    result->digits = (uint8_t*)calloc(result->cap, 1);
    result->len = a->len + b->len;

    /* 竖式乘法 */
    for (int i = 0; i < a->len; i++) {
        int carry = 0;
        for (int j = 0; j < b->len; j++) {
            int prod = result->digits[i + j] + a->digits[i] * b->digits[j] + carry;
            result->digits[i + j] = prod % 10;
            carry = prod / 10;
        }
        if (carry > 0) {
            result->digits[i + b->len] += carry;
        }
    }
    strip_leading_zeros(result);
    return result;
}

BigInt* lumyr_bigint_div(BigInt* a, BigInt* b) {
    if (!a || !b || b->sign == 0) return NULL;  /* 除零错误 */
    if (a->sign == 0) {
        return lumyr_bigint_from_int64(0);
    }

    /* 简化：先用减法实现（后续可以优化为试商法） */
    BigInt* result = (BigInt*)calloc(1, sizeof(BigInt));
    result->sign = a->sign * b->sign;
    result->cap = 8;
    result->digits = (uint8_t*)calloc(result->cap, 1);
    result->len = 1;

    BigInt* remainder = lumyr_bigint_from_string(lumyr_bigint_to_string(a));
    remainder->sign = 1;  /* 用绝对值做除法 */

    BigInt* abs_b = lumyr_bigint_from_string(lumyr_bigint_to_string(b));
    abs_b->sign = 1;

    int quotient = 0;
    while (abs_cmp(remainder, abs_b) >= 0) {
        BigInt* new_rem = (BigInt*)calloc(1, sizeof(BigInt));
        abs_sub(remainder, abs_b, new_rem);
        lumyr_bigint_free(remainder);
        remainder = new_rem;
        quotient++;
    }

    /* 把商转成 BigInt */
    lumyr_bigint_free(remainder);
    lumyr_bigint_free(abs_b);
    lumyr_bigint_free(result);
    result = lumyr_bigint_from_int64(quotient);
    result->sign = a->sign * b->sign;
    return result;
}

/* ===== 比较 ===== */

int lumyr_bigint_cmp(BigInt* a, BigInt* b) {
    if (!a || !b) return 0;
    if (a->sign != b->sign) {
        return a->sign > b->sign ? 1 : -1;
    }
    if (a->sign == 0) return 0;

    int cmp = abs_cmp(a, b);
    return a->sign > 0 ? cmp : -cmp;
}

/* ===== 打印 ===== */

void lumyr_bigint_print(BigInt* bi) {
    char* s = lumyr_bigint_to_string(bi);
    printf("%s", s);
    free(s);
}
