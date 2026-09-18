// lumyr-lang IR 编译器
// 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
// 清空重写：保留接口，只实现核心框架

#include "ir_compile.h"
#include "ir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * 全局函数表（红黑树存储，无硬上限）
 * 支持：普通函数、类方法、静态方法、构造函数
 * 键：(class_name, method_name) 复合键
 * ============================================================ */

#include "rbtree.h"

static RBTree* g_func_tree = NULL;

/* 初始化函数表 */
static void func_table_init(void) {
    if(!g_func_tree) {
        g_func_tree = rbtree_create();
    }
}

void ir_func_table_reset(void) {
    if(g_func_tree) {
        rbtree_destroy(g_func_tree);
        g_func_tree = NULL;
    }
    func_table_init();
}

BytecodeFunc* ir_func_table_lookup(const char* name) {
    func_table_init();
    return (BytecodeFunc*)rbtree_find(g_func_tree, NULL, name);
}

BytecodeFunc* ir_func_table_lookup_class(const char* class_name, const char* method_name) {
    func_table_init();
    return (BytecodeFunc*)rbtree_find(g_func_tree, class_name, method_name);
}

BytecodeFunc* ir_func_table_lookup_any(const char* name) {
    func_table_init();
    /* 先查找普通函数 */
    BytecodeFunc* fn = (BytecodeFunc*)rbtree_find(g_func_tree, NULL, name);
    if(fn) return fn;
    /* 再查找任意类的方法（简化：只查第一个匹配） */
    /* TODO: 红黑树支持前缀查找 */
    return NULL;
}

void ir_func_table_foreach(void (*callback)(const char*, const char*, void*, void*), void* user_data) {
    func_table_init();
    rbtree_foreach(g_func_tree, callback, user_data);
}

/* ============================================================
 * 字符串常量缓存
 * ============================================================ */

void string_cache_reset(void) {
    // 简化版：不做缓存
}

/* ============================================================
 * 内部辅助函数
 * ============================================================ */

/* emit, bf_sym, bf_const 在 ir_emit.c 和 bytecode.c 中定义 */

/* ============================================================
 * 编译表达式
 * ============================================================ */

void c_expr(Ctx* c, AstNode* node) {
    if(!c || !node) return;

    /* 整数字面量 */
    if(node->type == AST_INT) {
        emit(c, OPC_LOAD_CONST, 0, 0);  // 简化：先返回0
        return;
    }

    /* 变量引用 */
    if(node->type == AST_VAR) {
        int var_idx = bf_sym(c->fn, node->u.varname);  /* 外部函数 */
        emit(c, OPC_LOAD_VAR, var_idx, 0);
        return;
    }
}

/* ============================================================
 * 主编译入口
 * ============================================================ */

BytecodeFunc* ir_compile_function(const char* name, AstNode* params, AstNode* body, int is_generator, const char* class_name) {
    (void)params; (void)body; (void)is_generator;
    
    func_table_init();
    
    BytecodeFunc* fn = calloc(1, sizeof(BytecodeFunc));
    fn->name = strdup(name);
    
    /* 插入红黑树：class_name 为 NULL 表示普通函数 */
    rbtree_insert(g_func_tree, class_name, name, fn);
    
    return fn;
}

BytecodeFunc* ir_func_table_recompile(const char* name, AstNode* params, AstNode* body) {
    (void)params; (void)body;
    return ir_func_table_lookup(name);
}

BytecodeFunc* ir_compile_main(AstNode* root) {
    (void)root;
    
    BytecodeFunc* fn = calloc(1, sizeof(BytecodeFunc));
    fn->is_main = 1;
    
    return fn;
}
