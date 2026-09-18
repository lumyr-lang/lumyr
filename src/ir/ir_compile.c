// lumyr-lang IR 编译器
// 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
#include "ir_compile.h"
#include "ir_types.h"
#include "ir_arith.h"
#include "rbtree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * 全局符号表（红黑树，三维复合键）
 * ============================================================ */

static RBTree* g_sym_tree = NULL;

static void sym_table_init(void) {
    if(!g_sym_tree) g_sym_tree = rbtree_create();
}

void ir_func_table_reset(void) {
    if(g_sym_tree) { rbtree_destroy(g_sym_tree); g_sym_tree = NULL; }
    sym_table_init();
}

BytecodeFunc* ir_func_table_lookup(const char* name) {
    sym_table_init();
    return (BytecodeFunc*)rbtree_find(g_sym_tree, NS_FUNCTION, NULL, name);
}

BytecodeFunc* ir_func_table_lookup_class(const char* class_name, const char* method_name) {
    sym_table_init();
    return (BytecodeFunc*)rbtree_find(g_sym_tree, NS_METHOD, class_name, method_name);
}

BytecodeFunc* ir_func_table_lookup_any(const char* name) {
    sym_table_init();
    /* 先查普通函数 */
    BytecodeFunc* fn = (BytecodeFunc*)rbtree_find(g_sym_tree, NS_FUNCTION, NULL, name);
    if(fn) return fn;
    /* 再查构造函数（与类名同名） */
    fn = (BytecodeFunc*)rbtree_find(g_sym_tree, NS_CONSTRUCTOR, name, name);
    return fn;
}

void ir_func_table_foreach(void (*callback)(const char*, const char*, void*, void*), void* user_data) {
    sym_table_init();
    /* 简化：只遍历函数 */
    rbtree_foreach_ns(g_sym_tree, NS_FUNCTION, 
        (void (*)(RBTNamespace, const char*, const char*, void*, void*))callback, user_data);
}

void string_cache_reset(void) {}

/* ============================================================
 * 内部辅助函数
 * ============================================================ */

/* bf_sym, bf_const 在 bytecode.c 中定义 */

/* ============================================================
 * 表达式编译（4 核心栈设计）
 * ============================================================ */

/* get_var_cast_type 在 ir_arith.c 中定义 */

/* 编译表达式，根据类型选择专用栈 */
void c_expr(Ctx* c, AstNode* node) {
    if(!c || !node) return;

    /* 整数字面量 → INT64 栈 */
    if(node->type == AST_INT) {
        long long val = node->u.inum;
        emit(c, OPC_PUSH_INT64_CONST, (int)(val & 0xFFFFFFFF), (int)((val >> 32) & 0xFFFFFFFF));
        return;
    }

    /* 浮点字面量 → DOUBLE 栈 */
    if(node->type == AST_NUM) {
        double val = node->u.num;
        uint64_t bits = 0;
        memcpy(&bits, &val, sizeof(double));
        emit(c, OPC_PUSH_DOUBLE_CONST, (int)(bits & 0xFFFFFFFF), (int)((bits >> 32) & 0xFFFFFFFF));
        return;
    }

    /* 布尔字面量 → INT64 栈（0/1） */
    if(node->type == AST_BOOL) {
        emit(c, OPC_PUSH_INT64_CONST, node->u.bval ? 1 : 0, 0);
        return;
    }

    /* 字符串字面量 → 简化：先走 Value 栈 */
    if(node->type == AST_STRING) {
        emit(c, OPC_LOAD_CONST, 0, 0);  /* TODO: 字符串常量 */
        return;
    }

    /* 变量引用：根据类型标记选择专用栈 */
    if(node->type == AST_VAR) {
        int var_idx = bf_sym(c->fn, node->u.varname);
        CastKind ct = get_var_cast_type(c, node->u.varname);
        
        /* 根据类型标记选择专用栈 */
        if(ct >= CAST_INT && ct <= CAST_SSIZE_T) {
            /* 整数类型 → INT64 栈 */
            emit(c, OPC_LOAD_INT64_VAR, var_idx, 0);
        } else if(ct == CAST_FLOAT || ct == CAST_DOUBLE || ct == CAST_LONG_DOUBLE) {
            /* 浮点类型 → DOUBLE 栈 */
            emit(c, OPC_LOAD_DOUBLE_VAR, var_idx, 0);
        } else if(ct == CAST_STRING || ct == CAST_ASCII) {
            /* 字符串类型 → PTR 栈 */
            emit(c, OPC_LOAD_PTR_VAR, var_idx, 0);
        } else {
            /* 动态类型 → Value 栈 */
            emit(c, OPC_LOAD_VAR, var_idx, 0);
        }
        return;
    }

    /* 二元运算（简化版） */
    if(node->type == AST_BINOP) {
        ExprType left_type = arith_get_expr_type(c, node->u.bin.left);
        ExprType right_type = arith_get_expr_type(c, node->u.bin.right);
        
        /* 递归编译左右操作数 */
        c_expr(c, node->u.bin.left);
        c_expr(c, node->u.bin.right);
        
        /* 简化：所有二元运算都用 OP_ADD，后续完善 */
        emit(c, OPC_ADD, 0, 0);
        return;
    }
}

/* ============================================================
 * 语句编译
 * ============================================================ */

/* 编译赋值语句 */
static void c_assign(Ctx* c, AstNode* node) {
    if(!c || !node || node->type != AST_ASSIGN) return;
    
    /* 赋值：varname = expr */
    const char* lhs_name = node->u.assign.varname;
    AstNode* rhs = node->u.assign.expr;
    
    int var_idx = bf_sym(c->fn, lhs_name);
    
    /* 获取右操作数类型 */
    ExprType rhs_type = arith_get_expr_type(c, rhs);
    
    /* 编译右操作数 */
    c_expr(c, rhs);
    
    /* 根据类型选择存储指令 */
    switch(rhs_type) {
        case EXPR_TYPE_INT:
            /* 整数 → INT64 栈 */
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_INT;
            break;
            
        case EXPR_TYPE_DOUBLE:
            /* 浮点 → DOUBLE 栈 */
            emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_DOUBLE;
            break;
            
        case EXPR_TYPE_PTR:
            /* 指针 → PTR 栈 */
            emit(c, OPC_STORE_PTR_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_STRING;
            break;
            
        default:
            /* 动态类型 → Value 栈 */
            emit(c, OPC_STORE_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = -1;
            break;
    }
}

/* 编译 print 语句 */
static void c_print(Ctx* c, AstNode* node) {
    if(!c || !node) return;
    
    /* 简化：编译表达式，然后打印 */
    /* 后续完善 */
}

/* ============================================================
 * 主编译入口
 * ============================================================ */

BytecodeFunc* ir_compile_function(const char* name, AstNode* params, AstNode* body, int is_generator, const char* class_name) {
    (void)params; (void)body; (void)is_generator;
    
    sym_table_init();
    
    BytecodeFunc* fn = calloc(1, sizeof(BytecodeFunc));
    fn->name = strdup(name);
    
    /* 根据是否有 class_name 选择命名空间 */
    RBTNamespace ns = class_name ? NS_METHOD : NS_FUNCTION;
    rbtree_insert(g_sym_tree, ns, class_name, name, fn);
    
    return fn;
}

BytecodeFunc* ir_func_table_recompile(const char* name, AstNode* params, AstNode* body) {
    (void)params; (void)body;
    return ir_func_table_lookup(name);
}

BytecodeFunc* ir_compile_main(AstNode* root) {
    sym_table_init();
    
    BytecodeFunc* fn = calloc(1, sizeof(BytecodeFunc));
    fn->is_main = 1;
    
    return fn;
}
