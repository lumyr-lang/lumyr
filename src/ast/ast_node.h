#ifndef AST_NODE_H
#define AST_NODE_H

#include "lumyr_types.h"
#include "ast_node_type.h"

// 创建AST节点
AstNode* ast_int(long long v);
AstNode* ast_num(double v);
AstNode* ast_bool(_Bool v);
AstNode* ast_none(void);
AstNode* ast_funcref(const char* name);
AstNode* ast_new_char(char c);
AstNode* ast_string(const char* s);
AstNode* ast_var(char* name);
AstNode* ast_binop(BinOp op, AstNode* l, AstNode* r);
AstNode* ast_assign(char* name, AstNode* e);
AstNode* ast_print(AstNode* args);
AstNode* ast_seq(AstNode* a, AstNode* b);
AstNode* ast_seq_append(AstNode* list, AstNode* item);
void ast_collect_varnames(AstNode* seq, char*** out_names, int* out_count);
AstNode* ast_if(AstNode* cond, AstNode* then_stmt, AstNode* elif_chain, AstNode* else_stmt);
AstNode* ast_block(AstNode* stmts);
AstNode* ast_elif(AstNode* cond, AstNode* body);
AstNode* ast_elif_append(AstNode* list, AstNode* elif);
AstNode* ast_if_chain(AstNode* cond, AstNode* if_body, AstNode* elif_list, AstNode* else_body);
AstNode* ast_new(AstType type);
AstNode* ast_while(AstNode* cond, AstNode* body);
AstNode* ast_do_while(AstNode* cond, AstNode* body);
AstNode* ast_annotation(char* name, AstNode* args);
AstNode* ast_safe_call(AstNode* obj, char* method, AstNode* args);
AstNode* ast_null_coalesce(AstNode* left, AstNode* right);
AstNode* ast_macro_def(char* name, AstNode* params, AstNode* body);
AstNode* ast_for(AstNode* init, AstNode* cond, AstNode* update, AstNode* body);
AstNode* new_cast_node(int cast_type, AstNode* child);
AstNode* ast_unary(BinOp op, AstNode* child);
AstNode* ast_ternary(AstNode* cond, AstNode* t, AstNode* f);
AstNode* ast_switch(AstNode* expr, AstNode* cases);
AstNode* ast_case(AstNode* const_expr, AstNode* body, int is_default);
AstNode* ast_case_type(int match_type, AstNode* body);  /* type match: case int: etc */
AstNode* ast_case_guard(const char* bind_var, AstNode* guard, AstNode* body);  /* pattern binding + guard: case x if cond: */
AstNode* ast_break(void);
AstNode* ast_continue(void);
AstNode* ast_return(AstNode* expr);
AstNode* ast_yield(AstNode* value);
AstNode* ast_case_append(AstNode* case_list, AstNode* one_case);

// func def: func name(params) body
AstNode* ast_func_def(char* name, AstNode* params, AstNode* body);
AstNode* ast_extern_func(char* name, AstNode* params, char* ret_type_name, char* libname);
// 类型标注 <type>expr：给变量打类型标记（等价 C 的类型声明）
AstNode* ast_type_annotation(int cast_type, AstNode* expr);
AstNode* ast_interface_annotation(char* interface_name, AstNode* expr);
// 形参；is_ellipsis=1代表 ...args
AstNode* ast_param(char* name, int is_ellipsis, AstNode* default_val);
AstNode* ast_param_append(AstNode* list, AstNode* p);
AstNode* ast_param_constraint(char* name, char* constraint);

// 函数调用
AstNode* ast_call(char* func_name, AstNode* arg_list);
AstNode* ast_dyn_call(AstNode* callee, AstNode* arg_list);
AstNode* ast_seq_front(AstNode* chain, AstNode* recv);  // 实参链头部插入接收者（方法链 a.b(x)→b(a,x)）
AstNode* ast_arg_append(AstNode* list, AstNode* arg);

// 数组
AstNode* ast_index(AstNode* arr, AstNode* idx);
AstNode* ast_index_assign(AstNode* arr, AstNode* idx, AstNode* value);
AstNode* ast_array_lit(AstNode* elems, int elem_type);
AstNode* ast_map_lit(AstNode* entries);
AstNode* ast_class_new(char* class_name, int argc, AstNode* args);
AstNode* ast_try(AstNode* body, char* catch_var, AstNode* catch_body, AstNode* finally_body);
AstNode* ast_try_multi(AstNode* body, CatchClause* catches, int catch_count, AstNode* finally_body);
AstNode* ast_throw(AstNode* expr);
AstNode* ast_destruct(char** names, int count, AstNode* rhs);
AstNode* ast_spread(AstNode* expr);
AstNode* ast_map_entry(AstNode* key, AstNode* value);

// 深拷贝（复合赋值下标展开防双重释放）
AstNode* ast_clone_node(const AstNode* src);

// 释放AST
void ast_free(AstNode* node);

#endif
