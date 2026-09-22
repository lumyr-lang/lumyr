// lm_container.h —— 容器与数值扩展类型（tuple/set/bytes/complex）
#ifndef LM_CONTAINER_H
#define LM_CONTAINER_H

#include "lm_value.h"

// ===== tuple（VAL_TUPLE）：不可变固定长度异构序列 =====
// 构造：tuple(1, "x", 3.14) → 新 tuple；items 拷贝输入
Value lumyr_tuple_make(int argc, const Value* args);
// 长度
int   lumyr_tuple_len(Value v);
// 下标访问（越界返回 VAL_NONE）
Value lumyr_tuple_get(Value v, int idx);
// 值相等（逐元素 lumyr_eq，长度不等直接 false）
_Bool lumyr_tuple_eq(Value a, Value b);
// 转字符串（malloc，调用方 free）：(e1, e2, ...)
char* lumyr_tuple_to_str(Value v);

// ===== set（VAL_SET）：无序唯一元素集合 =====
// 构造：set(1, 2, 3) / set([1,2,3]) → 新 set（去重）
Value lumyr_set_make(int argc, const Value* args);
// 元素数
int   lumyr_set_len(Value v);
// 包含判断
_Bool lumyr_set_has(Value v, Value elem);
// 添加（原地，返回自身）
Value lumyr_set_add(Value* v, Value elem);
// 删除（原地，返回自身）
Value lumyr_set_remove(Value* v, Value elem);
// 并集 → 新 set
Value lumyr_set_union(Value a, Value b);
// 交集 → 新 set
Value lumyr_set_intersect(Value a, Value b);
// 差集 a-b → 新 set
Value lumyr_set_diff(Value a, Value b);
// 转数组（keys）
Value lumyr_set_to_array(Value v);
// 转字符串（malloc，调用方 free）：{e1, e2, ...}
char* lumyr_set_to_str(Value v);
// 值相等（元素个数相同且互相包含）
_Bool lumyr_set_eq(Value a, Value b);

// ===== bytes（VAL_BYTES）：不可变字节串 =====
// 构造：bytes("hello") / bytes([0x48,0x65]) → 新 bytes
Value lumyr_bytes_make(int argc, const Value* args);
// 从十六进制字符串构造（"48656c6c6f" → bytes）
Value lumyr_bytes_from_hex(const char* hex);
// 长度
int   lumyr_bytes_len(Value v);
// 下标访问（越界返回 VAL_NONE）
Value lumyr_bytes_get(Value v, int idx);
// 转字符串（malloc，调用方 free）：b"..."
char* lumyr_bytes_to_str(Value v);
// 转十六进制字符串（malloc，调用方 free）："48656c6c6f"
char* lumyr_bytes_hex(Value v);
// 解码为 UTF-8 字符串（malloc，调用方 free）
char* lumyr_bytes_to_utf8(Value v);

// ===== complex（VAL_COMPLEX）：复数 real+imag double =====
// 构造：complex(1.0, 2.0)
Value lumyr_complex_make(double real, double imag);
// 属性访问：real/imag
double lumyr_complex_real(Value v);
double lumyr_complex_imag(Value v);
// 模 |c|
double lumyr_complex_abs(Value v);
// 共轭 → 新 complex
Value lumyr_complex_conjugate(Value v);
// 转字符串（malloc，调用方 free）："(re+imj)" 或 "(re-imj)"
char* lumyr_complex_to_str(Value v);
// 算术：complex + - * （除法另处理）
Value lumyr_complex_add(Value a, Value b);
Value lumyr_complex_sub(Value a, Value b);
Value lumyr_complex_mul(Value a, Value b);

#endif // LM_CONTAINER_H
