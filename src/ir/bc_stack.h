// lumyr-lang 字节码栈深度分析
// 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
#ifndef LUMYR_IR_BYTECODE_STACK_H
#define LUMYR_IR_BYTECODE_STACK_H

#include "bytecode_type.h"

/* ============================================================
 * StackDelta 结构体：一条指令执行后各栈的深度变化
 * 4 核心栈设计，所有细分类型合并到对应宽类型栈
 * ============================================================ */
typedef struct {
    int value;       /* STACK_VALUE 深度变化 */
    int int64;       /* STACK_INT64 深度变化（所有整数/布尔/字符） */
    int double_stk;  /* STACK_DOUBLE 深度变化（所有浮点） */
    int ptr;         /* STACK_PTR 深度变化（所有指针/字符串） */
} StackDelta;

/* ============================================================
 * 栈深度分析函数
 * ============================================================ */

/* 计算一条指令执行后各栈的深度变化 */
StackDelta op_stack_delta(BytecodeFunc* fn, Instruction in);

/* 指令执行瞬间的额外栈高（压栈动作造成的峰值超出进入深度部分） */
int op_stack_push(OpCode op);

#endif // LUMYR_IR_BYTECODE_STACK_H
