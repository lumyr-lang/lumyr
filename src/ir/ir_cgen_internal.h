#ifndef LUMYR_IR_CGEN_INTERNAL_H
#define LUMYR_IR_CGEN_INTERNAL_H

/* ir_cgen 内部头文件：所有拆分模块共享的内部函数和全局变量声明
 * 这些函数和变量不对外暴露，仅在 ir_cgen 系列模块内部使用 */

#include "bytecode.h"
#include "ir_cgen.h"
#include "lumyr_ffi.h"
#include "ir_compile.h"
#include "ast/lumyr_types.h"
#include "ast/ast_types.h"
#include "ast/func_compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* 常量定义 */
#define ITEMS_STACK_MAX 256
#define SCALAR_REPL_MAX 16

/* ---------------- 全局变量（跨模块共享） ---------------- */

/* 基础全局变量 */
extern FILE* out;
typedef struct { char** names; int count; int cap; } NameSet;
extern NameSet g_globals;
extern NameSet fn_locals;
extern BytecodeFunc* g_cur_fn;
extern BytecodeFunc* g_main_fn;  // main 函数（用于全局变量类型查找）

/* 逃逸分析结果 */
extern uint8_t* g_stack_alloc;
extern int g_stack_alloc_len;
extern uint8_t* g_items_stack_alloc;
extern int g_items_stack_alloc_len;
extern uint8_t* g_map_stack_alloc;
extern int g_map_stack_alloc_len;

/* 标量替换结果 */
extern uint8_t* g_scalar_var;
extern uint8_t* g_scalar_kind;
extern int* g_scalar_count;
extern int** g_scalar_keys;
extern int g_scalar_sym_cnt;

/* 闭包装箱分析结果 */
extern NameSet g_boxed;
extern NameSet g_cur_caps;

/* finally 标签 */
extern int fin_lab_cnt;
extern int* fin_lab_pcs;
extern int fin_lab_cap;

/* ---------------- 工具函数 ---------------- */

/* NameSet 操作 */
extern int ns_has(const NameSet* s, const char* name);
extern void ns_add(NameSet* s, const char* name);

/* 变量/参数判断 */
extern int fn_has_param(const BytecodeFunc* fn, const char* name);
extern int is_scalar_replaced_lit(const BytecodeFunc* fn, int i);

/* 捕获变量 */
extern int cap_index_of(const char* name);
extern int lambda_has_captures(const char* name);

/* 变量访问表达式生成 */
extern const char* cvar_rw(const char* name);
extern const char* cvar_ptr(const char* name);
extern const char* cell_ptr_expr(const char* name);

/* 函数表 */
extern BytecodeFunc* func_table_lookup(const char* name);

/* 常量发射 */
extern void emit_c_string_lit(FILE* f, const char* s);
extern void emit_c_char_lit(FILE* f, char ch);
extern void emit_const(FILE* f, const Value* v);

/* 变量收集 */
extern void scan_var_refs(BytecodeFunc* fn, NameSet* set, int include_load);
extern void collect_func_locals(BytecodeFunc* fn);

/* finally 标签 */
extern int fin_lab_idx_of(int pc);
extern int is_jump_target(BytecodeFunc* fn, int idx);

/* ---------------- 逃逸分析与标量替换 ---------------- */
extern int ea_is_global(BytecodeFunc* fn, int sym_idx);
extern void analyze_escape_for(BytecodeFunc* fn, int target_op,
                                uint8_t* stack_alloc_out, uint8_t* items_alloc_out,
                                int is_array);
extern void analyze_scalar_replacement(BytecodeFunc* fn);

/* ---------------- 闭包装箱分析 ---------------- */
extern void analyze_boxing(BytecodeFunc* fn);

/* ---------------- struct 转换 ---------------- */
extern void emit_struct_to_value(const char* struct_name, const char* var_expr);
extern void emit_value_to_struct(const char* struct_name, const char* var_expr, const char* value_expr);

/* ---------------- 指令发射 ---------------- */
extern void emit_insns(BytecodeFunc* fn);

/* ---------------- 函数生成 ---------------- */
extern void emit_func_proto(BytecodeFunc* fn);
extern void emit_func_def(BytecodeFunc* fn);
extern void emit_func_wraps(void);
extern void emit_main(BytecodeFunc* main_fn);

/* ---------------- 生成器代码生成（ir_cgen_generator.c） ---------------- */
extern int g_gen_yield_count;
extern int g_is_generator;        /* 当前是否在生成器函数中（局部变量访问加 g-> 前缀） */
extern int count_yields(const BytecodeFunc* fn);
extern void emit_gen_struct(BytecodeFunc* fn);
extern void emit_gen_create(BytecodeFunc* fn);
extern void emit_gen_next_header(BytecodeFunc* fn);
extern void emit_gen_next_footer(BytecodeFunc* fn);
extern void emit_gen_yield(BytecodeFunc* fn, int yield_id);
extern void emit_gen_wrapper_support(void);
extern void gen_set_try_labels(int* labels, int count);  /* 设置当前生成器的 try-catch 块 catch 标签列表 */

#endif // LUMYR_IR_CGEN_INTERNAL_H
