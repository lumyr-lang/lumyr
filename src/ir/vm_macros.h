#ifndef LUMYR_VM_MACROS_H
#define LUMYR_VM_MACROS_H

/*
 * VM 专用栈操作宏定义
 *
 * 本头文件定义 VM 中使用的专用栈操作宏，包括各种数据类型的
 * PUSH/POP/PEEK/TOP 操作。这些宏使用统一的栈管理模块 stack_manager。
 */

#include "stack_manager.h"

/* ========== 整数类型专用栈操作 ========== */

#define INT_PUSH(val) do { stack_global_ensure(STACK_INT, 1); ((int64_t*)stack_global_get_stack(STACK_INT))[(*stack_global_get_sp(STACK_INT))++] = (val); } while(0)
#define INT_POP() (((int64_t*)stack_global_get_stack(STACK_INT))[--(*stack_global_get_sp(STACK_INT))])
#define INT_PEEK() (((int64_t*)stack_global_get_stack(STACK_INT))[(*stack_global_get_sp(STACK_INT)) - 1])
#define INT_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_INT))[(*stack_global_get_sp(STACK_INT)) - 1 - (idx)])

#define UINT_PUSH(val) do { stack_global_ensure(STACK_UINT, 1); ((int64_t*)stack_global_get_stack(STACK_UINT))[(*stack_global_get_sp(STACK_UINT))++] = (val); } while(0)
#define UINT_POP() (((int64_t*)stack_global_get_stack(STACK_UINT))[--(*stack_global_get_sp(STACK_UINT))])
#define UINT_PEEK() (((int64_t*)stack_global_get_stack(STACK_UINT))[(*stack_global_get_sp(STACK_UINT)) - 1])
#define UINT_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_UINT))[(*stack_global_get_sp(STACK_UINT)) - 1 - (idx)])

#define INT8_PUSH(val) do { stack_global_ensure(STACK_INT8, 1); ((int64_t*)stack_global_get_stack(STACK_INT8))[(*stack_global_get_sp(STACK_INT8))++] = (val); } while(0)
#define INT8_POP() (((int64_t*)stack_global_get_stack(STACK_INT8))[--(*stack_global_get_sp(STACK_INT8))])
#define INT8_PEEK() (((int64_t*)stack_global_get_stack(STACK_INT8))[(*stack_global_get_sp(STACK_INT8)) - 1])
#define INT8_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_INT8))[(*stack_global_get_sp(STACK_INT8)) - 1 - (idx)])

#define INT16_PUSH(val) do { stack_global_ensure(STACK_INT16, 1); ((int64_t*)stack_global_get_stack(STACK_INT16))[(*stack_global_get_sp(STACK_INT16))++] = (val); } while(0)
#define INT16_POP() (((int64_t*)stack_global_get_stack(STACK_INT16))[--(*stack_global_get_sp(STACK_INT16))])
#define INT16_PEEK() (((int64_t*)stack_global_get_stack(STACK_INT16))[(*stack_global_get_sp(STACK_INT16)) - 1])
#define INT16_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_INT16))[(*stack_global_get_sp(STACK_INT16)) - 1 - (idx)])

#define SHORT_PUSH(val) do { stack_global_ensure(STACK_SHORT, 1); ((int64_t*)stack_global_get_stack(STACK_SHORT))[(*stack_global_get_sp(STACK_SHORT))++] = (val); } while(0)
#define SHORT_POP() (((int64_t*)stack_global_get_stack(STACK_SHORT))[--(*stack_global_get_sp(STACK_SHORT))])
#define SHORT_PEEK() (((int64_t*)stack_global_get_stack(STACK_SHORT))[(*stack_global_get_sp(STACK_SHORT)) - 1])
#define SHORT_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_SHORT))[(*stack_global_get_sp(STACK_SHORT)) - 1 - (idx)])

#define INT32_PUSH(val) do { stack_global_ensure(STACK_INT32, 1); ((int64_t*)stack_global_get_stack(STACK_INT32))[(*stack_global_get_sp(STACK_INT32))++] = (val); } while(0)
#define INT32_POP() (((int64_t*)stack_global_get_stack(STACK_INT32))[--(*stack_global_get_sp(STACK_INT32))])
#define INT32_PEEK() (((int64_t*)stack_global_get_stack(STACK_INT32))[(*stack_global_get_sp(STACK_INT32)) - 1])
#define INT32_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_INT32))[(*stack_global_get_sp(STACK_INT32)) - 1 - (idx)])

#define INT64_PUSH(val) do { stack_global_ensure(STACK_INT64, 1); ((int64_t*)stack_global_get_stack(STACK_INT64))[(*stack_global_get_sp(STACK_INT64))++] = (val); } while(0)
#define INT64_POP() (((int64_t*)stack_global_get_stack(STACK_INT64))[--(*stack_global_get_sp(STACK_INT64))])
#define INT64_PEEK() (((int64_t*)stack_global_get_stack(STACK_INT64))[(*stack_global_get_sp(STACK_INT64)) - 1])
#define INT64_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_INT64))[(*stack_global_get_sp(STACK_INT64)) - 1 - (idx)])

#define UINT8_PUSH(val) do { stack_global_ensure(STACK_UINT8, 1); ((int64_t*)stack_global_get_stack(STACK_UINT8))[(*stack_global_get_sp(STACK_UINT8))++] = (val); } while(0)
#define UINT8_POP() (((int64_t*)stack_global_get_stack(STACK_UINT8))[--(*stack_global_get_sp(STACK_UINT8))])
#define UINT8_PEEK() (((int64_t*)stack_global_get_stack(STACK_UINT8))[(*stack_global_get_sp(STACK_UINT8)) - 1])
#define UINT8_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_UINT8))[(*stack_global_get_sp(STACK_UINT8)) - 1 - (idx)])

#define UINT16_PUSH(val) do { stack_global_ensure(STACK_UINT16, 1); ((int64_t*)stack_global_get_stack(STACK_UINT16))[(*stack_global_get_sp(STACK_UINT16))++] = (val); } while(0)
#define UINT16_POP() (((int64_t*)stack_global_get_stack(STACK_UINT16))[--(*stack_global_get_sp(STACK_UINT16))])
#define UINT16_PEEK() (((int64_t*)stack_global_get_stack(STACK_UINT16))[(*stack_global_get_sp(STACK_UINT16)) - 1])
#define UINT16_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_UINT16))[(*stack_global_get_sp(STACK_UINT16)) - 1 - (idx)])

#define UINT32_PUSH(val) do { stack_global_ensure(STACK_UINT32, 1); ((int64_t*)stack_global_get_stack(STACK_UINT32))[(*stack_global_get_sp(STACK_UINT32))++] = (val); } while(0)
#define UINT32_POP() (((int64_t*)stack_global_get_stack(STACK_UINT32))[--(*stack_global_get_sp(STACK_UINT32))])
#define UINT32_PEEK() (((int64_t*)stack_global_get_stack(STACK_UINT32))[(*stack_global_get_sp(STACK_UINT32)) - 1])
#define UINT32_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_UINT32))[(*stack_global_get_sp(STACK_UINT32)) - 1 - (idx)])

#define UINT64_PUSH(val) do { stack_global_ensure(STACK_UINT64, 1); ((int64_t*)stack_global_get_stack(STACK_UINT64))[(*stack_global_get_sp(STACK_UINT64))++] = (val); } while(0)
#define UINT64_POP() (((int64_t*)stack_global_get_stack(STACK_UINT64))[--(*stack_global_get_sp(STACK_UINT64))])
#define UINT64_PEEK() (((int64_t*)stack_global_get_stack(STACK_UINT64))[(*stack_global_get_sp(STACK_UINT64)) - 1])
#define UINT64_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_UINT64))[(*stack_global_get_sp(STACK_UINT64)) - 1 - (idx)])

/* ========== 浮点类型专用栈操作 ========== */

#define FLOAT_PUSH(val) do { stack_global_ensure(STACK_FLOAT, 1); ((double*)stack_global_get_stack(STACK_FLOAT))[(*stack_global_get_sp(STACK_FLOAT))++] = (val); } while(0)
#define FLOAT_POP() (((double*)stack_global_get_stack(STACK_FLOAT))[--(*stack_global_get_sp(STACK_FLOAT))])
#define FLOAT_PEEK() (((double*)stack_global_get_stack(STACK_FLOAT))[(*stack_global_get_sp(STACK_FLOAT)) - 1])
#define FLOAT_TOP(idx) (((double*)stack_global_get_stack(STACK_FLOAT))[(*stack_global_get_sp(STACK_FLOAT)) - 1 - (idx)])

#define DOUBLE_PUSH(val) do { stack_global_ensure(STACK_DOUBLE, 1); ((double*)stack_global_get_stack(STACK_DOUBLE))[(*stack_global_get_sp(STACK_DOUBLE))++] = (val); } while(0)
static inline double double_pop_debug(const char* file, int line, const char* func) {
    int* sp = stack_global_get_sp(STACK_DOUBLE);
    if(*sp <= 0) {
        fprintf(stderr, "[DOUBLE_POP] STACK UNDERFLOW! sp=%d, called from %s:%d in %s\n", *sp, file, line, func);
    }
    return ((double*)stack_global_get_stack(STACK_DOUBLE))[--(*sp)];
}
#define DOUBLE_POP() double_pop_debug(__FILE__, __LINE__, __func__)
#define DOUBLE_PEEK() (((double*)stack_global_get_stack(STACK_DOUBLE))[(*stack_global_get_sp(STACK_DOUBLE)) - 1])
#define DOUBLE_TOP(idx) (((double*)stack_global_get_stack(STACK_DOUBLE))[(*stack_global_get_sp(STACK_DOUBLE)) - 1 - (idx)])

/* ========== 其他类型专用栈操作 ========== */

#define BOOL_PUSH(val) do { stack_global_ensure(STACK_BOOL, 1); ((int64_t*)stack_global_get_stack(STACK_BOOL))[(*stack_global_get_sp(STACK_BOOL))++] = (val); } while(0)
#define BOOL_POP() (((int64_t*)stack_global_get_stack(STACK_BOOL))[--(*stack_global_get_sp(STACK_BOOL))])
#define BOOL_PEEK() (((int64_t*)stack_global_get_stack(STACK_BOOL))[(*stack_global_get_sp(STACK_BOOL)) - 1])
#define BOOL_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_BOOL))[(*stack_global_get_sp(STACK_BOOL)) - 1 - (idx)])

#define CHAR_PUSH(val) do { stack_global_ensure(STACK_CHAR, 1); ((int64_t*)stack_global_get_stack(STACK_CHAR))[(*stack_global_get_sp(STACK_CHAR))++] = (val); } while(0)
#define CHAR_POP() (((int64_t*)stack_global_get_stack(STACK_CHAR))[--(*stack_global_get_sp(STACK_CHAR))])
#define CHAR_PEEK() (((int64_t*)stack_global_get_stack(STACK_CHAR))[(*stack_global_get_sp(STACK_CHAR)) - 1])
#define CHAR_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_CHAR))[(*stack_global_get_sp(STACK_CHAR)) - 1 - (idx)])

#define BYTE_PUSH(val) do { stack_global_ensure(STACK_BYTE, 1); ((int64_t*)stack_global_get_stack(STACK_BYTE))[(*stack_global_get_sp(STACK_BYTE))++] = (val); } while(0)
#define BYTE_POP() (((int64_t*)stack_global_get_stack(STACK_BYTE))[--(*stack_global_get_sp(STACK_BYTE))])
#define BYTE_PEEK() (((int64_t*)stack_global_get_stack(STACK_BYTE))[(*stack_global_get_sp(STACK_BYTE)) - 1])
#define BYTE_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_BYTE))[(*stack_global_get_sp(STACK_BYTE)) - 1 - (idx)])


/* ========== 长整型类型专用栈操作 ========== */

#define LONG_PUSH(val) do { stack_global_ensure(STACK_LONG, 1); ((int64_t*)stack_global_get_stack(STACK_LONG))[(*stack_global_get_sp(STACK_LONG))++] = (val); } while(0)
#define LONG_POP() (((int64_t*)stack_global_get_stack(STACK_LONG))[--(*stack_global_get_sp(STACK_LONG))])
#define LONG_PEEK() (((int64_t*)stack_global_get_stack(STACK_LONG))[(*stack_global_get_sp(STACK_LONG)) - 1])
#define LONG_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_LONG))[(*stack_global_get_sp(STACK_LONG)) - 1 - (idx)])

#define ULONG_PUSH(val) do { stack_global_ensure(STACK_ULONG, 1); ((int64_t*)stack_global_get_stack(STACK_ULONG))[(*stack_global_get_sp(STACK_ULONG))++] = (val); } while(0)
#define ULONG_POP() (((int64_t*)stack_global_get_stack(STACK_ULONG))[--(*stack_global_get_sp(STACK_ULONG))])
#define ULONG_PEEK() (((int64_t*)stack_global_get_stack(STACK_ULONG))[(*stack_global_get_sp(STACK_ULONG)) - 1])
#define ULONG_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_ULONG))[(*stack_global_get_sp(STACK_ULONG)) - 1 - (idx)])

#define LONG_LONG_PUSH(val) do { stack_global_ensure(STACK_LONG_LONG, 1); ((int64_t*)stack_global_get_stack(STACK_LONG_LONG))[(*stack_global_get_sp(STACK_LONG_LONG))++] = (val); } while(0)
#define LONG_LONG_POP() (((int64_t*)stack_global_get_stack(STACK_LONG_LONG))[--(*stack_global_get_sp(STACK_LONG_LONG))])
#define LONG_LONG_PEEK() (((int64_t*)stack_global_get_stack(STACK_LONG_LONG))[(*stack_global_get_sp(STACK_LONG_LONG)) - 1])
#define LONG_LONG_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_LONG_LONG))[(*stack_global_get_sp(STACK_LONG_LONG)) - 1 - (idx)])

#define SIZE_T_PUSH(val) do { stack_global_ensure(STACK_SIZE_T, 1); ((int64_t*)stack_global_get_stack(STACK_SIZE_T))[(*stack_global_get_sp(STACK_SIZE_T))++] = (val); } while(0)
#define SIZE_T_POP() (((int64_t*)stack_global_get_stack(STACK_SIZE_T))[--(*stack_global_get_sp(STACK_SIZE_T))])
#define SIZE_T_PEEK() (((int64_t*)stack_global_get_stack(STACK_SIZE_T))[(*stack_global_get_sp(STACK_SIZE_T)) - 1])
#define SIZE_T_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_SIZE_T))[(*stack_global_get_sp(STACK_SIZE_T)) - 1 - (idx)])

#define SSIZE_T_PUSH(val) do { stack_global_ensure(STACK_SSIZE_T, 1); ((int64_t*)stack_global_get_stack(STACK_SSIZE_T))[(*stack_global_get_sp(STACK_SSIZE_T))++] = (val); } while(0)
#define SSIZE_T_POP() (((int64_t*)stack_global_get_stack(STACK_SSIZE_T))[--(*stack_global_get_sp(STACK_SSIZE_T))])
#define SSIZE_T_PEEK() (((int64_t*)stack_global_get_stack(STACK_SSIZE_T))[(*stack_global_get_sp(STACK_SSIZE_T)) - 1])
#define SSIZE_T_TOP(idx) (((int64_t*)stack_global_get_stack(STACK_SSIZE_T))[(*stack_global_get_sp(STACK_SSIZE_T)) - 1 - (idx)])

#define LONG_DOUBLE_PUSH(val) do { stack_global_ensure(STACK_LONG_DOUBLE, 1); ((long double*)stack_global_get_stack(STACK_LONG_DOUBLE))[(*stack_global_get_sp(STACK_LONG_DOUBLE))++] = (val); } while(0)
#define LONG_DOUBLE_POP() (((long double*)stack_global_get_stack(STACK_LONG_DOUBLE))[--(*stack_global_get_sp(STACK_LONG_DOUBLE))])
#define LONG_DOUBLE_PEEK() (((long double*)stack_global_get_stack(STACK_LONG_DOUBLE))[(*stack_global_get_sp(STACK_LONG_DOUBLE)) - 1])
#define LONG_DOUBLE_TOP(idx) (((long double*)stack_global_get_stack(STACK_LONG_DOUBLE))[(*stack_global_get_sp(STACK_LONG_DOUBLE)) - 1 - (idx)])

#endif /* LUMYR_VM_MACROS_H */
