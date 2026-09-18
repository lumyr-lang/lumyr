#ifndef LUMYR_IR_ARITH_H
#define LUMYR_IR_ARITH_H

#include "ir_types.h"
#include "bytecode.h"

/*
 * 运算优化模块
 *
 * 本模块负责各种数据类型的算术运算和比较运算的零开销优化，
 * 包括类型判断、指令发射、运算结果处理等。
 *
 * 支持的数据类型：
 * - 有符号整数：int、int8、int16、int32、int64、long long、long、ssize_t
 * - 无符号整数：uint、uint8、uint16、uint32、uint64、unsigned long、size_t、byte
 * - 浮点数：float、double、long double
 * - 其他：bool、char
 */

/* ========== 类型判断函数 ========== */

/* 判断变量是否为指定类型（通过变量类型标记） */
int arith_is_int_var(Ctx* c, AstNode* node);
int arith_is_uint_var(Ctx* c, AstNode* node);
int arith_is_double_var(Ctx* c, AstNode* node);
int arith_is_float_var(Ctx* c, AstNode* node);
int arith_is_long_long_var(Ctx* c, AstNode* node);
int arith_is_long_double_var(Ctx* c, AstNode* node);
int arith_is_bool_var(Ctx* c, AstNode* node);
int arith_is_char_var(Ctx* c, AstNode* node);
int arith_is_byte_var(Ctx* c, AstNode* node);
int arith_is_int8_var(Ctx* c, AstNode* node);
int arith_is_int16_var(Ctx* c, AstNode* node);
int arith_is_short_var(Ctx* c, AstNode* node);
int arith_is_int32_var(Ctx* c, AstNode* node);
int arith_is_int64_var(Ctx* c, AstNode* node);
int arith_is_uint8_var(Ctx* c, AstNode* node);
int arith_is_uint16_var(Ctx* c, AstNode* node);
int arith_is_uchar_var(Ctx* c, AstNode* node);
int arith_is_ushort_var(Ctx* c, AstNode* node);
int arith_is_uint32_var(Ctx* c, AstNode* node);
int arith_is_uint64_var(Ctx* c, AstNode* node);
int arith_is_long_var(Ctx* c, AstNode* node);
int arith_is_ulong_var(Ctx* c, AstNode* node);
int arith_is_size_t_var(Ctx* c, AstNode* node);
int arith_is_ssize_t_var(Ctx* c, AstNode* node);

/* ========== 表达式类型判断 ========== */

/* 获取表达式的类型（用于算术运算结果的上下文感知） */
ExprType arith_get_expr_type(Ctx* c, AstNode* node);

/* ========== 算术运算和比较运算优化 ========== */

/*
 * 尝试对二元运算进行零开销优化
 * 返回 1 表示已优化（发射了专用指令），返回 0 表示未优化（需要走通用路径）
 *
 * 参数：
 *   c - 编译上下文
 *   node - 二元运算 AST 节点
 *   bop - 运算符（OP_ADD、OP_SUB 等）
 */
int arith_try_optimize_binop(Ctx* c, AstNode* node, int bop);

/* ========== 运算结果处理 ========== */

/*
 * 处理赋值场景中的算术运算结果
 * 根据运算结果类型选择对应的专用存储指令
 *
 * 参数：
 *   c - 编译上下文
 *   binop - 二元运算 AST 节点
 *   var_idx - 变量索引
 *   result_type - 运算结果类型（EXPR_TYPE_*）
 */
void arith_handle_assign_result(Ctx* c, AstNode* binop, int var_idx, ExprType result_type);

/*
 * 处理 print 场景中的算术运算结果
 * 根据运算结果类型选择对应的专用打印指令
 *
 * 参数：
 *   c - 编译上下文
 *   single_arg - print 的参数表达式
 *   result_type - 运算结果类型（EXPR_TYPE_*）
 */
int arith_handle_print_result(Ctx* c, AstNode* single_arg, ExprType result_type);

#endif /* LUMYR_IR_ARITH_H */

/* 获取变量类型标记（定义在 ir_arith.c） */
CastKind get_var_cast_type(Ctx* c, const char* name);
