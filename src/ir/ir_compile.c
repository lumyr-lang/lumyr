/*
 * ir_compile.c - IR 编译器
 * 将 AST 编译为字节码，4 核心栈设计
 */
#include "ir_compile.h"
#include "ir_arith.h"
#include "ir_types.h"
#include "bytecode_type.h"
#include "stack_manager.h"
#include "rbtree.h"
#include "ast/ast_node.h"
#include "ast/ast_node_type.h"
#include "ast/stackframe.h"
#include "ast/ast_runtime_sym.h"
#include "lm_value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 前向声明 */
void c_stmt(Ctx* c, AstNode* node);
ExprType c_expr(Ctx* c, AstNode* node);

/* ============================================================
 * 表达式编译
 * ============================================================ */

/* 添加常量到常量池，返回索引 */
static int c_add_const(Ctx* c, Value v) {
    BytecodeFunc* fn = c->fn;
    if(fn->const_cnt >= fn->const_cap) {
        fn->const_cap = fn->const_cap ? fn->const_cap * 2 : 16;
        fn->consts = realloc(fn->consts, fn->const_cap * sizeof(Value));
    }
    int idx = fn->const_cnt++;
    fn->consts[idx] = v;
    return idx;
}

/* 添加字符串到字符串常量池，返回索引 */
static int c_add_string_const(Ctx* c, const char* s) {
    BytecodeFunc* fn = c->fn;
    if(fn->str_const_cnt >= fn->str_const_cap) {
        fn->str_const_cap = fn->str_const_cap ? fn->str_const_cap * 2 : 16;
        fn->string_consts = realloc(fn->string_consts, fn->str_const_cap * sizeof(const char*));
    }
    int idx = fn->str_const_cnt++;
    fn->string_consts[idx] = s;
    return idx;
}

/* 查找变量索引，返回 -1 表示未找到 */
static int c_find_var(Ctx* c, const char* name) {
    for(int i = 0; i < c->var_cnt; i++) {
        if(strcmp(c->var_names[i], name) == 0) return i;
    }
    return -1;
}

/* 注册新变量，返回索引 */
static int c_add_var(Ctx* c, const char* name, ExprType type) {
    int idx = c_find_var(c, name);
    if(idx >= 0) {
        /* 已存在，更新类型 */
        c->var_types[idx] = type;
        return idx;
    }
    /* 新增到 Ctx 符号表 */
    if(c->var_cnt >= c->var_cap) {
        c->var_cap = c->var_cap ? c->var_cap * 2 : 16;
        c->var_names = realloc(c->var_names, c->var_cap * sizeof(char*));
        c->var_types = realloc(c->var_types, c->var_cap * sizeof(ExprType));
    }
    idx = c->var_cnt++;
    c->var_names[idx] = strdup(name);
    c->var_types[idx] = type;
    /* 同步到 BytecodeFunc 符号表（供 arith_get_expr_type 使用） */
    int bf_idx = bf_sym(c->fn, name);
    c->fn->var_type_tags[bf_idx] = (int)type;
    return idx;
}

/* 编译表达式，返回表达式类型 */
ExprType c_expr(Ctx* c, AstNode* node) {
    if(!node) return EXPR_TYPE_NONE;
    
    switch(node->type) {
    case AST_INT: {
        /* 整数字面量：压入 INT64 栈 */
        int64_t val = node->u.inum;
        int low = (int)(val & 0xFFFFFFFF);
        int high = (int)((val >> 32) & 0xFFFFFFFF);
        emit(c, OPC_PUSH_INT64_CONST, low, high);
        return EXPR_TYPE_INT;
    }

    case AST_NUM: {
        /* 浮点数字面量：压入 DOUBLE 栈 */
        double val = node->u.num;
        uint64_t bits;
        memcpy(&bits, &val, sizeof(double));
        int low = (int)(bits & 0xFFFFFFFF);
        int high = (int)((bits >> 32) & 0xFFFFFFFF);
        emit(c, OPC_PUSH_DOUBLE_CONST, low, high);
        return EXPR_TYPE_DOUBLE;
    }

    case AST_BOOL: {
        /* 布尔字面量：压入 INT64 栈（1=真，0=假） */
        int64_t val = node->u.bval ? 1 : 0;
        int low = (int)(val & 0xFFFFFFFF);
        int high = (int)((val >> 32) & 0xFFFFFFFF);
        emit(c, OPC_PUSH_INT64_CONST, low, high);
        return EXPR_TYPE_INT;
    }

    case AST_CHAR: {
        /* 字符字面量：压入 INT64 栈（字符编码值） */
        int64_t val = (int64_t)node->u.ch;
        int low = (int)(val & 0xFFFFFFFF);
        int high = (int)((val >> 32) & 0xFFFFFFFF);
        emit(c, OPC_PUSH_INT64_CONST, low, high);
        return EXPR_TYPE_INT;
    }

    case AST_STRING: {
        /* 字符串字面量：压入 PTR 栈 */
        int idx = c_add_string_const(c, node->u.sval);
        emit(c, OPC_LOAD_STRING_CONST, idx, 0);
        return EXPR_TYPE_PTR;
    }
    
    case AST_VAR: {
        /* 变量：查找符号表，根据类型选择加载指令 */
        const char* name = node->u.varname;
        int idx = c_find_var(c, name);
        if(idx < 0) {
            /* 未声明的变量，先用 VALUE 栈 */
            emit(c, OPC_LOAD_VAR, 0, 0);
            return EXPR_TYPE_NONE;
        }
        ExprType vt = c->var_types[idx];
        if(vt == EXPR_TYPE_INT) {
            emit(c, OPC_LOAD_INT64_VAR, idx, 0);
        } else if(vt == EXPR_TYPE_DOUBLE) {
            emit(c, OPC_LOAD_DOUBLE_VAR, idx, 0);
        } else if(vt == EXPR_TYPE_PTR) {
            emit(c, OPC_LOAD_PTR_VAR, idx, 0);
        } else {
            emit(c, OPC_LOAD_VAR, idx, 0);
        }
        return vt;
    }

    case AST_CAST: {
        /* 类型标注 <type>expr：编译子表达式，标记类型 */
        ExprType child_type = c_expr(c, node->u.cast.child);
        CastKind ct = node->u.cast.cast_type;
        /* 根据 CastKind 返回表达式类型 */
        if(ct == CAST_INT || ct == CAST_SHORT || ct == CAST_INT8 || ct == CAST_INT16 ||
           ct == CAST_INT32 || ct == CAST_INT64 || ct == CAST_UINT8 || ct == CAST_UINT16 ||
           ct == CAST_UINT32 || ct == CAST_UINT64 || ct == CAST_CHAR || ct == CAST_BOOL) {
            return EXPR_TYPE_INT;
        } else if(ct == CAST_DOUBLE || ct == CAST_FLOAT) {
            return EXPR_TYPE_DOUBLE;
        } else if(ct == CAST_STRING) {
            return EXPR_TYPE_PTR;
        }
        return child_type;
    }

    case AST_TYPE_ANNOTATION: {
        /* 类型标注 <type>expr：编译子表达式，标记类型 */
        ExprType child_type = c_expr(c, node->u.type_annotation.expr);
        CastKind ct = node->u.type_annotation.cast_type;
        /* 根据 CastKind 返回表达式类型 */
        if(ct == CAST_INT || ct == CAST_SHORT || ct == CAST_INT8 || ct == CAST_INT16 ||
           ct == CAST_INT32 || ct == CAST_INT64 || ct == CAST_UINT8 || ct == CAST_UINT16 ||
           ct == CAST_UINT32 || ct == CAST_UINT64 || ct == CAST_CHAR || ct == CAST_BOOL) {
            return EXPR_TYPE_INT;
        } else if(ct == CAST_DOUBLE || ct == CAST_FLOAT) {
            return EXPR_TYPE_DOUBLE;
        } else if(ct == CAST_STRING) {
            return EXPR_TYPE_PTR;
        }
        return child_type;
    }

    case AST_BINOP: {
        /* 二元运算 */
        ExprType lt = c_expr(c, node->u.bin.left);
        ExprType rt = c_expr(c, node->u.bin.right);
        
        /* 类型提升：int + double → double */
        ExprType result = arith_get_expr_type(c, node);
        
        /* 根据运算符和类型选择指令 */
        switch(node->u.bin.op) {
        case OP_ADD:
            if(result == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_ADD, 0, 0);
            } else if(result == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_ADD, 0, 0);
            }
            break;
        case OP_SUB:
            if(result == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_SUB, 0, 0);
            } else if(result == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_SUB, 0, 0);
            }
            break;
        case OP_MUL:
            if(result == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_MUL, 0, 0);
            } else if(result == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_MUL, 0, 0);
            }
            break;
        case OP_DIV:
            if(result == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_DIV, 0, 0);
            } else if(result == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_DIV, 0, 0);
            }
            break;
        default:
            break;
        }
        return result;
    }
    
    default:
        fprintf(stderr, "IR: unknown expr type %d\n", node->type);
        return EXPR_TYPE_NONE;
    }
}

/* ============================================================
 * 语句编译
 * ============================================================ */

/* 编译语句 */
void c_stmt(Ctx* c, AstNode* node) {
    if(!node) return;
    
    switch(node->type) {
    case AST_PRINT: {
        /* print 语句：根据表达式类型选择打印指令 */
        AstNode* args = node->u.print.args;
        if(args) {
            ExprType arg_type = c_expr(c, args);
            if(arg_type == EXPR_TYPE_INT) {
                emit(c, OPC_PRINT_INT64, 0, 0);
            } else if(arg_type == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_PRINT_DOUBLE, 0, 0);
            } else if(arg_type == EXPR_TYPE_PTR) {
                emit(c, OPC_PRINT_PTR, 0, 0);
            } else {
                emit(c, OPC_PRINT, 0, 0);
            }
        }
        break;
    }

    case AST_ASSIGN: {
        /* 赋值语句：注册变量，根据表达式类型选择存储指令 */
        const char* var_name = node->u.assign.varname;
        ExprType rt = c_expr(c, node->u.assign.expr);
        int var_idx = c_add_var(c, var_name, rt);
        if(rt == EXPR_TYPE_INT) {
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
        } else if(rt == EXPR_TYPE_DOUBLE) {
            emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
        } else if(rt == EXPR_TYPE_PTR) {
            emit(c, OPC_STORE_PTR_VAR, var_idx, 0);
        } else {
            emit(c, OPC_STORE_VAR, var_idx, 0);
        }
        break;
    }
    
    case AST_SEQ: {
        /* 语句序列 */
        c_stmt(c, node->u.seq.first);
        c_stmt(c, node->u.seq.second);
        break;
    }
    
    default:
        fprintf(stderr, "IR: unknown stmt type %d\n", node->type);
        break;
    }
}

/* ============================================================
 * 主编译入口
 * ============================================================ */

BytecodeFunc* ir_compile_main(AstNode* root) {
    /* 创建字节码函数 */
    BytecodeFunc* fn = calloc(1, sizeof(BytecodeFunc));
    fn->is_main = 1;
    fn->name = strdup("main");
    
    /* 创建编译上下文 */
    Ctx c;
    memset(&c, 0, sizeof(Ctx));
    c.fn = fn;
    
    /* 编译 AST */
    c_stmt(&c, root);
    
    /* 添加返回指令 */
    emit(&c, OPC_RETURN, 0, 0);
    
    return fn;
}

/* 编译函数（简化） */
BytecodeFunc* ir_compile_function(const char* name, AstNode* params, AstNode* body, int is_generator, const char* class_name) {
    (void)params; (void)body; (void)is_generator;
    
    BytecodeFunc* fn = calloc(1, sizeof(BytecodeFunc));
    fn->name = strdup(name);
    
    return fn;
}

/* 函数表查找（简化） */
BytecodeFunc* ir_func_table_lookup(const char* name) {
    (void)name;
    return NULL;
}

BytecodeFunc* ir_func_table_lookup_class(const char* class_name, const char* method_name) {
    (void)class_name; (void)method_name;
    return NULL;
}

BytecodeFunc* ir_func_table_lookup_any(const char* name) {
    return ir_func_table_lookup(name);
}

void ir_func_table_reset(void) {
}

void ir_func_table_foreach(void (*callback)(const char*, const char*, void*, void*), void* user_data) {
    (void)callback; (void)user_data;
}

BytecodeFunc* ir_func_table_recompile(const char* name, AstNode* params, AstNode* body) {
    return ir_compile_function(name, params, body, 0, NULL);
}

void string_cache_reset(void) {
}
