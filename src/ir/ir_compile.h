#ifndef LUMYR_IR_COMPILE_H
#define LUMYR_IR_COMPILE_H

#include "bytecode.h"
#include "ast/ast_node.h"

// 编译一个 lum 函数体为字节码（yacc 期注册函数时调用）
BytecodeFunc* ir_compile_function(const char* name, AstNode* params, AstNode* body, int is_generator);
// 重编译已注册函数（typecheck 转换 AST_VAR→AST_FUNCREF 后原位替换字节码）
BytecodeFunc* ir_func_table_recompile(const char* name, AstNode* params, AstNode* body);

// 编译顶层语句为 main 字节码（执行 / -c 生成 C 共用同一 IR）
BytecodeFunc* ir_compile_main(AstNode* root);

// 全局函数表（ir_compile_function / ir_compile_main 注册；ir_cgen 遍历用）
void ir_func_table_reset(void);
BytecodeFunc* ir_func_table_lookup(const char* name);
/* 按 class_name + method_name 查找 class 方法（红黑树快速查找） */
BytecodeFunc* ir_func_table_lookup_class(const char* class_name, const char* method_name);
/* 遍历所有函数（红黑树中序遍历） */
void ir_func_table_foreach(void (*callback)(const char* class_name, const char* method_name, void* data, void* user_data), void* user_data);

// 字符串常量缓存（编译期全局去重，避免重复分配；编译完成后调用 reset 清理）
void string_cache_reset(void);

#endif // LUMYR_IR_COMPILE_H
