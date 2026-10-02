// lm_lex_unescape.c —— 字面量转义序列统一解码（实现见同名头文件规则说明）
#include "lm_lex_unescape.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define LM_UNICODE_MAX      0x10FFFFu
#define LM_SURROGATE_LO     0xD800u
#define LM_SURROGATE_HI     0xDFFFu

/* 错误正文缓冲（双语） */
typedef char lmEscErr[192];

static void esc_err(char* buf, int size, const char* cn, const char* en) {
    snprintf(buf, (size_t)size, "%s / %s", cn, en);
}

static int hexVal(int c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int octVal(int c) {
    return (c >= '0' && c <= '7') ? c - '0' : -1;
}

/* 把 Unicode 码点按 UTF-8 写入 out（1-4 字节），返回字节数 */
static int encodeUtf8(unsigned int cp, char* out) {
    if(cp <= 0x7Fu) {
        out[0] = (char)cp;
        return 1;
    }
    if(cp <= 0x7FFu) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if(cp <= 0xFFFFu) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/*
 * 解码一个转义序列。
 * 入口 *pp 指向反斜杠（保证 *pp < end 且 **pp == '\\'）。
 * 出口 *pp 越过被消费的全部字符；out 至少 4 字节可写。
 */
static int decode_one(const char** pp, const char* end, int quoteKind,
                      char* out, int* outN, lmEscErr err) {
    const char* p = *pp;
    /* 越过反斜杠 */
    if(++p >= end) {
        esc_err(err, sizeof(lmEscErr),
                "字符串结尾出现孤立的反斜杠",
                "lone backslash at end of string");
        return -1;
    }
    int c = (unsigned char)p[0];
    int n = 0;

    switch(c) {
        case 'a': n = 1; out[0] = '\a'; p++; break;
        case 'b': n = 1; out[0] = '\b'; p++; break;
        case 'f': n = 1; out[0] = '\f'; p++; break;
        case 'n': n = 1; out[0] = '\n'; p++; break;
        case 'r': n = 1; out[0] = '\r'; p++; break;
        case 't': n = 1; out[0] = '\t'; p++; break;
        case 'v': n = 1; out[0] = '\v'; p++; break;
        case '\\': n = 1; out[0] = '\\'; p++; break;
        case '"': n = 1; out[0] = '"'; p++; break;
        case '\'': n = 1; out[0] = '\''; p++; break;

        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            /* 八进制 1-3 位变长，值 0-255 */
            unsigned int v = 0;
            int digits = 0;
            while(digits < 3 && p < end) {
                int d = octVal((unsigned char)*p);
                if(d < 0) break;
                v = (v << 3) | (unsigned)d;
                p++;
                digits++;
            }
            if(v > 255u) {
                esc_err(err, sizeof(lmEscErr),
                        "八进制转义值超出字节范围（>255）",
                        "octal escape value out of byte range (>255)");
                return -1;
            }
            out[0] = (char)v;
            n = 1;
            break;
        }

        case 'x': {
            /* 十六进制 1-2 位，值 0-255 */
            p++;
            unsigned int v = 0;
            int digits = 0;
            while(digits < 2 && p < end) {
                int d = hexVal((unsigned char)*p);
                if(d < 0) break;
                v = (v << 4) | (unsigned)d;
                p++;
                digits++;
            }
            if(digits == 0) {
                esc_err(err, sizeof(lmEscErr),
                        "\\x 后至少需要 1 位十六进制数字",
                        "\\x requires at least 1 hexadecimal digit");
                return -1;
            }
            out[0] = (char)v;
            n = 1;
            break;
        }

        case 'u':
        case 'U': {
            int need = (c == 'u') ? 4 : 8;
            const char* tag = (c == 'u') ? "\\u" : "\\U";
            if(quoteKind == LM_QUOTE_CHAR) {
                esc_err(err, sizeof(lmEscErr),
                        "字符字面量不支持 Unicode 转义（仅单字节）",
                        "unicode escape not allowed in char literal");
                return -1;
            }
            p++;
            unsigned int v = 0;
            for(int i = 0; i < need; i++) {
                if(p >= end) {
                    char cn[128], en[128];
                    snprintf(cn, sizeof(cn),
                             "%s 需要恰好 %d 位十六进制数字（位数不足）",
                             tag, need);
                    snprintf(en, sizeof(en),
                             "%s requires exactly %d hexadecimal digits",
                             tag, need);
                    esc_err(err, sizeof(lmEscErr), cn, en);
                    return -1;
                }
                int d = hexVal((unsigned char)*p);
                if(d < 0) {
                    char cn[128], en[128];
                    snprintf(cn, sizeof(cn),
                             "%s 中出现非法的十六进制字符 '%c'",
                             tag, *p);
                    snprintf(en, sizeof(en),
                             "invalid hexadecimal character '%c' in %s",
                             *p, tag);
                    esc_err(err, sizeof(lmEscErr), cn, en);
                    return -1;
                }
                v = (v << 4) | (unsigned)d;
                p++;
            }
            if(v > LM_UNICODE_MAX ||
               (v >= LM_SURROGATE_LO && v <= LM_SURROGATE_HI)) {
                esc_err(err, sizeof(lmEscErr),
                        "非法 Unicode 码点（超出范围或落入代理区）",
                        "invalid unicode code point (out of range or surrogate)");
                return -1;
            }
            n = encodeUtf8(v, out);
            break;
        }

        default: {
            char cn[128], en[128];
            snprintf(cn, sizeof(cn), "非法字符串转义 '\\%c'", (char)c);
            snprintf(en, sizeof(en), "invalid string escape '\\%c'", (char)c);
            esc_err(err, sizeof(lmEscErr), cn, en);
            return -1;
        }
    }

    *pp = p;
    *outN = n;
    return 0;
}

/* 对最终结果做字符字面量校验：必须恰为 1 字节 */
static int validate_char_result(int total, lmEscErr err) {
    if(total == 1) return 0;
    esc_err(err, sizeof(lmEscErr),
            "字符字面量必须恰为 1 个字符",
            "char literal must contain exactly one character");
    return -1;
}

int lm_lex_unescape_one(const char** pp, const char* end, int quoteKind,
                        char* out, int* outN,
                        char* errBuf, int errBufSize) {
    lmEscErr localErr;
    char* err = (errBuf && errBufSize > 0) ? errBuf : (char*)localErr;
    int rc = decode_one(pp, end, quoteKind, out, outN, err);
    if(rc != 0) return rc;
    if(quoteKind == LM_QUOTE_CHAR &&
       validate_char_result(*outN, err) != 0) {
        return -1;
    }
    return 0;
}

int lm_lex_unescape(const char* raw, int quoteKind,
                    char** out, int* outLen,
                    char* errBuf, int errBufSize) {
    if(!raw || !out || !outLen) return -1;
    lmEscErr localErr;
    char* err = (errBuf && errBufSize > 0) ? errBuf : (char*)localErr;

    size_t rawLen = strlen(raw);
    /* 解码后不会长于原文（UTF-8 扩展只来自 \u/\U 六/十字符 → 最多 4 字节，
       仍短于源序列），按源长度分配足够 */
    char* buf = (char*)malloc(rawLen + 1);
    if(!buf) {
        esc_err(err, errBufSize, "字符串解码内存不足",
                "out of memory while decoding string");
        return -1;
    }

    const char* p = raw;
    const char* end = raw + rawLen;
    int w = 0;
    while(p < end) {
        if(*p == '\\') {
            int add = 0;
            if(decode_one(&p, end, quoteKind, buf + w, &add, err) != 0) {
                free(buf);
                return -1;
            }
            w += add;
        } else {
            buf[w++] = *p++;
        }
    }
    buf[w] = '\0';

    if(quoteKind == LM_QUOTE_CHAR && validate_char_result(w, err) != 0) {
        free(buf);
        return -1;
    }

    *out = buf;
    *outLen = w;
    return 0;
}
