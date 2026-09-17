// lumyr-lang 字节码栈深度分析
// 跟踪所有专用栈（Value/int/double/float/uint/bool/char/byte 等）的深度变化
#ifndef LUMYR_IR_BYTECODE_STACK_H
#define LUMYR_IR_BYTECODE_STACK_H

#include "bytecode_type.h"

/* ============================================================
 * StackDelta 结构体：一条指令执行后各栈的深度变化
 * ============================================================ */
typedef struct {
    int value;          /* Value 栈深度变化 */
    int int_stack;      /* int 栈深度变化 */
    int double_stack;   /* double 栈深度变化 */
    int float_stack;    /* float 栈深度变化 */
    int uint_stack;     /* uint 栈深度变化 */
    int bool_stack;     /* bool 栈深度变化 */
    int char_stack;     /* char 栈深度变化 */
    int byte_stack;     /* byte 栈深度变化 */
    int short_stack;    /* short 栈深度变化 */
    int int8_stack;     /* int8 栈深度变化 */
    int int16_stack;    /* int16 栈深度变化 */
    int int32_stack;    /* int32 栈深度变化 */
    int int64_stack;    /* int64 栈深度变化 */
    int uint8_stack;    /* uint8 栈深度变化 */
    int uint16_stack;   /* uint16 栈深度变化 */
    int uint32_stack;   /* uint32 栈深度变化 */
    int uint64_stack;   /* uint64 栈深度变化 */
    int long_stack;     /* long 栈深度变化 */
    int ulong_stack;    /* ulong 栈深度变化 */
    int size_t_stack;   /* size_t 栈深度变化 */
    int ssize_t_stack;  /* ssize_t 栈深度变化 */
    int long_double_stack; /* long double 栈深度变化 */
    int long_long_stack;   /* long long 栈深度变化 */
} StackDelta;

/* ============================================================
 * 栈深度分析函数
 * ============================================================ */

/* 计算一条指令执行后各栈的深度变化 */
StackDelta op_stack_delta(BytecodeFunc* fn, Instruction in);

/* 指令执行瞬间的额外栈高（压栈动作造成的峰值超出进入深度部分） */
int op_stack_push(OpCode op);

#endif // LUMYR_IR_BYTECODE_STACK_H
