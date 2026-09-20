// lumyr-lang 字节码 IR 定义
// 基于栈的 VM：表达式求值压栈，跳转指令用绝对 pc 目标。
#ifndef LUMYR_IR_BYTECODE_H
#define LUMYR_IR_BYTECODE_H

#include "bytecode_type.h"

/* ============================================================
 * BytecodeFunc 操作函数
 * ============================================================ */
BytecodeFunc* bytecode_func_new(const char* name, int is_main);
void bytecode_func_free(BytecodeFunc* fn);
int bf_sym(BytecodeFunc* fn, const char* name);
int bf_add_i64_const(BytecodeFunc* fn, int64_t val);
int bf_add_double_const(BytecodeFunc* fn, double val);
int bf_add_str_const(BytecodeFunc* fn, const char* s);
/* 添加 uint64 到大常量池（用于 RuntimeTypeInfo* 指针存储），返回索引 */
int bf_add_u64_const(BytecodeFunc* fn, uint64_t val);
void bf_emit(BytecodeFunc* fn, OpCode op, int a, int b);
int bf_emit_here(BytecodeFunc* fn, OpCode op, int a, int b);
const char* opc_name(OpCode op);
void bf_patch(BytecodeFunc* fn, int pos, int target);
void bf_patch_b(BytecodeFunc* fn, int pos, int target);
int bf_add_callsite(BytecodeFunc* fn, const char* callee, int argc, int keep_result, int ret_stack);

/* ============================================================
 * 静态栈深度分析
 * ============================================================ */
// 静态栈深度分析：计算每条指令执行前的栈深（写入 depth_out，可 NULL），
// 返回整个函数的最大栈深。IR 生成正确时每点栈深确定；不可达指令深度记 0。
int bc_analyze_stack(BytecodeFunc* fn, int* depth_out, int depth_cap);

/* ============================================================
 * 反汇编：输出指令文本（-S 模式）
 * ============================================================ */
void bc_disasm(FILE* out, BytecodeFunc* fn);

#endif // LUMYR_IR_BYTECODE_H
