#ifndef LUMYR_IR_COMPILE_H
#define LUMYR_IR_COMPILE_H

#include "bytecode.h"
#include "ast/ast_node.h"
#include "ir_types.h"
#include "ir_emit.h"

/* ========== 函数声明 ========== */

/* 编译表达式（递归），返回表达式类型 */
ExprType c_expr(Ctx* c, AstNode* node);

// 编译一个 lum 函数体为字节码（yacc 期注册函数时调用）
BytecodeFunc* ir_compile_function(const char* name, AstNode* params, AstNode* body, int is_generator, const char* class_name, const char* ret_type_name);
// 重编译已注册函数（typecheck 转换 AST_VAR→AST_FUNCREF 后原位替换字节码）
BytecodeFunc* ir_func_table_recompile(const char* name, AstNode* params, AstNode* body, int is_generator, const char* class_name, const char* ret_type_name);

// 编译顶层语句为 main 字节码（执行 / -c 生成 C 共用同一 IR）
BytecodeFunc* ir_compile_main(AstNode* root);

// 全局函数表（ir_compile_function / ir_compile_main 注册；ir_cgen 遍历用）
void ir_func_table_reset(void);
/* 注册/替换普通函数（同名旧函数被释放） */
void ir_func_table_register(BytecodeFunc* fn);
BytecodeFunc* ir_func_table_lookup(const char* name);
/* 按 class_name + method_name 查找 class 方法（红黑树快速查找） */
BytecodeFunc* ir_func_table_lookup_class(const char* class_name, const char* method_name);
/* 查找任意函数（先查找普通函数，如果找不到，再按名字查找第一个匹配的 class 方法）
   用于 CC 模式的代码生成器 */
BytecodeFunc* ir_func_table_lookup_any(const char* name);
/* 遍历所有函数（红黑树中序遍历） */
void ir_func_table_foreach(void (*callback)(const char* class_name, const char* method_name, void* data, void* user_data), void* user_data);

// 字符串常量缓存（编译期全局去重，避免重复分配；编译完成后调用 reset 清理）
void string_cache_reset(void);

/* 检测 node 是否是 self.field 访问；若是则返回字段 CastKind，否则 CAST_NONE
 * 供 arith_get_expr_type / c_expr_cast_type 统一识别 self.field 类型 */
CastKind lumyr_self_field_castkind(Ctx* c, AstNode* node);

#endif // LUMYR_IR_COMPILE_H
