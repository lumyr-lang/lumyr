// lumyr-lang 字节码栈深度分析实现
#include "bytecode_stack.h"
#include <stdio.h>
#include <stdlib.h>

/* ============================================================
 * op_stack_delta：计算一条指令执行后各栈的深度变化
 * ============================================================ */
StackDelta op_stack_delta(BytecodeFunc* fn, Instruction in)
{
    StackDelta d;
    /* 初始化为 0 */
    d.value = 0;
    d.int_stack = 0;
    d.double_stack = 0;
    d.float_stack = 0;
    d.uint_stack = 0;
    d.bool_stack = 0;
    d.char_stack = 0;
    d.byte_stack = 0;
    d.short_stack = 0;
    d.int8_stack = 0;
    d.int16_stack = 0;
    d.int32_stack = 0;
    d.int64_stack = 0;
    d.uint8_stack = 0;
    d.uint16_stack = 0;
    d.uint32_stack = 0;
    d.uint64_stack = 0;
    d.long_stack = 0;
    d.ulong_stack = 0;
    d.size_t_stack = 0;
    d.ssize_t_stack = 0;
    d.long_double_stack = 0;
    d.long_long_stack = 0;

    switch(in.op) {
        /* ===== Value 栈压入指令 ===== */
        case OPC_LOAD_CONST:
        case OPC_LOAD_VAR:
        case OPC_LOAD_VAR_REF:
        case OPC_GETFUNC:
        case OPC_MKCLOSURE:
        case OPC_PRE_INC: case OPC_POST_INC: case OPC_PRE_DEC: case OPC_POST_DEC:
        case OPC_DUP:
            d.value = +1;
            break;

        /* ===== int 专用栈 ===== */
        case OPC_LOAD_INT64_VAR:
            d.int_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.int_stack = -1;
            d.value = +1;   /* 包装成 Value 压回（赋值表达式有返回值） */
            break;
        case OPC_PUSH_INT64_CONST:
            d.int_stack = +1;
            break;
        case OPC_INT64_ADD: case OPC_INT64_SUB: case OPC_INT64_MUL: case OPC_INT64_DIV: case OPC_INT64_MOD:
            d.int_stack = -1;    /* 弹2压1，净变化 -1 */
            break;
        case OPC_INT64_TO_VALUE:
            d.int_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_GT: case OPC_INT64_LT: case OPC_INT64_GE: case OPC_INT64_LE: case OPC_INT64_EQ: case OPC_INT64_NE:
            d.int_stack = -2;
            d.value = +1;
            break;
        case OPC_INT64_INDEX_SET:
            d.value = -1;
            d.int_stack = -1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) {
                d.int_stack = -in.b;  /* 从 int 栈读取 b 个元素 */
            } else {
                d.value = -in.b;      /* 从 Value 栈读取 b 个元素 */
            }
            d.value += 1;             /* 压入 1 个数组 Value */
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;             /* 弹 arr,idx 两个 Value */
            d.int_stack = +1;         /* 压入 int 栈 */
            break;

        /* ===== uint 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
            d.uint_stack = +1;
            break;
        case OPC_LOAD_INT64_VAR:
            d.uint_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.uint_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ADD: case OPC_INT64_SUB: case OPC_INT64_MUL: case OPC_INT64_DIV: case OPC_INT64_MOD:
            d.uint_stack = -1;
            break;
        case OPC_INT64_TO_VALUE:
            d.uint_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_GT: case OPC_INT64_LT: case OPC_INT64_GE: case OPC_INT64_LE: case OPC_INT64_EQ: case OPC_INT64_NE:
            d.uint_stack = -2;
            d.value = +1;
            break;
        case OPC_INT64_INDEX_SET:
            d.value = -1;
            d.uint_stack = -1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) {
                d.uint_stack = -in.b;
            } else {
                d.value = -in.b;
            }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.uint_stack = +1;
            break;

        /* ===== double 专用栈 ===== */
        case OPC_LOAD_DOUBLE_VAR:
        case OPC_PUSH_DOUBLE_CONST:
            d.double_stack = +1;
            break;
        case OPC_STORE_DOUBLE_VAR:
            d.double_stack = -1;
            d.value = +1;
            break;
        case OPC_DOUBLE_ADD: case OPC_DOUBLE_SUB: case OPC_DOUBLE_MUL: case OPC_DOUBLE_DIV:
            d.double_stack = -1;
            break;
        case OPC_DOUBLE_TO_VALUE:
            d.double_stack = -1;
            d.value = +1;
            break;
        case OPC_DOUBLE_GT: case OPC_DOUBLE_LT: case OPC_DOUBLE_GE: case OPC_DOUBLE_LE: case OPC_DOUBLE_EQ: case OPC_DOUBLE_NE:
            d.double_stack = -2;
            d.value = +1;
            break;
        case OPC_DOUBLE_INDEX_SET:
            d.value = -1;
            d.double_stack = -1;
            break;
        case OPC_DOUBLE_ARRAY_LIT:
            if(in.a == 1) {
                d.double_stack = -in.b;
            } else {
                d.value = -in.b;
            }
            d.value += 1;
            break;
        case OPC_DOUBLE_ARRAY_LIT:
            d.value = -2;
            d.double_stack = +1;
            break;

        /* ===== float 专用栈 ===== */
        case OPC_PUSH_DOUBLE_CONST:
        case OPC_LOAD_DOUBLE_VAR:
            d.float_stack = +1;
            break;
        case OPC_STORE_DOUBLE_VAR:
            d.float_stack = -1;
            d.value = +1;
            break;
        case OPC_DOUBLE_ADD: case OPC_DOUBLE_SUB: case OPC_DOUBLE_MUL: case OPC_DOUBLE_DIV:
            d.float_stack = -1;
            break;
        case OPC_DOUBLE_TO_VALUE:
            d.float_stack = -1;
            d.value = +1;
            break;
        case OPC_DOUBLE_GT: case OPC_DOUBLE_LT: case OPC_DOUBLE_GE: case OPC_DOUBLE_LE: case OPC_DOUBLE_EQ: case OPC_DOUBLE_NE:
            d.float_stack = -2;
            d.value = +1;
            break;
        case OPC_DOUBLE_INDEX_SET:
            d.value = -1;
            d.float_stack = -1;
            break;
        case OPC_DOUBLE_ARRAY_LIT:
            if(in.a == 1) {
                d.float_stack = -in.b;
            } else {
                d.value = -in.b;
            }
            d.value += 1;
            break;
        case OPC_DOUBLE_ARRAY_LIT:
            d.value = -2;
            d.float_stack = +1;
            break;

        /* ===== bool 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.bool_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.bool_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.bool_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) {
                d.bool_stack = -in.b;
            } else {
                d.value = -in.b;
            }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.bool_stack = +1;
            break;

        /* ===== char 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.char_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.char_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.char_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) {
                d.char_stack = -in.b;
            } else {
                d.value = -in.b;
            }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.char_stack = +1;
            break;

        /* ===== byte 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.byte_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.byte_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.byte_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) {
                d.byte_stack = -in.b;
            } else {
                d.value = -in.b;
            }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.byte_stack = +1;
            break;

        /* ===== int8 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.int8_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.int8_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ADD: case OPC_INT64_SUB: case OPC_INT64_MUL: case OPC_INT64_DIV: case OPC_INT64_MOD:
            d.int8_stack = -1;
            break;
        case OPC_INT64_TO_VALUE:
            d.int8_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_GT: case OPC_INT64_LT: case OPC_INT64_GE: case OPC_INT64_LE: case OPC_INT64_EQ: case OPC_INT64_NE:
            d.int8_stack = -2;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.int8_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.int8_stack = +1;
            break;

        /* ===== int16 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.int16_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.int16_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ADD: case OPC_INT64_SUB: case OPC_INT64_MUL: case OPC_INT64_DIV: case OPC_INT64_MOD:
            d.int16_stack = -1;
            break;
        case OPC_INT64_TO_VALUE:
            d.int16_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_GT: case OPC_INT64_LT: case OPC_INT64_GE: case OPC_INT64_LE: case OPC_INT64_EQ: case OPC_INT64_NE:
            d.int16_stack = -2;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.int16_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.int16_stack = +1;
            break;

        /* ===== short 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.short_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.short_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ADD: case OPC_INT64_SUB: case OPC_INT64_MUL: case OPC_INT64_DIV: case OPC_INT64_MOD:
            d.short_stack = -1;
            break;
        case OPC_INT64_TO_VALUE:
            d.short_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_GT: case OPC_INT64_LT: case OPC_INT64_GE: case OPC_INT64_LE: case OPC_INT64_EQ: case OPC_INT64_NE:
            d.short_stack = -2;
            d.value = +1;
            break;

        /* ===== int32 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.int32_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.int32_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ADD: case OPC_INT64_SUB: case OPC_INT64_MUL: case OPC_INT64_DIV: case OPC_INT64_MOD:
            d.int32_stack = -1;
            break;
        case OPC_INT64_GT: case OPC_INT64_LT: case OPC_INT64_GE: case OPC_INT64_LE: case OPC_INT64_EQ: case OPC_INT64_NE:
            d.int32_stack = -2;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.int32_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.int32_stack = +1;
            break;

        /* ===== int64 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.int64_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.int64_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ADD: case OPC_INT64_SUB: case OPC_INT64_MUL: case OPC_INT64_DIV: case OPC_INT64_MOD:
            d.int64_stack = -1;
            break;
        case OPC_INT64_TO_VALUE:
            d.int64_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_GT: case OPC_INT64_LT: case OPC_INT64_GE: case OPC_INT64_LE: case OPC_INT64_EQ: case OPC_INT64_NE:
            d.int64_stack = -2;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.int64_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.int64_stack = +1;
            break;

        /* ===== uint8 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.uint8_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.uint8_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.uint8_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.uint8_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.uint8_stack = +1;
            break;

        /* ===== uint16 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.uint16_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.uint16_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.uint16_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.uint16_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.uint16_stack = +1;
            break;

        /* ===== uint32 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.uint32_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.uint32_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.uint32_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.uint32_stack = +1;
            break;

        /* ===== uint64 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.uint64_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.uint64_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.uint64_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.uint64_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.uint64_stack = +1;
            break;

        /* ===== long 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.long_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.long_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.long_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.long_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.long_stack = +1;
            break;

        /* ===== ulong 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.ulong_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.ulong_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.ulong_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.ulong_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.ulong_stack = +1;
            break;

        /* ===== size_t 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.size_t_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.size_t_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.size_t_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.size_t_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.size_t_stack = +1;
            break;

        /* ===== ssize_t 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.ssize_t_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.ssize_t_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.ssize_t_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.ssize_t_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.ssize_t_stack = +1;
            break;

        /* ===== long long 专用栈 ===== */
        case OPC_PUSH_INT64_CONST:
        case OPC_LOAD_INT64_VAR:
            d.long_long_stack = +1;
            break;
        case OPC_STORE_INT64_VAR:
            d.long_long_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_ADD: case OPC_INT64_SUB: case OPC_INT64_MUL: case OPC_INT64_DIV: case OPC_INT64_MOD:
            d.long_long_stack = -1;
            break;
        case OPC_INT64_TO_VALUE:
            d.long_long_stack = -1;
            d.value = +1;
            break;
        case OPC_INT64_GT: case OPC_INT64_LT: case OPC_INT64_GE: case OPC_INT64_LE: case OPC_INT64_EQ: case OPC_INT64_NE:
            d.long_long_stack = -2;
            d.value = +1;
            break;
        case OPC_INT64_INDEX_SET:
            d.value = -1;
            d.long_long_stack = -1;
            break;
        case OPC_INT64_ARRAY_LIT:
            if(in.a == 1) { d.long_long_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT64_ARRAY_LIT:
            d.value = -2;
            d.long_long_stack = +1;
            break;

        /* ===== long double 专用栈 ===== */
        case OPC_PUSH_DOUBLE_CONST:
        case OPC_LOAD_DOUBLE_VAR:
            d.long_double_stack = +1;
            break;
        case OPC_STORE_DOUBLE_VAR:
            d.long_double_stack = -1;
            d.value = +1;
            break;
        case OPC_DOUBLE_ADD: case OPC_DOUBLE_SUB: case OPC_DOUBLE_MUL: case OPC_DOUBLE_DIV:
            d.long_double_stack = -1;
            break;
        case OPC_DOUBLE_TO_VALUE:
            d.long_double_stack = -1;
            d.value = +1;
            break;
        case OPC_DOUBLE_GT: case OPC_DOUBLE_LT: case OPC_DOUBLE_GE: case OPC_DOUBLE_LE: case OPC_DOUBLE_EQ: case OPC_DOUBLE_NE:
            d.long_double_stack = -2;
            d.value = +1;
            break;
        case OPC_DOUBLE_ARRAY_LIT:
            if(in.a == 1) { d.long_double_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_DOUBLE_ARRAY_LIT:
            d.value = -2;
            d.long_double_stack = +1;
            break;

        /* ===== 专用栈之间类型转换（Value 栈不变） ===== */
        case OPC_INT64_TO_VALUE:
            d.int_stack = -1;
            d.uint_stack = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.uint_stack = -1;
            d.int_stack = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.int_stack = -1;
            d.float_stack = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.int_stack = -1;
            d.double_stack = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.uint_stack = -1;
            d.float_stack = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.uint_stack = -1;
            d.double_stack = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.float_stack = -1;
            d.double_stack = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.int_stack = -1;
            d.long_long_stack = +1;
            break;
        case OPC_INT64_TO_VALUE:
            d.uint_stack = -1;
            d.long_long_stack = +1;
            break;
        case OPC_DOUBLE_TO_INT64:
            d.float_stack = -1;
            d.long_long_stack = +1;
            break;
        case OPC_DOUBLE_TO_INT64:
            d.double_stack = -1;
            d.long_long_stack = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.long_long_stack = -1;
            d.float_stack = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.long_long_stack = -1;
            d.double_stack = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.int_stack = -1;
            d.long_double_stack = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.uint_stack = -1;
            d.long_double_stack = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.float_stack = -1;
            d.long_double_stack = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.double_stack = -1;
            d.long_double_stack = +1;
            break;
        case OPC_INT64_TO_DOUBLE:
            d.long_long_stack = -1;
            d.long_double_stack = +1;
            break;

        /* ===== 通用 Value 栈指令 ===== */
        case OPC_POP:
        case OPC_PEND_RETURN:
        case OPC_THROW:
            d.value = -1;
            break;
        case OPC_ADD: case OPC_SUB: case OPC_MUL: case OPC_DIV: case OPC_MOD:
        case OPC_GT: case OPC_LT: case OPC_GE: case OPC_LE: case OPC_EQ: case OPC_NE: case OPC_IMPLEMENTS:
            d.value = -1;   /* 弹2压1 */
            break;
        case OPC_NEG: case OPC_POS:
        case OPC_LOGIC_NOT:
        case OPC_CAST_INT: case OPC_CAST_DOUBLE: case OPC_CAST_CHAR:
        case OPC_CAST_BOOL: case OPC_CAST_STRING: case OPC_CAST_ASCII:
        case OPC_CAST_BYTE:
        case OPC_CAST_INT8: case OPC_CAST_INT16: case OPC_CAST_INT32: case OPC_CAST_INT64:
        case OPC_CAST_UINT8: case OPC_CAST_UINT16: case OPC_CAST_UINT32: case OPC_CAST_UINT64:
        case OPC_CAST_LONG: case OPC_CAST_LONGLONG: case OPC_CAST_FLOAT:
            /* 弹1压1，栈不变 */
            break;
        case OPC_TRY:
        case OPC_ENDTRY:
        case OPC_FIN_PUSH:
        case OPC_FINISH:
            /* 栈不变 */
            break;
        case OPC_GET_ERR:
            d.value = +1;
            break;
        case OPC_BUILTIN:
            d.value = -in.b + 1;
            break;
        case OPC_ARRAY_LIT:
            d.value = -in.b + 1;
            break;
        case OPC_MAP_LIT:
            d.value = -2 * in.b + 1;
            break;
        case OPC_CLASS_NEW:
            d.value = +1;
            break;
        case OPC_INDEX_GET:
            d.value = -1;
            break;
        case OPC_INDEX_SET:
            d.value = -2;
            break;
        case OPC_LOAD_FIELD:
            d.value = +1;
            break;
        case OPC_STORE_FIELD:
            /* 弹1压1 */
            break;
        case OPC_LOAD_STRUCT_PTR:
            d.value = +1;
            break;
        case OPC_STORE_NESTED_FIELD:
            /* 弹1压1 */
            break;
        case OPC_STORE_VAR:
            /* 弹1压1 */
            break;
        case OPC_PRINT:
            d.value = -(in.a > 0 ? in.a : 1);
            break;
        case OPC_PRINT_INT64:
            d.int_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.uint_stack = -1;
            break;
        case OPC_PRINT_DOUBLE:
            d.float_stack = -1;
            break;
        case OPC_PRINT_DOUBLE:
            d.double_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.bool_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.char_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.byte_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.int8_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.int16_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.short_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.int32_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.int64_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.uint8_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.uint16_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.uint32_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.uint64_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.long_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.ulong_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.size_t_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.ssize_t_stack = -1;
            break;
        case OPC_PRINT_DOUBLE:
            d.long_double_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.long_long_stack = -1;
            break;
        case OPC_YIELD:
            /* 生成器 yield，栈不变 */
            break;
        case OPC_TO_BOOL:
        case OPC_JMP:
        case OPC_RETURN_NIL:
        case OPC_HALT:
        case OPC_NOP:
            /* 栈不变 */
            break;
        case OPC_JMP_IF_FALSE:
        case OPC_JMP_IF_TRUE:
        case OPC_JMP_IF_NULL:
            d.value = -1;   /* 弹条件 */
            break;
        case OPC_CALL:
            d.value = -in.b + 1;
            break;
        case OPC_CALLV:
            d.value = -in.b;
            break;
        case OPC_RETURN:
            d.value = -1;
            break;
        default:
            /* 未知指令默认不改变栈深度 */
            break;
    }
    return d;
}

/* ============================================================
 * op_stack_push：指令执行瞬间的额外栈高
 * ============================================================ */
int op_stack_push(OpCode op)
{
    switch(op) {
        case OPC_LOAD_CONST:
        case OPC_LOAD_VAR:
        case OPC_LOAD_VAR_REF:
        case OPC_GETFUNC:
        case OPC_MKCLOSURE:
        case OPC_PRE_INC: case OPC_POST_INC: case OPC_PRE_DEC: case OPC_POST_DEC:
        case OPC_DUP:
        case OPC_LOAD_FIELD:
        case OPC_INT64_ARRAY_LIT:
        case OPC_DOUBLE_ARRAY_LIT:
        case OPC_DOUBLE_ARRAY_LIT:
        case OPC_INT64_ARRAY_LIT:
        case OPC_INT64_ARRAY_LIT:
        case OPC_INT64_ARRAY_LIT:
        case OPC_INT64_ARRAY_LIT:
            return 1;
        default:
            return 0;
    }
}
