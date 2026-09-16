#ifndef LUMYR_IR_TYPES_H
#define LUMYR_IR_TYPES_H

#include "bytecode.h"
#include "ast/ast_node.h"

/*
 * IR 模块共用的结构体和枚举定义
 *
 * 本文件包含 IR 编译、代码生成、虚拟机等模块共用的结构体和枚举定义，
 * 避免在多个 .c 文件中重复定义，提高代码的可维护性。
 */

/* ========== 表达式类型枚举 ========== */

/*
 * 表达式类型枚举，用于算术运算结果的上下文感知和类型提升
 * 数值大小对应类型优先级，用于混合类型运算时的类型提升
 *
 * 注意：这个枚举不复用 ValueType，因为 ValueType 的数值大小不对应类型优先级，
 * 不能直接用于类型提升判断。
 */
typedef enum {
    EXPR_TYPE_NONE = 0,
    EXPR_TYPE_BOOL = 1,
    EXPR_TYPE_CHAR = 2,
    EXPR_TYPE_INT8 = 3,
    EXPR_TYPE_INT16 = 4,
    EXPR_TYPE_INT = 5,
    EXPR_TYPE_INT64 = 6,
    EXPR_TYPE_LONG_LONG = 7,
    EXPR_TYPE_LONG = 8,
    EXPR_TYPE_BYTE = 9,
    EXPR_TYPE_UINT8 = 10,
    EXPR_TYPE_UINT16 = 11,
    EXPR_TYPE_UINT = 12,
    EXPR_TYPE_UINT64 = 13,
    EXPR_TYPE_ULONG = 14,
    EXPR_TYPE_SIZE_T = 15,
    EXPR_TYPE_SSIZE_T = 16,
    EXPR_TYPE_FLOAT = 17,
    EXPR_TYPE_DOUBLE = 18,
    EXPR_TYPE_LONG_DOUBLE = 19,
    EXPR_TYPE_COUNT
} ExprType;

/* ========== 控制层结构体 ========== */

/*
 * 控制层（循环/switch/try-finally 嵌套）
 * 用于管理 break、continue、try-finally 等控制流的跳转位置
 */
typedef struct {
    int kind;              /* 0=循环 1=switch */
    int* brk; int brk_cnt, brk_cap;    /* 未定 break 跳转位置（JMP.a） */
    int* cont; int cont_cnt, cont_cap; /* 未定 continue 跳转位置（循环，JMP.a） */
    int* brk_fin; int brk_fin_cnt, brk_fin_cap;   /* try-finally 内 break 的 FIN_PUSH 位置（patch b） */
    int* cont_fin; int cont_fin_cnt, cont_fin_cap; /* try-finally 内 continue 的 FIN_PUSH 位置（patch b） */
    int cont_target;       /* 已知 continue 目标（while 的 cond 开头）或 -1 */
} Layer;

/* ========== 编译上下文结构体 ========== */

/*
 * 编译上下文
 * 包含当前编译的函数、控制层嵌套、finally 上下文等信息
 */
typedef struct {
    BytecodeFunc* fn;
    Layer* layers;           /* 动态：循环/switch 嵌套无硬上限 */
    int layer_depth;
    int layers_cap;
    /* finally 上下文：fin_depth>0 表示当前编译位置在 try-finally 内；
       fin_pend[depth][*] = body/catch 中 PEND_RETURN(b=0) 的位置，fstart 确定后统一 patch b
       行/列均动态扩容，try-finally 嵌套与每层挂起数无硬上限 */
    int** fin_pend;
    int* fin_pend_n;
    int* fin_pend_cap;
    int** fin_jmp;            /* try-finally 内 break/continue 的 JMP 位置（patch a=fstart） */
    int* fin_jmp_n;
    int* fin_jmp_cap;
    int fin_depth;
    int fin_cap;
} Ctx;

#endif /* LUMYR_IR_TYPES_H */
