// lm_lex_unescape.h —— 字面量转义序列统一解码
//
// 为双引号字符串、反引号模板串、字符字面量提供同一套严格的转义规则：
//
//   \a \b \f \n \r \t \v \\ \" \'   单字符转义（\0 见八进制）
//   \nnn                            八进制 1-3 位，值 0-255（含单独 \0）
//   \xHH                            十六进制 1-2 位，值 0-255（至少 1 位）
//   \uHHHH                          十六进制 4 位 Unicode 码点（编码 UTF-8）
//   \UHHHHHHHH                      十六进制 8 位 Unicode 码点（编码 UTF-8）
//
// 任何无法识别、位数不足、数值越界或落入代理区（0xD800-0xDFFF）的序列
// 一律判为词法错误——严格失败，不静默丢弃反斜杠，以免悄悄改写正则、
// 路径等字符串语义。
//
// 字符字面量（LM_QUOTE_CHAR）只接受最终恰为 1 字节的结果，
// 不支持 \u/\U（多字节）。
#ifndef LM_LEX_UNESCAPE_H
#define LM_LEX_UNESCAPE_H

/* 引号类型 */
enum {
    LM_QUOTE_STR = 0,   /* 双引号字符串 "..." */
    LM_QUOTE_MSTR,      /* 反引号模板串 `...` */
    LM_QUOTE_CHAR       /* 字符字面量 'x' */
};

/*
 * 将已去掉外层引号的原始内容 raw 整体解码。
 * 成功返回 0：*out 为新 malloc 的缓冲（调用方 free），*outLen 为字节
 * 长度（不含结尾 '\0'）。
 * 失败返回非 0：双语错误正文写入 errBuf（建议容量 >= 192）。
 */
int lm_lex_unescape(const char* raw, int quoteKind,
                    char** out, int* outLen,
                    char* errBuf, int errBufSize);

/*
 * 流式解码单个转义序列：入口 *pp 指向起始反斜杠，消费该序列，
 * 向 out 写入 0-4 个字节并由 *outN 给出字节数；end 为输入尾（不含）。
 * 供模板字面文本逐序列追加使用。成功返回 0，失败语义同整体解码。
 */
int lm_lex_unescape_one(const char** pp, const char* end, int quoteKind,
                        char* out, int* outN,
                        char* errBuf, int errBufSize);

#endif
