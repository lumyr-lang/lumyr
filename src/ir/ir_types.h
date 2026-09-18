#ifndef LUMYR_IR_TYPES_H
#define LUMYR_IR_TYPES_H

#include "bytecode.h"
#include "ast/ast_node.h"

/*
 * IR 模块共用的结构体和枚举定义
 * 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
 */

/* ========== 表达式类型枚举（4 核心栈设计） ========== */

/*
 * 表达式类型枚举，用于类型推断和栈选择
 * 所有细分整数类型合并到 INT，所有浮点类型合并到 DOUBLE
 * 所有指针/字符串类型合并到 PTR
 */
typedef enum {
    EXPR_TYPE_NONE = 0,    /* 动态类型 → Value 栈 */
    EXPR_TYPE_INT = 1,     /* 整数类型 → INT64 栈（所有整数/布尔/字符） */
    EXPR_TYPE_DOUBLE = 2,  /* 浮点类型 → DOUBLE 栈（所有浮点） */
    EXPR_TYPE_PTR = 3,     /* 指针类型 → PTR 栈（所有指针/字符串） */
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
    /* finally 上下文：fin_depth>0 表示当前编译位置在 try-finally 内 */
    int** fin_pend;
    int* fin_pend_n;
    int* fin_pend_cap;
    int** fin_jmp;
    int* fin_jmp_n;
    int* fin_jmp_cap;
    int fin_depth;
    int fin_cap;
} Ctx;

#endif /* LUMYR_IR_TYPES_H */
