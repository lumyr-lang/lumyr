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
        case OPC_LOAD_INT_VAR:
            d.int_stack = +1;
            break;
        case OPC_STORE_INT_VAR:
            d.int_stack = -1;
            d.value = +1;   /* 包装成 Value 压回（赋值表达式有返回值） */
            break;
        case OPC_PUSH_INT_CONST:
            d.int_stack = +1;
            break;
        case OPC_INT_ADD: case OPC_INT_SUB: case OPC_INT_MUL: case OPC_INT_DIV: case OPC_INT_MOD:
            d.int_stack = -1;    /* 弹2压1，净变化 -1 */
            break;
        case OPC_INT_TO_VALUE:
            d.int_stack = -1;
            d.value = +1;
            break;
        case OPC_INT_GT: case OPC_INT_LT: case OPC_INT_GE: case OPC_INT_LE: case OPC_INT_EQ: case OPC_INT_NE:
            d.int_stack = -2;
            d.value = +1;
            break;
        case OPC_INT_ARRAY_SET:
            d.value = -1;
            d.int_stack = -1;
            break;
        case OPC_INT_ARRAY_LIT:
            if(in.a == 1) {
                d.int_stack = -in.b;  /* 从 int 栈读取 b 个元素 */
            } else {
                d.value = -in.b;      /* 从 Value 栈读取 b 个元素 */
            }
            d.value += 1;             /* 压入 1 个数组 Value */
            break;
        case OPC_INT_ARRAY_GET:
            d.value = -2;             /* 弹 arr,idx 两个 Value */
            d.int_stack = +1;         /* 压入 int 栈 */
            break;

        /* ===== uint 专用栈 ===== */
        case OPC_PUSH_UINT_CONST:
            d.uint_stack = +1;
            break;
        case OPC_LOAD_UINT_VAR:
            d.uint_stack = +1;
            break;
        case OPC_STORE_UINT_VAR:
            d.uint_stack = -1;
            d.value = +1;
            break;
        case OPC_UINT_ADD: case OPC_UINT_SUB: case OPC_UINT_MUL: case OPC_UINT_DIV: case OPC_UINT_MOD:
            d.uint_stack = -1;
            break;
        case OPC_UINT_TO_VALUE:
            d.uint_stack = -1;
            d.value = +1;
            break;
        case OPC_UINT_GT: case OPC_UINT_LT: case OPC_UINT_GE: case OPC_UINT_LE: case OPC_UINT_EQ: case OPC_UINT_NE:
            d.uint_stack = -2;
            d.value = +1;
            break;
        case OPC_UINT_ARRAY_SET:
            d.value = -1;
            d.uint_stack = -1;
            break;
        case OPC_UINT_ARRAY_LIT:
            if(in.a == 1) {
                d.uint_stack = -in.b;
            } else {
                d.value = -in.b;
            }
            d.value += 1;
            break;
        case OPC_UINT_ARRAY_GET:
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
        case OPC_DOUBLE_ARRAY_SET:
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
        case OPC_DOUBLE_ARRAY_GET:
            d.value = -2;
            d.double_stack = +1;
            break;

        /* ===== float 专用栈 ===== */
        case OPC_PUSH_FLOAT_CONST:
        case OPC_LOAD_FLOAT_VAR:
            d.float_stack = +1;
            break;
        case OPC_STORE_FLOAT_VAR:
            d.float_stack = -1;
            d.value = +1;
            break;
        case OPC_FLOAT_ADD: case OPC_FLOAT_SUB: case OPC_FLOAT_MUL: case OPC_FLOAT_DIV:
            d.float_stack = -1;
            break;
        case OPC_FLOAT_TO_VALUE:
            d.float_stack = -1;
            d.value = +1;
            break;
        case OPC_FLOAT_GT: case OPC_FLOAT_LT: case OPC_FLOAT_GE: case OPC_FLOAT_LE: case OPC_FLOAT_EQ: case OPC_FLOAT_NE:
            d.float_stack = -2;
            d.value = +1;
            break;
        case OPC_FLOAT_ARRAY_SET:
            d.value = -1;
            d.float_stack = -1;
            break;
        case OPC_FLOAT_ARRAY_LIT:
            if(in.a == 1) {
                d.float_stack = -in.b;
            } else {
                d.value = -in.b;
            }
            d.value += 1;
            break;
        case OPC_FLOAT_ARRAY_GET:
            d.value = -2;
            d.float_stack = +1;
            break;

        /* ===== bool 专用栈 ===== */
        case OPC_PUSH_BOOL_CONST:
        case OPC_LOAD_BOOL_VAR:
            d.bool_stack = +1;
            break;
        case OPC_STORE_BOOL_VAR:
            d.bool_stack = -1;
            d.value = +1;
            break;
        case OPC_BOOL_TO_VALUE:
            d.bool_stack = -1;
            d.value = +1;
            break;
        case OPC_BOOL_ARRAY_LIT:
            if(in.a == 1) {
                d.bool_stack = -in.b;
            } else {
                d.value = -in.b;
            }
            d.value += 1;
            break;
        case OPC_BOOL_ARRAY_GET:
            d.value = -2;
            d.bool_stack = +1;
            break;

        /* ===== char 专用栈 ===== */
        case OPC_PUSH_CHAR_CONST:
        case OPC_LOAD_CHAR_VAR:
            d.char_stack = +1;
            break;
        case OPC_STORE_CHAR_VAR:
            d.char_stack = -1;
            d.value = +1;
            break;
        case OPC_CHAR_TO_VALUE:
            d.char_stack = -1;
            d.value = +1;
            break;
        case OPC_CHAR_ARRAY_LIT:
            if(in.a == 1) {
                d.char_stack = -in.b;
            } else {
                d.value = -in.b;
            }
            d.value += 1;
            break;
        case OPC_CHAR_ARRAY_GET:
            d.value = -2;
            d.char_stack = +1;
            break;

        /* ===== byte 专用栈 ===== */
        case OPC_PUSH_BYTE_CONST:
        case OPC_LOAD_BYTE_VAR:
            d.byte_stack = +1;
            break;
        case OPC_STORE_BYTE_VAR:
            d.byte_stack = -1;
            d.value = +1;
            break;
        case OPC_BYTE_TO_VALUE:
            d.byte_stack = -1;
            d.value = +1;
            break;
        case OPC_BYTE_ARRAY_LIT:
            if(in.a == 1) {
                d.byte_stack = -in.b;
            } else {
                d.value = -in.b;
            }
            d.value += 1;
            break;
        case OPC_BYTE_ARRAY_GET:
            d.value = -2;
            d.byte_stack = +1;
            break;

        /* ===== int8 专用栈 ===== */
        case OPC_PUSH_INT8_CONST:
        case OPC_LOAD_INT8_VAR:
            d.int8_stack = +1;
            break;
        case OPC_STORE_INT8_VAR:
            d.int8_stack = -1;
            d.value = +1;
            break;
        case OPC_INT8_ADD: case OPC_INT8_SUB: case OPC_INT8_MUL: case OPC_INT8_DIV: case OPC_INT8_MOD:
            d.int8_stack = -1;
            break;
        case OPC_INT8_TO_VALUE:
            d.int8_stack = -1;
            d.value = +1;
            break;
        case OPC_INT8_GT: case OPC_INT8_LT: case OPC_INT8_GE: case OPC_INT8_LE: case OPC_INT8_EQ: case OPC_INT8_NE:
            d.int8_stack = -2;
            d.value = +1;
            break;
        case OPC_INT8_ARRAY_LIT:
            if(in.a == 1) { d.int8_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT8_ARRAY_GET:
            d.value = -2;
            d.int8_stack = +1;
            break;

        /* ===== int16 专用栈 ===== */
        case OPC_PUSH_INT16_CONST:
        case OPC_LOAD_INT16_VAR:
            d.int16_stack = +1;
            break;
        case OPC_STORE_INT16_VAR:
            d.int16_stack = -1;
            d.value = +1;
            break;
        case OPC_INT16_ADD: case OPC_INT16_SUB: case OPC_INT16_MUL: case OPC_INT16_DIV: case OPC_INT16_MOD:
            d.int16_stack = -1;
            break;
        case OPC_INT16_TO_VALUE:
            d.int16_stack = -1;
            d.value = +1;
            break;
        case OPC_INT16_GT: case OPC_INT16_LT: case OPC_INT16_GE: case OPC_INT16_LE: case OPC_INT16_EQ: case OPC_INT16_NE:
            d.int16_stack = -2;
            d.value = +1;
            break;
        case OPC_INT16_ARRAY_LIT:
            if(in.a == 1) { d.int16_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT16_ARRAY_GET:
            d.value = -2;
            d.int16_stack = +1;
            break;

        /* ===== short 专用栈 ===== */
        case OPC_PUSH_SHORT_CONST:
        case OPC_LOAD_SHORT_VAR:
            d.short_stack = +1;
            break;
        case OPC_STORE_SHORT_VAR:
            d.short_stack = -1;
            d.value = +1;
            break;
        case OPC_SHORT_ADD: case OPC_SHORT_SUB: case OPC_SHORT_MUL: case OPC_SHORT_DIV: case OPC_SHORT_MOD:
            d.short_stack = -1;
            break;
        case OPC_SHORT_TO_VALUE:
            d.short_stack = -1;
            d.value = +1;
            break;
        case OPC_SHORT_GT: case OPC_SHORT_LT: case OPC_SHORT_GE: case OPC_SHORT_LE: case OPC_SHORT_EQ: case OPC_SHORT_NE:
            d.short_stack = -2;
            d.value = +1;
            break;

        /* ===== int32 专用栈 ===== */
        case OPC_PUSH_INT32_CONST:
        case OPC_LOAD_INT32_VAR:
            d.int32_stack = +1;
            break;
        case OPC_STORE_INT32_VAR:
            d.int32_stack = -1;
            d.value = +1;
            break;
        case OPC_INT32_ADD: case OPC_INT32_SUB: case OPC_INT32_MUL: case OPC_INT32_DIV: case OPC_INT32_MOD:
            d.int32_stack = -1;
            break;
        case OPC_INT32_GT: case OPC_INT32_LT: case OPC_INT32_GE: case OPC_INT32_LE: case OPC_INT32_EQ: case OPC_INT32_NE:
            d.int32_stack = -2;
            d.value = +1;
            break;
        case OPC_INT32_ARRAY_LIT:
            if(in.a == 1) { d.int32_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_INT32_ARRAY_GET:
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
        case OPC_INT64_ARRAY_GET:
            d.value = -2;
            d.int64_stack = +1;
            break;

        /* ===== uint8 专用栈 ===== */
        case OPC_PUSH_UINT8_CONST:
        case OPC_LOAD_UINT8_VAR:
            d.uint8_stack = +1;
            break;
        case OPC_STORE_UINT8_VAR:
            d.uint8_stack = -1;
            d.value = +1;
            break;
        case OPC_UINT8_TO_VALUE:
            d.uint8_stack = -1;
            d.value = +1;
            break;
        case OPC_UINT8_ARRAY_LIT:
            if(in.a == 1) { d.uint8_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_UINT8_ARRAY_GET:
            d.value = -2;
            d.uint8_stack = +1;
            break;

        /* ===== uint16 专用栈 ===== */
        case OPC_PUSH_UINT16_CONST:
        case OPC_LOAD_UINT16_VAR:
            d.uint16_stack = +1;
            break;
        case OPC_STORE_UINT16_VAR:
            d.uint16_stack = -1;
            d.value = +1;
            break;
        case OPC_UINT16_TO_VALUE:
            d.uint16_stack = -1;
            d.value = +1;
            break;
        case OPC_UINT16_ARRAY_LIT:
            if(in.a == 1) { d.uint16_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_UINT16_ARRAY_GET:
            d.value = -2;
            d.uint16_stack = +1;
            break;

        /* ===== uint32 专用栈 ===== */
        case OPC_PUSH_UINT32_CONST:
        case OPC_LOAD_UINT32_VAR:
            d.uint32_stack = +1;
            break;
        case OPC_STORE_UINT32_VAR:
            d.uint32_stack = -1;
            d.value = +1;
            break;
        case OPC_UINT32_ARRAY_LIT:
            if(in.a == 1) { d.uint32_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_UINT32_ARRAY_GET:
            d.value = -2;
            d.uint32_stack = +1;
            break;

        /* ===== uint64 专用栈 ===== */
        case OPC_PUSH_UINT64_CONST:
        case OPC_LOAD_UINT64_VAR:
            d.uint64_stack = +1;
            break;
        case OPC_STORE_UINT64_VAR:
            d.uint64_stack = -1;
            d.value = +1;
            break;
        case OPC_UINT64_TO_VALUE:
            d.uint64_stack = -1;
            d.value = +1;
            break;
        case OPC_UINT64_ARRAY_LIT:
            if(in.a == 1) { d.uint64_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_UINT64_ARRAY_GET:
            d.value = -2;
            d.uint64_stack = +1;
            break;

        /* ===== long 专用栈 ===== */
        case OPC_PUSH_LONG_CONST:
        case OPC_LOAD_LONG_VAR:
            d.long_stack = +1;
            break;
        case OPC_STORE_LONG_VAR:
            d.long_stack = -1;
            d.value = +1;
            break;
        case OPC_LONG_TO_VALUE:
            d.long_stack = -1;
            d.value = +1;
            break;
        case OPC_LONG_ARRAY_LIT:
            if(in.a == 1) { d.long_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_LONG_ARRAY_GET:
            d.value = -2;
            d.long_stack = +1;
            break;

        /* ===== ulong 专用栈 ===== */
        case OPC_PUSH_ULONG_CONST:
        case OPC_LOAD_ULONG_VAR:
            d.ulong_stack = +1;
            break;
        case OPC_STORE_ULONG_VAR:
            d.ulong_stack = -1;
            d.value = +1;
            break;
        case OPC_ULONG_TO_VALUE:
            d.ulong_stack = -1;
            d.value = +1;
            break;
        case OPC_ULONG_ARRAY_LIT:
            if(in.a == 1) { d.ulong_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_ULONG_ARRAY_GET:
            d.value = -2;
            d.ulong_stack = +1;
            break;

        /* ===== size_t 专用栈 ===== */
        case OPC_PUSH_SIZE_T_CONST:
        case OPC_LOAD_SIZE_T_VAR:
            d.size_t_stack = +1;
            break;
        case OPC_STORE_SIZE_T_VAR:
            d.size_t_stack = -1;
            d.value = +1;
            break;
        case OPC_SIZE_T_TO_VALUE:
            d.size_t_stack = -1;
            d.value = +1;
            break;
        case OPC_SIZE_T_ARRAY_LIT:
            if(in.a == 1) { d.size_t_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_SIZE_T_ARRAY_GET:
            d.value = -2;
            d.size_t_stack = +1;
            break;

        /* ===== ssize_t 专用栈 ===== */
        case OPC_PUSH_SSIZE_T_CONST:
        case OPC_LOAD_SSIZE_T_VAR:
            d.ssize_t_stack = +1;
            break;
        case OPC_STORE_SSIZE_T_VAR:
            d.ssize_t_stack = -1;
            d.value = +1;
            break;
        case OPC_SSIZE_T_TO_VALUE:
            d.ssize_t_stack = -1;
            d.value = +1;
            break;
        case OPC_SSIZE_T_ARRAY_LIT:
            if(in.a == 1) { d.ssize_t_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_SSIZE_T_ARRAY_GET:
            d.value = -2;
            d.ssize_t_stack = +1;
            break;

        /* ===== long long 专用栈 ===== */
        case OPC_PUSH_LONG_LONG_CONST:
        case OPC_LOAD_LONG_LONG_VAR:
            d.long_long_stack = +1;
            break;
        case OPC_STORE_LONG_LONG_VAR:
            d.long_long_stack = -1;
            d.value = +1;
            break;
        case OPC_LONG_LONG_ADD: case OPC_LONG_LONG_SUB: case OPC_LONG_LONG_MUL: case OPC_LONG_LONG_DIV: case OPC_LONG_LONG_MOD:
            d.long_long_stack = -1;
            break;
        case OPC_LONG_LONG_TO_VALUE:
            d.long_long_stack = -1;
            d.value = +1;
            break;
        case OPC_LONG_LONG_GT: case OPC_LONG_LONG_LT: case OPC_LONG_LONG_GE: case OPC_LONG_LONG_LE: case OPC_LONG_LONG_EQ: case OPC_LONG_LONG_NE:
            d.long_long_stack = -2;
            d.value = +1;
            break;
        case OPC_LONG_LONG_ARRAY_SET:
            d.value = -1;
            d.long_long_stack = -1;
            break;
        case OPC_LONG_LONG_ARRAY_LIT:
            if(in.a == 1) { d.long_long_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_LONG_LONG_ARRAY_GET:
            d.value = -2;
            d.long_long_stack = +1;
            break;

        /* ===== long double 专用栈 ===== */
        case OPC_PUSH_LONG_DOUBLE_CONST:
        case OPC_LOAD_LONG_DOUBLE_VAR:
            d.long_double_stack = +1;
            break;
        case OPC_STORE_LONG_DOUBLE_VAR:
            d.long_double_stack = -1;
            d.value = +1;
            break;
        case OPC_LONG_DOUBLE_ADD: case OPC_LONG_DOUBLE_SUB: case OPC_LONG_DOUBLE_MUL: case OPC_LONG_DOUBLE_DIV:
            d.long_double_stack = -1;
            break;
        case OPC_LONG_DOUBLE_TO_VALUE:
            d.long_double_stack = -1;
            d.value = +1;
            break;
        case OPC_LONG_DOUBLE_GT: case OPC_LONG_DOUBLE_LT: case OPC_LONG_DOUBLE_GE: case OPC_LONG_DOUBLE_LE: case OPC_LONG_DOUBLE_EQ: case OPC_LONG_DOUBLE_NE:
            d.long_double_stack = -2;
            d.value = +1;
            break;
        case OPC_LONG_DOUBLE_ARRAY_LIT:
            if(in.a == 1) { d.long_double_stack = -in.b; } else { d.value = -in.b; }
            d.value += 1;
            break;
        case OPC_LONG_DOUBLE_ARRAY_GET:
            d.value = -2;
            d.long_double_stack = +1;
            break;

        /* ===== 专用栈之间类型转换（Value 栈不变） ===== */
        case OPC_INT_TO_UINT:
            d.int_stack = -1;
            d.uint_stack = +1;
            break;
        case OPC_UINT_TO_INT:
            d.uint_stack = -1;
            d.int_stack = +1;
            break;
        case OPC_INT_TO_FLOAT:
            d.int_stack = -1;
            d.float_stack = +1;
            break;
        case OPC_INT_TO_DOUBLE:
            d.int_stack = -1;
            d.double_stack = +1;
            break;
        case OPC_UINT_TO_FLOAT:
            d.uint_stack = -1;
            d.float_stack = +1;
            break;
        case OPC_UINT_TO_DOUBLE:
            d.uint_stack = -1;
            d.double_stack = +1;
            break;
        case OPC_FLOAT_TO_DOUBLE:
            d.float_stack = -1;
            d.double_stack = +1;
            break;
        case OPC_INT_TO_LONG_LONG:
            d.int_stack = -1;
            d.long_long_stack = +1;
            break;
        case OPC_UINT_TO_LONG_LONG:
            d.uint_stack = -1;
            d.long_long_stack = +1;
            break;
        case OPC_FLOAT_TO_LONG_LONG:
            d.float_stack = -1;
            d.long_long_stack = +1;
            break;
        case OPC_DOUBLE_TO_LONG_LONG:
            d.double_stack = -1;
            d.long_long_stack = +1;
            break;
        case OPC_LONG_LONG_TO_FLOAT:
            d.long_long_stack = -1;
            d.float_stack = +1;
            break;
        case OPC_LONG_LONG_TO_DOUBLE:
            d.long_long_stack = -1;
            d.double_stack = +1;
            break;
        case OPC_INT_TO_LONG_DOUBLE:
            d.int_stack = -1;
            d.long_double_stack = +1;
            break;
        case OPC_UINT_TO_LONG_DOUBLE:
            d.uint_stack = -1;
            d.long_double_stack = +1;
            break;
        case OPC_FLOAT_TO_LONG_DOUBLE:
            d.float_stack = -1;
            d.long_double_stack = +1;
            break;
        case OPC_DOUBLE_TO_LONG_DOUBLE:
            d.double_stack = -1;
            d.long_double_stack = +1;
            break;
        case OPC_LONG_LONG_TO_LONG_DOUBLE:
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
        case OPC_PRINT_INT:
            d.int_stack = -1;
            break;
        case OPC_PRINT_UINT:
            d.uint_stack = -1;
            break;
        case OPC_PRINT_FLOAT:
            d.float_stack = -1;
            break;
        case OPC_PRINT_DOUBLE:
            d.double_stack = -1;
            break;
        case OPC_PRINT_BOOL:
            d.bool_stack = -1;
            break;
        case OPC_PRINT_CHAR:
            d.char_stack = -1;
            break;
        case OPC_PRINT_BYTE:
            d.byte_stack = -1;
            break;
        case OPC_PRINT_INT8:
            d.int8_stack = -1;
            break;
        case OPC_PRINT_INT16:
            d.int16_stack = -1;
            break;
        case OPC_PRINT_SHORT:
            d.short_stack = -1;
            break;
        case OPC_PRINT_INT32:
            d.int32_stack = -1;
            break;
        case OPC_PRINT_INT64:
            d.int64_stack = -1;
            break;
        case OPC_PRINT_UINT8:
            d.uint8_stack = -1;
            break;
        case OPC_PRINT_UINT16:
            d.uint16_stack = -1;
            break;
        case OPC_PRINT_UINT32:
            d.uint32_stack = -1;
            break;
        case OPC_PRINT_UINT64:
            d.uint64_stack = -1;
            break;
        case OPC_PRINT_LONG:
            d.long_stack = -1;
            break;
        case OPC_PRINT_ULONG:
            d.ulong_stack = -1;
            break;
        case OPC_PRINT_SIZE_T:
            d.size_t_stack = -1;
            break;
        case OPC_PRINT_SSIZE_T:
            d.ssize_t_stack = -1;
            break;
        case OPC_PRINT_LONG_DOUBLE:
            d.long_double_stack = -1;
            break;
        case OPC_PRINT_LONG_LONG:
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
        case OPC_INT_ARRAY_LIT:
        case OPC_DOUBLE_ARRAY_LIT:
        case OPC_FLOAT_ARRAY_LIT:
        case OPC_UINT_ARRAY_LIT:
        case OPC_BOOL_ARRAY_LIT:
        case OPC_CHAR_ARRAY_LIT:
        case OPC_BYTE_ARRAY_LIT:
            return 1;
        default:
            return 0;
    }
}
