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
    bi->digits = (uint32_t*)realloc(bi->digits, new_cap * sizeof(uint32_t));
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

    uint64_t carry = 0;
    for (int i = 0; i < max_len; i++) {
        uint64_t da = i < a->len ? a->digits[i] : 0;
        uint64_t db = i < b->len ? b->digits[i] : 0;
        uint64_t sum = da + db + carry;
        result->digits[i] = (uint32_t)(sum & BIGINT_MASK);
        carry = sum >> 30;
    }
    if (carry > 0) {
        result->digits[max_len] = (uint32_t)carry;
        result->len = max_len + 1;
    } else {
        result->len = max_len;
    }
}

/* 绝对值减法：|a| - |b|，要求 |a| >= |b|，结果存入 result（调用方分配） */
static void abs_sub(BigInt* a, BigInt* b, BigInt* result) {
    ensure_cap(result, a->len);

    int64_t borrow = 0;
    for (int i = 0; i < a->len; i++) {
        int64_t da = a->digits[i];
        int64_t db = i < b->len ? b->digits[i] : 0;
        int64_t diff = da - db - borrow;
        if (diff < 0) {
            diff += BIGINT_BASE;
            borrow = 1;
        } else {
            borrow = 0;
        }
        result->digits[i] = (uint32_t)diff;
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
        bi->digits = (uint32_t*)calloc(bi->cap, sizeof(uint32_t));
        return bi;
    }

    bi->sign = v < 0 ? -1 : 1;
    uint64_t uv = v < 0 ? (uint64_t)(-v) : (uint64_t)v;

    bi->cap = 8;
    bi->digits = (uint32_t*)calloc(bi->cap, sizeof(uint32_t));
    bi->len = 0;

    while (uv > 0) {
        ensure_cap(bi, bi->len + 1);
        bi->digits[bi->len] = (uint32_t)(uv & BIGINT_MASK);
        uv >>= 30;
        bi->len++;
    }

    return bi;
}

BigInt* lumyr_bigint_from_string(const char* s) {
    if (!s || *s == '\0') {
        return lumyr_bigint_from_int64(0);
    }

    /* 处理符号 */
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }

    BigInt* result = lumyr_bigint_from_int64(0);
    result->sign = sign;

    /* 从左到右，每次 result = result * 10 + digit */
    BigInt* ten = lumyr_bigint_from_int64(10);

    while (*s != '\0' && isdigit((unsigned char)*s)) {
        int digit = *s - '0';

        /* result = result * 10 */
        BigInt* temp = lumyr_bigint_mul(result, ten);
        lumyr_bigint_free(result);
        result = temp;

        /* result = result + digit */
        BigInt* d = lumyr_bigint_from_int64(digit);
        temp = lumyr_bigint_add(result, d);
        lumyr_bigint_free(result);
        lumyr_bigint_free(d);
        result = temp;

        s++;
    }

    lumyr_bigint_free(ten);

    if (result->len == 1 && result->digits[0] == 0) {
        result->sign = 0;
    }

    return result;
}

char* lumyr_bigint_to_string(BigInt* bi) {
    if (!bi || bi->sign == 0) {
        return strdup("0");
    }

    /* 先把绝对值转成十进制字符串 */
    BigInt* abs_bi = (BigInt*)calloc(1, sizeof(BigInt));
    abs_bi->sign = 1;
    abs_bi->cap = bi->cap;
    abs_bi->len = bi->len;
    abs_bi->digits = (uint32_t*)calloc(abs_bi->cap, sizeof(uint32_t));
    memcpy(abs_bi->digits, bi->digits, bi->len * sizeof(uint32_t));

    /* 用除法转成十进制字符串 */
    BigInt* ten = lumyr_bigint_from_int64(10);
    char* buf = (char*)calloc(1024, 1);
    int pos = 1023;
    buf[pos--] = '\0';

    BigInt* rem = abs_bi;
    while (rem->len > 1 || rem->digits[0] > 0) {
        /* rem / 10，取商和余数 */
        BigInt* q = lumyr_bigint_div(rem, ten);
        BigInt* new_rem = (BigInt*)calloc(1, sizeof(BigInt));
        /* 计算余数：rem - q * 10 */
        BigInt* q_mul_10 = lumyr_bigint_mul(q, ten);
        abs_sub(rem, q_mul_10, new_rem);
        lumyr_bigint_free(q_mul_10);
        lumyr_bigint_free(rem);
        /* 把 rem 更新为商，不是余数 */
        rem = q;

        /* 把余数存入 buf */
        buf[pos--] = '0' + new_rem->digits[0];
        lumyr_bigint_free(new_rem);
    }

    lumyr_bigint_free(rem);
    lumyr_bigint_free(ten);

    /* 加上符号 */
    if (bi->sign < 0) {
        buf[pos--] = '-';
    }

    return strdup(buf + pos + 1);
}

void lumyr_bigint_free(BigInt* bi) {
    if (!bi) return;
    free(bi->digits);
    free(bi);
}

/* ===== 加减乘除 ===== */

BigInt* lumyr_bigint_add(BigInt* a, BigInt* b) {
    if (!a || !b) return NULL;

    /* 零的情况：直接复制，不调用 lumyr_bigint_from_string(lumyr_bigint_to_string(b))，避免无限递归 */
    if (a->sign == 0) {
        BigInt* result = (BigInt*)calloc(1, sizeof(BigInt));
        result->sign = b->sign;
        result->cap = b->cap;
        result->len = b->len;
        result->digits = (uint32_t*)calloc(result->cap, sizeof(uint32_t));
        memcpy(result->digits, b->digits, b->len * sizeof(uint32_t));
        return result;
    }
    if (b->sign == 0) {
        BigInt* result = (BigInt*)calloc(1, sizeof(BigInt));
        result->sign = a->sign;
        result->cap = a->cap;
        result->len = a->len;
        result->digits = (uint32_t*)calloc(result->cap, sizeof(uint32_t));
        memcpy(result->digits, a->digits, a->len * sizeof(uint32_t));
        return result;
    }

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
            result->digits = (uint32_t*)calloc(result->cap, sizeof(uint32_t));
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
    result->cap = a->len + b->len + 1;
    result->digits = (uint32_t*)calloc(result->cap, sizeof(uint32_t));
    result->len = a->len + b->len;

    /* 朴素乘法 */
    for (int i = 0; i < a->len; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->len; j++) {
            uint64_t prod = (uint64_t)a->digits[i] * b->digits[j] + result->digits[i + j] + carry;
            result->digits[i + j] = (uint32_t)(prod & BIGINT_MASK);
            carry = prod >> 30;
        }
        if (carry > 0) {
            result->digits[i + b->len] = (uint32_t)carry;
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
    result->digits = (uint32_t*)calloc(result->cap, sizeof(uint32_t));
    result->len = 1;

    /* 直接用绝对值，不调用 lumyr_bigint_from_string(lumyr_bigint_to_string(a))，避免无限递归 */
    BigInt* remainder = (BigInt*)calloc(1, sizeof(BigInt));
    remainder->sign = 1;
    remainder->cap = a->cap;
    remainder->len = a->len;
    remainder->digits = (uint32_t*)calloc(remainder->cap, sizeof(uint32_t));
    memcpy(remainder->digits, a->digits, a->len * sizeof(uint32_t));

    BigInt* abs_b = (BigInt*)calloc(1, sizeof(BigInt));
    abs_b->sign = 1;
    abs_b->cap = b->cap;
    abs_b->len = b->len;
    abs_b->digits = (uint32_t*)calloc(abs_b->cap, sizeof(uint32_t));
    memcpy(abs_b->digits, b->digits, b->len * sizeof(uint32_t));

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
