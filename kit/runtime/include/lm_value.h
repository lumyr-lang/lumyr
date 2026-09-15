// lm_value.h —— 值系统核心（构造/算术/比较/下标/强转）+ 子模块头聚合
// 职责拆分：字符串 → lm_string.h、数组 → lm_array.h、数学 → lm_math.h、
//          文件 IO → lm_io.h、字典 → lm_map.h
#ifndef LM_VALUE_H
#define LM_VALUE_H

#include "lumyr_value_type.h"
#include "lumyr_value.h"

// 构造
Value lumyr_make_int(long long i);
Value lumyr_make_double(double d);
Value lumyr_make_bool(_Bool b);
Value lumyr_make_string(const char* s);
Value lumyr_make_char(char ch);
Value lumyr_make_byte(unsigned char b);   // byte：8 位无符号整数（0-255）
Value lumyr_make_int8(int8_t i8);          // int8：8 位有符号整数（-128 到 127）
Value lumyr_make_int16(int16_t i16);        // int16：16 位有符号整数（-32768 到 32767）
Value lumyr_make_int32(int32_t i32);        // int32：32 位有符号整数（-2147483648 到 2147483647）
Value lumyr_make_int64(int64_t i64);        // int64：64 位有符号整数（-9223372036854775808 到 9223372036854775807）
Value lumyr_make_uint8(uint8_t u8);          // uint8：8 位无符号整数（0 到 255）
Value lumyr_make_struct_ptr(void* ptr);    // C结构体指针：零拷贝传递，类型由外部标识

// 算术
Value lumyr_add(Value a, Value b);
Value lumyr_sub(Value a, Value b);
Value lumyr_mul(Value a, Value b);
Value lumyr_div(Value a, Value b);
Value lumyr_mod(Value a, Value b);
Value lumyr_unary_plus(Value v);
Value lumyr_unary_minus(Value v);

// 比较
Value lumyr_gt(Value a, Value b);
Value lumyr_lt(Value a, Value b);
Value lumyr_ge(Value a, Value b);
Value lumyr_le(Value a, Value b);
Value lumyr_eq(Value a, Value b);
Value lumyr_ne(Value a, Value b);

_Bool lumyr_to_bool(Value v);
Value lumyr_logic_not(Value v);

// ===== 数组 =====
Value lumyr_array_get(Value arr, Value idx);
Value lumyr_array_set(Value arr, Value idx, Value val);
void lumyr_check_mapname_ro(Value arr, Value idx, const char* op); // 只读 __mapname__ 拦截
Value lumyr_len(Value v);           // 数组/字符串/字典长度
Value lumyr_index_get(Value c, Value idx);  // 数组元素 / 字符串字符 / 字典键

// ===== 内置函数（核心） =====
Value lumyr_type(Value v);          // type(x)：类型名字符串
Value lumyr_input(void);            // input()：读一行（去换行）

// ===== 错误机制全局（定义在 lm_value.c；动态扩容，无硬上限） =====
extern _Thread_local char* g_err_type;
extern _Thread_local const char** g_trace;
extern _Thread_local int g_trace_n;
extern _Thread_local char* g_err_msg;

void __g_ensure(int need);
void g_trace_push(const char* nm);
void g_err_msg_set(const char* s);
void g_err_type_set(const char* s);

// ===== 错误对象 =====
Value lumyr_make_error(const char* type, const char* msg, const char* stack);
char* lumyr_build_stack_trace(void);   // malloc，调用方 free

// ===== 强转 =====
Value lumyr_cast_int(Value v);
Value lumyr_cast_double(Value v);
Value lumyr_cast_bool(Value v);
Value lumyr_cast_string(Value v);
Value lumyr_cast_char(Value v);
Value lumyr_cast_ascii(Value v);
Value lumyr_cast_byte(Value v);
// 固定宽度整数强转（返回 VAL_INT，值做 C 风格截断）
Value lumyr_cast_int8(Value v);
Value lumyr_cast_int16(Value v);
Value lumyr_cast_int32(Value v);
Value lumyr_cast_int64(Value v);
Value lumyr_cast_uint8(Value v);
Value lumyr_cast_uint16(Value v);
Value lumyr_cast_uint32(Value v);
Value lumyr_cast_uint64(Value v);
Value lumyr_cast_long(Value v);
Value lumyr_cast_longlong(Value v);
Value lumyr_cast_float(Value v);

int lumyr_extract_int(Value v);

void lumyr_print(Value v);
void lumyr_print_inline(Value v);  /* 打印单个值不换行，用于多参数 print */

// ===== 跨文件共享辅助（拆分后子模块依赖） =====
double value_as_number(Value x);    // 值转数值（数值/布尔/字符）
char*  value_to_str(Value v);       // 值转字符串（malloc，调用方 free）
long long array_index_of(Value idx); // 下标值转 long long
long long range_to_ll(Value v);       // range 参数转 long long

// ===== 子模块头（职责拆分） =====
#include "lm_string.h"
#include "lm_array.h"
#include "lm_math.h"
#include "lm_io.h"
#include "lm_map.h"

#endif //LM_VALUE_H
