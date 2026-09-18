// lumyr-lang IR 编译器
// 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
// 清空重写：保留接口，只实现核心框架

#include "ir_compile.h"
#include "ir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * 全局函数表（简化版：用数组存储）
 * ============================================================ */

#define FUNC_TABLE_CAP 1024
static BytecodeFunc* g_func_table[FUNC_TABLE_CAP];
static int g_func_cnt = 0;

void ir_func_table_reset(void) {
    g_func_cnt = 0;
    memset(g_func_table, 0, sizeof(g_func_table));
}

BytecodeFunc* ir_func_table_lookup(const char* name) {
    for(int i = 0; i < g_func_cnt; i++) {
        if(g_func_table[i] && g_func_table[i]->name && strcmp(g_func_table[i]->name, name) == 0) {
            return g_func_table[i];
        }
    }
    return NULL;
}

BytecodeFunc* ir_func_table_lookup_class(const char* class_name, const char* method_name) {
    (void)class_name; (void)method_name;
    return NULL;
}

BytecodeFunc* ir_func_table_lookup_any(const char* name) {
    return ir_func_table_lookup(name);
}

void ir_func_table_foreach(void (*callback)(const char*, const char*, void*, void*), void* user_data) {
    (void)callback; (void)user_data;
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
    (void)params; (void)body; (void)is_generator; (void)class_name;
    
    BytecodeFunc* fn = calloc(1, sizeof(BytecodeFunc));
    fn->name = strdup(name);
    
    if(g_func_cnt < FUNC_TABLE_CAP) {
        g_func_table[g_func_cnt++] = fn;
    }
    
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
