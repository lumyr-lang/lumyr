// lumyr-lang 字节码 IR 定义
// 基于栈的 VM：表达式求值压栈，跳转指令用绝对 pc 目标。
#ifndef LUMYR_IR_BYTECODE_H
#define LUMYR_IR_BYTECODE_H

#include "lumyr_value_type.h"
#include "lumyr_value.h"

typedef enum {
    OPC_NOP,
    OPC_LOAD_CONST,     // a=常量池下标
    OPC_GETFUNC,        // a=函数名符号下标：压入函数值
    OPC_LOAD_VAR,       // a=符号表下标
    OPC_LOAD_INT_VAR,   // a=符号表下标；加载声明为 int 的变量，直接压入 int 栈（零检查零转换）
    OPC_STORE_INT_VAR,  // a=符号表下标；从 int 栈弹出 int 值，直接存储到变量的 int_vals（零包装零转换）
    OPC_PUSH_INT_CONST, // a=常量值；把 int 常量直接压入 int 栈（零检查零转换，用于 <int>42 字面量赋值）
    OPC_PUSH_UINT_CONST, // a=常量值；把 uint 常量直接压入 uint 栈（零检查零转换，用于 <uint>42 字面量赋值）
    OPC_INT_ADD,        // 从 int 栈弹出两个 int，相加，结果压回 int 栈（零检查零转换零 Value 开销）
    OPC_INT_SUB,        // 从 int 栈弹出两个 int，相减，结果压回 int 栈（零检查零转换零 Value 开销）
    OPC_INT_MUL,        // 从 int 栈弹出两个 int，相乘，结果压回 int 栈（零检查零转换零 Value 开销）
    OPC_INT_DIV,        // 从 int 栈弹出两个 int，相除，结果压回 int 栈（零检查零转换零 Value 开销）
    OPC_INT_MOD,        // 从 int 栈弹出两个 int，取模，结果压回 int 栈（零检查零转换零 Value 开销）
    OPC_INT_TO_VALUE,   // 从 int 栈弹出一个 int，包装成 Value，压入 Value 栈（用于兼容赋值等通用逻辑）
    OPC_INT_GT,         // 从 int 栈弹出两个 int，大于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_INT_LT,         // 从 int 栈弹出两个 int，小于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_INT_GE,         // 从 int 栈弹出两个 int，大于等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_INT_LE,         // 从 int 栈弹出两个 int，小于等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_INT_EQ,         // 从 int 栈弹出两个 int，等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_INT_NE,         // 从 int 栈弹出两个 int，不等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_INT_ARRAY_SET,  // 从 Value 栈弹出数组和索引，从 int 栈弹出值，写入 int 类型化数组（零转换）
    OPC_LOAD_VAR_REF,   // a=符号表下标；加载 ref 参数（struct 不转 Map，直接传递 VAL_STRUCT_PTR）
    OPC_STORE_VAR,      // a=符号表下标；弹值写变量（深拷贝入帧），原值压回（表达式值）
    OPC_ADD, OPC_SUB, OPC_MUL, OPC_DIV, OPC_MOD,
    OPC_GT, OPC_LT, OPC_GE, OPC_LE, OPC_EQ, OPC_NE, OPC_IMPLEMENTS,
    OPC_NEG, OPC_POS,
    OPC_LOGIC_NOT,   // 弹1压1 bool 取反
    OPC_PRE_INC, OPC_POST_INC, OPC_PRE_DEC, OPC_POST_DEC,  // a=符号表下标
    OPC_CAST_INT, OPC_CAST_DOUBLE, OPC_CAST_CHAR, OPC_CAST_BOOL, OPC_CAST_STRING, OPC_CAST_ASCII, OPC_CAST_BYTE,
    OPC_CAST_INT8, OPC_CAST_INT16, OPC_CAST_INT32, OPC_CAST_INT64,
    OPC_CAST_UINT8, OPC_CAST_UINT16, OPC_CAST_UINT32, OPC_CAST_UINT64,
    OPC_CAST_LONG, OPC_CAST_LONGLONG, OPC_CAST_FLOAT,
    OPC_ARRAY_LIT,    // b=元素个数；弹 b 个元素压数组
    OPC_INT_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 int，创建 int 泛型数组
    OPC_INT_ARRAY_GET, // 弹 arr,idx；直接从 int 类型化数组读取元素，压入 int 栈（零包装零 Value 开销）
    OPC_LOAD_DOUBLE_VAR,   // a=符号表下标；加载声明为 double 的变量，直接压入 double 栈（零检查零转换）
    OPC_STORE_DOUBLE_VAR,  // a=符号表下标；从 double 栈弹出 double 值，直接存储到变量的 double_vals（零包装零转换）
    OPC_DOUBLE_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 double，创建 double 泛型数组
    OPC_DOUBLE_ARRAY_GET, // 弹 arr,idx；直接从 double 类型化数组读取元素，压入 double 栈（零包装零 Value 开销）
    OPC_PUSH_DOUBLE_CONST, // a=常量值索引；把 double 常量直接压入 double 栈（零检查零转换，用于 <double>3.14 字面量赋值）
    OPC_DOUBLE_ADD,        // 从 double 栈弹出两个 double，相加，结果压回 double 栈（零检查零转换零 Value 开销）
    OPC_DOUBLE_SUB,        // 从 double 栈弹出两个 double，相减，结果压回 double 栈（零检查零转换零 Value 开销）
    OPC_DOUBLE_MUL,        // 从 double 栈弹出两个 double，相乘，结果压回 double 栈（零检查零转换零 Value 开销）
    OPC_DOUBLE_DIV,        // 从 double 栈弹出两个 double，相除，结果压回 double 栈（零检查零转换零 Value 开销）
    OPC_DOUBLE_TO_VALUE,   // 从 double 栈弹出一个 double，包装成 Value，压入 Value 栈（用于兼容赋值等通用逻辑）
    OPC_DOUBLE_GT,         // 从 double 栈弹出两个 double，大于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_DOUBLE_LT,         // 从 double 栈弹出两个 double，小于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_DOUBLE_GE,         // 从 double 栈弹出两个 double，大于等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_DOUBLE_LE,         // 从 double 栈弹出两个 double，小于等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_DOUBLE_EQ,         // 从 double 栈弹出两个 double，等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_DOUBLE_NE,         // 从 double 栈弹出两个 double，不等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_DOUBLE_ARRAY_SET,  // 从 Value 栈弹出数组和索引，从 double 栈弹出值，写入 double 类型化数组（零转换）
    OPC_LOAD_FLOAT_VAR,   // a=符号表下标；加载声明为 float 的变量，直接压入 float 栈（零检查零转换）
    OPC_STORE_FLOAT_VAR,  // a=符号表下标；从 float 栈弹出 float 值，直接存储到变量的 float_vals（零包装零转换）
    OPC_FLOAT_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 float，创建 float 泛型数组
    OPC_FLOAT_ARRAY_GET, // 弹 arr,idx；直接从 float 类型化数组读取元素，压入 float 栈（零包装零 Value 开销）
    OPC_PUSH_FLOAT_CONST, // a=常量池下标；float 常量零开销压栈，直接压入 float 栈（不创建Value）
    OPC_FLOAT_ADD,        // 从 float 栈弹出两个 float，相加，结果压回 float 栈（零检查零转换零 Value 开销）
    OPC_FLOAT_SUB,        // 从 float 栈弹出两个 float，相减，结果压回 float 栈（零检查零转换零 Value 开销）
    OPC_FLOAT_MUL,        // 从 float 栈弹出两个 float，相乘，结果压回 float 栈（零检查零转换零 Value 开销）
    OPC_FLOAT_DIV,        // 从 float 栈弹出两个 float，相除，结果压回 float 栈（零检查零转换零 Value 开销）
    OPC_FLOAT_TO_VALUE,   // 从 float 栈弹出一个 float，包装成 Value，压入 Value 栈（用于兼容赋值等通用逻辑）
    OPC_FLOAT_GT,         // 从 float 栈弹出两个 float，大于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_FLOAT_LT,         // 从 float 栈弹出两个 float，小于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_FLOAT_GE,         // 从 float 栈弹出两个 float，大于等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_FLOAT_LE,         // 从 float 栈弹出两个 float，小于等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_FLOAT_EQ,         // 从 float 栈弹出两个 float，等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_FLOAT_NE,         // 从 float 栈弹出两个 float，不等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_FLOAT_ARRAY_SET,  // 从 Value 栈弹出数组和索引，从 float 栈弹出值，写入 float 类型化数组（零转换）
    OPC_LOAD_UINT_VAR,   // a=符号表下标；加载声明为 uint 的变量，直接压入 uint 栈（零检查零转换）
    OPC_STORE_UINT_VAR,  // a=符号表下标；从 uint 栈弹出 uint 值，直接存储到变量的 uint_vals（零包装零转换）
    OPC_UINT_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 uint，创建 uint 泛型数组
    OPC_UINT_ARRAY_GET, // 弹 arr,idx；直接从 uint 类型化数组读取元素，压入 uint 栈（零包装零 Value 开销）
    OPC_UINT_ADD,        // 从 uint 栈弹出两个 uint，相加，结果压回 uint 栈（零检查零转换零 Value 开销）
    OPC_UINT_SUB,        // 从 uint 栈弹出两个 uint，相减，结果压回 uint 栈（零检查零转换零 Value 开销）
    OPC_UINT_MUL,        // 从 uint 栈弹出两个 uint，相乘，结果压回 uint 栈（零检查零转换零 Value 开销）
    OPC_UINT_DIV,        // 从 uint 栈弹出两个 uint，相除，结果压回 uint 栈（零检查零转换零 Value 开销）
    OPC_UINT_MOD,        // 从 uint 栈弹出两个 uint，取模，结果压回 uint 栈（零检查零转换零 Value 开销）
    OPC_UINT_TO_VALUE,   // 从 uint 栈弹出一个 uint，包装成 Value，压入 Value 栈（用于兼容赋值等通用逻辑）
    OPC_UINT_GT,         // 从 uint 栈弹出两个 uint，大于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_UINT_LT,         // 从 uint 栈弹出两个 uint，小于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_UINT_GE,         // 从 uint 栈弹出两个 uint，大于等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_UINT_LE,         // 从 uint 栈弹出两个 uint，小于等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_UINT_EQ,         // 从 uint 栈弹出两个 uint，等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_UINT_NE,         // 从 uint 栈弹出两个 uint，不等于比较，结果(bool)压入 Value 栈（零检查零转换）
    OPC_UINT_ARRAY_SET,  // 从 Value 栈弹出数组和索引，从 uint 栈弹出值，写入 uint 类型化数组（零转换）
    // 类型转换指令（专用栈之间的转换，零包装零Value开销）
    OPC_INT_TO_FLOAT,    // 从 int 栈弹出一个 int，转换为 float，压入 float 栈（零包装零Value开销）
    OPC_INT_TO_DOUBLE,   // 从 int 栈弹出一个 int，转换为 double，压入 double 栈（零包装零Value开销）
    OPC_UINT_TO_FLOAT,   // 从 uint 栈弹出一个 uint，转换为 float，压入 float 栈（零包装零Value开销）
    OPC_UINT_TO_DOUBLE,  // 从 uint 栈弹出一个 uint，转换为 double，压入 double 栈（零包装零Value开销）
    OPC_FLOAT_TO_DOUBLE, // 从 float 栈弹出一个 float，转换为 double，压入 double 栈（零包装零Value开销）
    // long long 类型专用指令（零检查零转换零 Value 开销）
    OPC_PUSH_LONG_LONG_CONST, // a=常量值；压入 long long 栈（零包装零Value开销）
    OPC_LOAD_LONG_LONG_VAR,   // a=符号表下标；加载声明为 long long 的变量，直接压入 long long 栈（零检查零转换）
    OPC_STORE_LONG_LONG_VAR,  // a=符号表下标；从 long long 栈弹出 long long 值，直接存储到变量（零包装零转换）
    OPC_LONG_LONG_ADD,        // 从 long long 栈弹出两个 long long，加法，结果压入 long long 栈（零检查零转换）
    OPC_LONG_LONG_SUB,        // 从 long long 栈弹出两个 long long，减法，结果压入 long long 栈
    OPC_LONG_LONG_MUL,        // 从 long long 栈弹出两个 long long，乘法，结果压入 long long 栈
    OPC_LONG_LONG_DIV,        // 从 long long 栈弹出两个 long long，除法，结果压入 long long 栈
    OPC_LONG_LONG_MOD,        // 从 long long 栈弹出两个 long long，取模，结果压入 long long 栈
    OPC_LONG_LONG_TO_VALUE,   // 从 long long 栈弹出一个 long long，包装成 Value，压入 Value 栈（用于兼容）
    OPC_LONG_LONG_GT,         // 从 long long 栈弹出两个 long long，大于比较，结果(bool)压入 Value 栈
    OPC_LONG_LONG_LT,         // 从 long long 栈弹出两个 long long，小于比较，结果(bool)压入 Value 栈
    OPC_LONG_LONG_GE,         // 从 long long 栈弹出两个 long long，大于等于比较，结果(bool)压入 Value 栈
    OPC_LONG_LONG_LE,         // 从 long long 栈弹出两个 long long，小于等于比较，结果(bool)压入 Value 栈
    OPC_LONG_LONG_EQ,         // 从 long long 栈弹出两个 long long，等于比较，结果(bool)压入 Value 栈
    OPC_LONG_LONG_NE,         // 从 long long 栈弹出两个 long long，不等于比较，结果(bool)压入 Value 栈
    OPC_LONG_LONG_ARRAY_SET,  // 从 Value 栈弹出数组和索引，从 long long 栈弹出值，写入 long long 类型化数组
    OPC_LONG_LONG_ARRAY_LIT,  // b=元素个数；弹 b 个元素，创建 long long 类型化数组
    OPC_LONG_LONG_ARRAY_GET,  // 弹 arr,idx；直接从 long long 类型化数组读取元素，压入 long long 栈
    OPC_PRINT_LONG_LONG,      // 从 long long 栈弹出 long long 值并打印（零包装零Value开销）
    OPC_LOAD_BOOL_VAR,   // a=符号表下标；加载声明为 bool 的变量，直接压入 bool 栈（零检查零转换）
    OPC_STORE_BOOL_VAR,  // a=符号表下标；从 bool 栈弹出 bool 值，直接存储到变量的 bool_vals（零包装零转换）
    OPC_BOOL_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 bool，创建 bool 泛型数组
    OPC_BOOL_ARRAY_GET, // 弹 arr,idx；直接从 bool 类型化数组读取元素，压入 bool 栈（零包装零 Value 开销）
    OPC_LOAD_CHAR_VAR,   // a=符号表下标；加载声明为 char 的变量，直接压入 char 栈（零检查零转换）
    OPC_STORE_CHAR_VAR,  // a=符号表下标；从 char 栈弹出 char 值，直接存储到变量的 char_vals（零包装零转换）
    OPC_CHAR_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 char，创建 char 泛型数组
    OPC_CHAR_ARRAY_GET, // 弹 arr,idx；直接从 char 类型化数组读取元素，压入 char 栈（零包装零 Value 开销）
    OPC_LOAD_BYTE_VAR,   // a=符号表下标；加载声明为 byte 的变量，直接压入 byte 栈（零检查零转换）
    OPC_STORE_BYTE_VAR,  // a=符号表下标；从 byte 栈弹出 byte 值，直接存储到变量的 byte_vals（零包装零转换）
    OPC_BYTE_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 byte，创建 byte 泛型数组
    OPC_BYTE_ARRAY_GET, // 弹 arr,idx；直接从 byte 类型化数组读取元素，压入 byte 栈（零包装零 Value 开销）
    OPC_MAP_LIT,      // b=键值对个数；弹 2b 个值（键、值交替）压字典
    OPC_INDEX_GET,    // 弹 arr,idx 压元素（数组元素 / 字符串字符）
    OPC_INDEX_SET,    // 弹 arr,idx,val 写回；压回 val（表达式值）
    OPC_LOAD_FIELD,   // a=变量符号下标，b=字段名常量下标；直接加载 struct 字段（零开销）
    OPC_STORE_FIELD,  // a=变量符号下标，b=字段名常量下标；弹值写入 struct 字段，压回值（零开销）
    OPC_STORE_NESTED_FIELD, // a=变量符号下标，b=组合字段名常量下标（如 "top_left.x"）；弹值写入嵌套 struct 字段
    OPC_LOAD_STRUCT_PTR,   // a=变量索引；加载 struct 变量的指针（用于方法 self 参数）
    OPC_BUILTIN,      // a=内置函数 ID，b=实参个数（见 BuiltinId）
    OPC_PRINT,        // 打印栈顶，不弹出
    OPC_PRINT_INT,    // 从 int 栈弹出并打印（零开销，用于声明为 int 的变量）
    OPC_PRINT_DOUBLE, // 从 double 栈弹出并打印（零开销，用于声明为 double 的变量）
    OPC_PRINT_FLOAT,  // 从 float 栈弹出并打印（零开销，用于声明为 float 的变量）
    OPC_PRINT_UINT,   // 从 uint 栈弹出并打印（零开销，用于声明为 uint 的变量）
    OPC_PRINT_BOOL,   // 从 bool 栈弹出并打印（零开销，用于声明为 bool 的变量）
    OPC_PRINT_CHAR,   // 从 char 栈弹出并打印（零开销，用于声明为 char 的变量）
    OPC_PRINT_BYTE,   // 从 byte 栈弹出并打印（零开销，用于声明为 byte 的变量）
    OPC_LOAD_INT8_VAR,   // a=符号表下标；加载声明为 int8 的变量，直接压入 int8 栈（零检查零转换）
    OPC_STORE_INT8_VAR,  // a=符号表下标；从 int8 栈弹出 int8 值，直接存储到变量的 int8_vals（零包装零转换）
    OPC_INT8_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 int8，创建 int8 泛型数组
    OPC_INT8_ARRAY_GET, // 弹 arr,idx；直接从 int8 类型化数组读取元素，压入 int8 栈（零包装零 Value 开销）
    OPC_PRINT_INT8,     // 从 int8 栈弹出并打印（零开销，用于声明为 int8 的变量）
    OPC_LOAD_INT16_VAR,   // a=符号表下标；加载声明为 int16 的变量，直接压入 int16 栈（零检查零转换）
    OPC_STORE_INT16_VAR,  // a=符号表下标；从 int16 栈弹出 int16 值，直接存储到变量的 int16_vals（零包装零转换）
    OPC_INT16_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 int16，创建 int16 泛型数组
    OPC_INT16_ARRAY_GET, // 弹 arr,idx；直接从 int16 类型化数组读取元素，压入 int16 栈（零包装零 Value 开销）
    OPC_PRINT_INT16,     // 从 int16 栈弹出并打印（零开销，用于声明为 int16 的变量）
    OPC_LOAD_INT32_VAR,   // a=符号表下标；加载声明为 int32 的变量，直接压入 int32 栈（零检查零转换）
    OPC_STORE_INT32_VAR,  // a=符号表下标；从 int32 栈弹出 int32 值，直接存储到变量的 int32_vals（零包装零转换）
    OPC_INT32_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 int32，创建 int32 泛型数组
    OPC_INT32_ARRAY_GET, // 弹 arr,idx；直接从 int32 类型化数组读取元素，压入 int32 栈（零包装零 Value 开销）
    OPC_PRINT_INT32,     // 从 int32 栈弹出并打印（零开销，用于声明为 int32 的变量）
    OPC_LOAD_INT64_VAR,   // a=符号表下标；加载声明为 int64 的变量，直接压入 int64 栈（零检查零转换）
    OPC_STORE_INT64_VAR,  // a=符号表下标；从 int64 栈弹出 int64 值，直接存储到变量的 int64_vals（零包装零转换）
    OPC_INT64_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 int64，创建 int64 泛型数组
    OPC_INT64_ARRAY_GET, // 弹 arr,idx；直接从 int64 类型化数组读取元素，压入 int64 栈（零包装零 Value 开销）
    OPC_PRINT_INT64,     // 从 int64 栈弹出并打印（零开销，用于声明为 int64 的变量）
    OPC_LOAD_UINT8_VAR,   // a=符号表下标；加载声明为 uint8 的变量，直接压入 uint8 栈（零检查零转换）
    OPC_STORE_UINT8_VAR,  // a=符号表下标；从 uint8 栈弹出 uint8 值，直接存储到变量的 uint8_vals（零包装零转换）
    OPC_UINT8_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 uint8，创建 uint8 泛型数组
    OPC_UINT8_ARRAY_GET, // 弹 arr,idx；直接从 uint8 类型化数组读取元素，压入 uint8 栈（零包装零 Value 开销）
    OPC_PRINT_UINT8,     // 从 uint8 栈弹出并打印（零开销，用于声明为 uint8 的变量）
    OPC_LOAD_UINT16_VAR,   // a=符号表下标；加载声明为 uint16 的变量，直接压入 uint16 栈（零检查零转换）
    OPC_STORE_UINT16_VAR,  // a=符号表下标；从 uint16 栈弹出 uint16 值，直接存储到变量的 uint16_vals（零包装零转换）
    OPC_UINT16_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 uint16，创建 uint16 泛型数组
    OPC_UINT16_ARRAY_GET, // 弹 arr,idx；直接从 uint16 类型化数组读取元素，压入 uint16 栈（零包装零 Value 开销）
    OPC_PRINT_UINT16,     // 从 uint16 栈弹出并打印（零开销，用于声明为 uint16 的变量）
    OPC_LOAD_UINT32_VAR,   // a=符号表下标；加载声明为 uint32 的变量，直接压入 uint32 栈（零检查零转换）
    OPC_STORE_UINT32_VAR,  // a=符号表下标；从 uint32 栈弹出 uint32 值，直接存储到变量的 uint32_vals（零包装零转换）
    OPC_UINT32_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 uint32，创建 uint32 泛型数组
    OPC_UINT32_ARRAY_GET, // 弹 arr,idx；直接从 uint32 类型化数组读取元素，压入 uint32 栈（零包装零 Value 开销）
    OPC_PRINT_UINT32,     // 从 uint32 栈弹出并打印（零开销，用于声明为 uint32 的变量）
    OPC_LOAD_UINT64_VAR,   // a=符号表下标；加载声明为 uint64 的变量，直接压入 uint64 栈（零检查零转换）
    OPC_STORE_UINT64_VAR,  // a=符号表下标；从 uint64 栈弹出 uint64 值，直接存储到变量的 uint64_vals（零包装零转换）
    OPC_UINT64_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 uint64，创建 uint64 泛型数组
    OPC_UINT64_ARRAY_GET, // 弹 arr,idx；直接从 uint64 类型化数组读取元素，压入 uint64 栈（零包装零 Value 开销）
    OPC_PRINT_UINT64,     // 从 uint64 栈弹出并打印（零开销，用于声明为 uint64 的变量）
    OPC_LOAD_LONG_VAR,   // a=符号表下标；加载声明为 long 的变量，直接压入 long 栈（零检查零转换）
    OPC_STORE_LONG_VAR,  // a=符号表下标；从 long 栈弹出 long 值，直接存储到变量的 long_vals（零包装零转换）
    OPC_LONG_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 long，创建 long 泛型数组
    OPC_LONG_ARRAY_GET, // 弹 arr,idx；直接从 long 类型化数组读取元素，压入 long 栈（零包装零 Value 开销）
    OPC_PRINT_LONG,     // 从 long 栈弹出并打印（零开销，用于声明为 long 的变量）
    OPC_LOAD_ULONG_VAR,   // a=符号表下标；加载声明为 ulong 的变量，直接压入 ulong 栈（零检查零转换）
    OPC_STORE_ULONG_VAR,  // a=符号表下标；从 ulong 栈弹出 ulong 值，直接存储到变量的 ulong_vals（零包装零转换）
    OPC_ULONG_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 ulong，创建 ulong 泛型数组
    OPC_ULONG_ARRAY_GET, // 弹 arr,idx；直接从 ulong 类型化数组读取元素，压入 ulong 栈（零包装零 Value 开销）
    OPC_PRINT_ULONG,     // 从 ulong 栈弹出并打印（零开销，用于声明为 ulong 的变量）
    OPC_LOAD_SIZE_T_VAR,   // a=符号表下标；加载声明为 size_t 的变量，直接压入 size_t 栈（零检查零转换）
    OPC_STORE_SIZE_T_VAR,  // a=符号表下标；从 size_t 栈弹出 size_t 值，直接存储到变量的 size_t_vals（零包装零转换）
    OPC_SIZE_T_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 size_t，创建 size_t 泛型数组
    OPC_SIZE_T_ARRAY_GET, // 弹 arr,idx；直接从 size_t 类型化数组读取元素，压入 size_t 栈（零包装零 Value 开销）
    OPC_PRINT_SIZE_T,     // 从 size_t 栈弹出并打印（零开销，用于声明为 size_t 的变量）
    OPC_LOAD_SSIZE_T_VAR,   // a=符号表下标；加载声明为 ssize_t 的变量，直接压入 ssize_t 栈（零检查零转换）
    OPC_STORE_SSIZE_T_VAR,  // a=符号表下标；从 ssize_t 栈弹出 ssize_t 值，直接存储到变量的 ssize_t_vals（零包装零转换）
    OPC_SSIZE_T_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 ssize_t，创建 ssize_t 泛型数组
    OPC_SSIZE_T_ARRAY_GET, // 弹 arr,idx；直接从 ssize_t 类型化数组读取元素，压入 ssize_t 栈（零包装零 Value 开销）
    OPC_PRINT_SSIZE_T,     // 从 ssize_t 栈弹出并打印（零开销，用于声明为 ssize_t 的变量）
    OPC_LOAD_LONG_DOUBLE_VAR,   // a=符号表下标；加载声明为 long double 的变量，直接压入 long double 栈（零检查零转换）
    OPC_STORE_LONG_DOUBLE_VAR,  // a=符号表下标；从 long double 栈弹出 long double 值，直接存储到变量的 long_double_vals（零包装零转换）
    OPC_LONG_DOUBLE_ARRAY_LIT, // b=元素个数；弹 b 个 Value 元素，内联转换为 long double，创建 long double 泛型数组
    OPC_LONG_DOUBLE_ARRAY_GET, // 弹 arr,idx；直接从 long double 类型化数组读取元素，压入 long double 栈（零包装零 Value 开销）
    OPC_PRINT_LONG_DOUBLE,     // 从 long double 栈弹出并打印（零开销，用于声明为 long double 的变量）
    OPC_TO_BOOL,      // 弹1压1 bool
    OPC_DUP,          // 复制栈顶
    OPC_POP,          // 丢弃栈顶
    OPC_TRY,          // a=catch 起始pc(0=无catch)，b=finally 起始pc(0=无finally)；setjmp 注册错误处理器
    OPC_ENDTRY,       // a=跳转目标pc；正常路径恢复外层处理器（无 finally 的旧布局用）
    OPC_GET_ERR,      // 压入最近捕获的错误对象（type/message/stack）
    OPC_THROW,        // 弹1；包装成错误对象并抛出（无处理器则打印退出）
    OPC_FIN_PUSH,     // a=完成动作(1=JMP 2=RETHROW 3=BREAK 4=CONT)，b=目标pc；压入 finally 完成动作
    OPC_FINISH,       // 弹 finally 完成动作并执行（JMP/RETHROW/RETURN 恢复）
    OPC_PEND_RETURN,  // 弹1（返回值）→ 挂起返回动作，跳 b（finally 起始；0=直接返回）
    OPC_JMP,          // a=目标pc
    OPC_JMP_IF_FALSE, // a=目标pc；弹条件，假则跳
    OPC_JMP_IF_TRUE,  // a=目标pc；弹条件，真则跳
    OPC_JMP_IF_NULL,  // a=目标pc；弹值，为 VAL_NONE 则跳
    OPC_CLASS_NEW,    // a=class名符号下标：创建 C 结构体实例并包装成 Value
    OPC_CALL,         // a=函数名符号下标，b=实参个数
    OPC_CALLV,        // 动态调用链：栈顶下一位=函数值（b=实参个数），栈顶 b 个为实参
    OPC_MKCLOSURE,    // a=lambda 符号下标：沿当前帧装箱捕获变量，压入新闭包函数值
    OPC_RETURN,       // 弹值返回（深拷贝）
    OPC_RETURN_NIL,   // 无返回值返回
    OPC_YIELD,        // 生成器 yield：弹值，保存执行状态，返回给调用者
    OPC_HALT
} OpCode;

typedef struct {
    OpCode op;
    int a;
    int b;
} Instruction;

// 内置函数（OPC_BUILTIN 的 a 字段）
typedef enum {
    BUILTIN_LEN = 0,      // len(x)：数组/字符串长度
    BUILTIN_TYPE,         // type(x)：类型名
    BUILTIN_INPUT,        // input()：读一行
    BUILTIN_RANGE,        // range(n)：[0..n-1] 数组
    BUILTIN_SUBSTR,       // substr(s, start, n)
    BUILTIN_TOUPPER,      // toupper(s)
    BUILTIN_TOLOWER,      // tolower(s)
    BUILTIN_SPLIT,        // split(s, sep)
    BUILTIN_DEL,          // del(arr, idx)
    BUILTIN_INSERT,       // insert(arr, idx, val)
    BUILTIN_FLOOR,        // floor(x)
    BUILTIN_CEIL,         // ceil(x)
    BUILTIN_ABS,          // abs(x)
    BUILTIN_SQRT,         // sqrt(x)
    BUILTIN_MAX,          // max(a, b, ...) 变参
    BUILTIN_MIN,          // min(a, b, ...) 变参
    BUILTIN_JOIN,         // join(arr, sep)
    BUILTIN_CONTAINS,     // contains(s/arr, x)
    BUILTIN_REPEAT,       // repeat(s, n)
    BUILTIN_REPLACE,      // replace(s, from, to)
    BUILTIN_SUM,          // sum(arr)
    BUILTIN_AVG,          // avg(arr)
    BUILTIN_FORMAT,       // format(fmt, args...) 变参
    BUILTIN_SORT,         // sort(arr)
    BUILTIN_REVERSE,      // reverse(arr)
    BUILTIN_MAP,          // map(arr, fn) 高阶
    BUILTIN_FILTER,       // filter(arr, fn) 高阶
    BUILTIN_REDUCE,       // reduce(arr, fn, init) 高阶
    BUILTIN_STRIP,        // strip(s)
    BUILTIN_STARTSWITH,   // startswith(s, prefix)
    BUILTIN_ENDSWITH,     // endswith(s, suffix)
    BUILTIN_READ_FILE,    // read_file(path) → 文件内容
    BUILTIN_WRITE_FILE,   // write_file(path, content)
    BUILTIN_FILE_EXISTS,  // file_exists(path) → bool
    BUILTIN_KEYS,         // keys(d) → 键字符串数组
    BUILTIN_VALUES,       // values(d) → 值数组
    BUILTIN_THREAD,       // thread(f, args...) → 线程id（多线程）
    BUILTIN_THREAD_JOIN,  // thread_join(tid) → 等待线程并取返回值（join 已被字符串拼接占用）
    BUILTIN_MUTEX,        // mutex() → 互斥锁 id
    BUILTIN_RMUTEX,       // rmutex() → 递归互斥锁 id
    BUILTIN_RWLOCK,       // rwlock() → 读写锁 id
    BUILTIN_SPINLOCK,     // spinlock() → 自旋锁 id
    BUILTIN_LOCK,         // lock(id) → 阻塞加锁
    BUILTIN_UNLOCK,       // unlock(id) → 解锁
    BUILTIN_TRYLOCK,      // trylock(id) → bool（非阻塞尝试）
    BUILTIN_RDLOCK,       // rdlock(id) → 读锁（读写锁）
    BUILTIN_WRLOCK,       // wrlock(id) → 写锁（读写锁）
    BUILTIN_TRYRDLOCK,    // tryrdlock(id) → bool（读锁非阻塞尝试，仅读写锁）
    BUILTIN_TRYWRLOCK,    // trywrlock(id) → bool（写锁非阻塞尝试，仅读写锁）
    BUILTIN_CONDVAR,      // condvar() → 条件变量 id
    BUILTIN_COND_WAIT,    // cond_wait(cond, lock) → 原子释放锁并等待
    BUILTIN_COND_TIMEDWAIT, // cond_wait_timeout(cond, lock, ms) → bool（唤醒 true / 超时 false）
    BUILTIN_COND_SIGNAL,  // cond_signal(cond) → 唤醒一个等待者
    BUILTIN_COND_BROADCAST, // cond_broadcast(cond) → 唤醒全部等待者
    BUILTIN_THREADLOCAL_GET, // threadlocal_get(name) → 当前线程局部值
    BUILTIN_THREADLOCAL_SET, // threadlocal_set(name, value) → 写当前线程局部槽，返回 value
    BUILTIN_HTTP_GET,     // requests.get(url, params?, config?) → map{status,body,headers}
    BUILTIN_HTTP_POST,    // requests.post(url, params?, config?)
    BUILTIN_HTTP_PUT,     // requests.put(url, params?, config?)
    BUILTIN_HTTP_DELETE,  // requests.delete(url, params?, config?)
    BUILTIN_HTTP_HEAD,    // requests.head(url, params?, config?)
    BUILTIN_HTTP_PATCH,   // requests.patch(url, params?, config?)
    BUILTIN_JSON,         // json(s)：解析 JSON 文本 → 值
    BUILTIN_STRINGIFY,    // stringify(v)：值 → JSON 文本
    BUILTIN_ARRAY_ADD,    // add(arr, x)：追加元素，返回新数组（arr.add(x) 方法链）
    BUILTIN_ARRAY_REMOVE, // remove(arr, i)：删下标 i，返回新数组（arr.remove(i)）
    BUILTIN_ARRAY_CLEAR,  // clear(arr)：清空，返回空数组（arr.clear()）
    BUILTIN_ARRAY_INDEXOF,// indexOf(arr, x)：首个相等元素下标，-1 未找到
    BUILTIN_ARRAY_GET,    // arr_get(arr, i)：安全取（越界/非数组 → null）
    BUILTIN_ARRAY_SET,    // set(arr, i, v)：原地改，返回数组（arr.set(i,v) 链式）
    BUILTIN_ARRAY_FIRST,  // first(arr)：首元素（空 → null）
    BUILTIN_ARRAY_LAST,   // last(arr)：尾元素（空 → null）
    BUILTIN_MAP_HAS,      // has(m, k)：键是否存在（m.has(k) 方法链）
    BUILTIN_ARRAY_FLAT,   // flat(arr, depth?)：数组/字典扁平化（.flat() 方法链）
    BUILTIN_QS,           // qs(v)：字典/数组 → 查询字符串；字符串 → 解析为字典/数组
    BUILTIN_ARRAY_ADDALL, // addAll(a, b)：数组追加全部元素 / 字典合并全部键值
    BUILTIN_BYTES,        // bytes(s, enc?)：字符串 → 字节数组（按编码，默认 UTF-8）
    BUILTIN_STR,          // str(arr, enc?)：字节数组 → 字符串（按编码，默认 UTF-8）
    BUILTIN_ENCODE,       // encode(s, enc?)：字符串 → 字节数组（按编码，默认 UTF-8）
    BUILTIN_DECODE,       // decode(arr, enc?)：字节数组 → 字符串（按编码，默认 UTF-8）
    BUILTIN_ENCODE_URL,   // encodeURL(s)：URL 编码（高字节原样）
    BUILTIN_DECODE_URL,   // decodeURL(s)：URL 解码（%XX/+ → 原字符）
    BUILTIN_MD5,          // md5(s)：MD5 32 位十六进制小写
    BUILTIN_ENCODE_BASE64,  // encodeBase64(s)：Base64 编码
    BUILTIN_DECODE_BASE64,  // decodeBase64(s)：Base64 解码
    BUILTIN_REGEX_MATCH,    // regex_match(s, pattern)：完整匹配 → bool
    BUILTIN_REGEX_SEARCH,   // regex_search(s, pattern)：搜索 → [match, group1, ...]
    BUILTIN_REGEX_REPLACE,  // regex_replace(s, pattern, repl)：替换所有匹配（支持 \1 反向引用）
    BUILTIN_NOW,            // now()：当前时间 map
    BUILTIN_TIMESTAMP,      // timestamp()：Unix 秒（double）
    BUILTIN_TIMESTAMP_MS,   // timestamp_ms()：Unix 毫秒（int）
    BUILTIN_SLEEP,          // sleep(ms)：休眠毫秒
    BUILTIN_DATE,           // date()："2026-09-07"
    BUILTIN_TIME,           // time()："15:30:45"
    BUILTIN_DATETIME,       // datetime()："2026-09-07 15:30:45"
    BUILTIN_FORMAT_TIME,    // format_time(fmt, ts?)：strftime 格式化
    BUILTIN_LOG_DEBUG,      // debug(msg) / log.debug(msg)
    BUILTIN_LOG_INFO,       // info(msg) / log.info(msg)
    BUILTIN_LOG_WARN,       // warn(msg) / log.warn(msg)
    BUILTIN_LOG_ERROR,      // error(msg) / log.error(msg)
    BUILTIN_LOG_FATAL,      // fatal(msg) / log.fatal(msg)
    BUILTIN_GC_COUNT,       // gc_count()：当前 GC 管理对象数
    BUILTIN_GC_BYTES,       // gc_bytes()：当前 GC 管理字节数（近似）
    BUILTIN_GC_COLLECT,     // gc_collect()：手动触发一次 GC
    BUILTIN_GC_STW_NS,      // gc_stw_ns()：累计 STW 停顿时间（纳秒）
    BUILTIN_NEXT,            // next(gen)：恢复生成器执行，返回 yield 值；结束返回 null
    BUILTIN_SEND,            // send(gen, val)：向生成器发送值，返回下一个 yield 值
    BUILTIN_RECEIVE,         // receive()：在生成器中获取 send() 发送的值
    BUILTIN_CLOSE,           // close(gen)：关闭生成器
    BUILTIN_GEN_THROW,       // GenThrow(gen, err)：向生成器抛出异常，在 yield 位置抛出
    BUILTIN_CHAIN,           // chain(g1, g2)：连接两个生成器
    BUILTIN_ZIP,             // zip(g1, g2)：压缩两个生成器
    BUILTIN_SKIP,            // skip(g, n)：跳过前 n 个元素
    BUILTIN_TAKE,            // take(g, n)：取前 n 个元素
    BUILTIN_ENUMERATE,       // enumerate(g)：枚举 [index, value]
    BUILTIN_COUNT
} BuiltinId;

// 一个可执行单元：main 或一个 lum 函数
typedef struct {
    const char* name;          // 函数名（main 为 NULL）
    int is_main;
    Instruction* code;
    int code_len, code_cap;
    char** syms;               // 符号名池（变量名/函数名）
    int sym_cnt, sym_cap;
    Value* consts;             // 常量池
    int const_cnt, const_cap;
    char** params;             // 参数名（普通参数在前，可变参数最后）
    int param_cnt;             // 普通参数个数
    int has_variadic;
    int* param_is_ref;         // 参数是否是 ref 引用传递（1=ref，0=值传递），长度 param_cnt
    int is_generator;          // 是否为生成器函数（gen func）
    int* var_type_tags;        // 变量类型标记（CastKind 枚举，-1 表示无标记），与 syms 平行数组
    char** var_struct_names;    // 变量的 struct 类型名（NULL 表示不是 struct），与 syms 平行数组
    int is_method;              // 是否为结构体方法（self 参数传递指针）
    char* method_self_struct;   // 方法 self 参数的 struct 类型名
    char* class_name;           // 方法所属的 class 名（NULL 表示不是 class 方法）
} BytecodeFunc;

BytecodeFunc* bytecode_func_new(const char* name, int is_main);
void bytecode_func_free(BytecodeFunc* fn);
int bf_sym(BytecodeFunc* fn, const char* name);
int bf_const(BytecodeFunc* fn, Value v);
void bf_emit(BytecodeFunc* fn, OpCode op, int a, int b);
int bf_emit_here(BytecodeFunc* fn, OpCode op, int a, int b);
void bf_patch(BytecodeFunc* fn, int pos, int target);
void bf_patch_b(BytecodeFunc* fn, int pos, int target);

// 静态栈深度分析：计算每条指令执行前的栈深（写入 depth_out，可 NULL），
// 返回整个函数的最大栈深。IR 生成正确时每点栈深确定；不可达指令深度记 0。
int bc_analyze_stack(BytecodeFunc* fn, int* depth_out, int depth_cap);

// 反汇编：输出指令文本（-S 模式）
void bc_disasm(FILE* out, BytecodeFunc* fn);

#endif // LUMYR_IR_BYTECODE_H
