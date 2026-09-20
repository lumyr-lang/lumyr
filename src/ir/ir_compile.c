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
#include "ast/ast_types.h"
#include "ast/func_compile.h"
#include "lm_value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 前向声明 */
void c_stmt(Ctx* c, AstNode* node);
ExprType c_expr(Ctx* c, AstNode* node);
static const char* c_expr_type_name(Ctx* c, AstNode* node);
static CastKind c_expr_cast_type(Ctx* c, AstNode* node);
static void emit_to_dynamic(Ctx* c, ExprType from, CastKind ck);
static void c_expr_to_value(Ctx* c, AstNode* node);
AstNode* func_ast_lookup(const char* name);   /* AST 函数表（func_compile.c） */
static ExprType compile_user_call(Ctx* c, BytecodeFunc* callee, AstNode* def_ast, AstNode* args, int keep_result);
static void collect_call_args(AstNode* n, AstNode*** argv, int* argc, int* acap);

/* ============================================================
 * 表达式编译
 * ============================================================ */

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

/* ============================================================
 * 三元表达式辅助
 * ============================================================ */

/* 在两个分支类型名之间选统一目标，优先级与 BINOP 类型提升一致 */
static const char* ternary_target_name(const char* a, const char* b) {
    static const char* order[] = {"string", "bigint", "decimal", "double", "int"};
    for(int i = 0; i < 5; i++) {
        if(strcmp(a, order[i]) == 0 || strcmp(b, order[i]) == 0) return order[i];
    }
    return a;
}

/* 把栈顶值从 from 类型转换为 to 类型（仅处理本编译器已支持的转换指令） */
static void ternary_cast_to(Ctx* c, const char* from, const char* to) {
    if(strcmp(from, to) == 0) return;
    if(strcmp(to, "double") == 0) {
        if(strcmp(from, "int") == 0) emit(c, OPC_INT64_TO_DOUBLE, 0, 0);
    } else if(strcmp(to, "int") == 0) {
        if(strcmp(from, "double") == 0) emit(c, OPC_DOUBLE_TO_INT64, 0, 0);
    } else if(strcmp(to, "string") == 0) {
        if(strcmp(from, "int") == 0) emit(c, OPC_INT64_TO_STRING, 0, 0);
        else if(strcmp(from, "double") == 0) emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
    }
}

/* 类型名 → ExprType */
static ExprType ternary_name_to_exprtype(const char* n) {
    if(strcmp(n, "double") == 0) return EXPR_TYPE_DOUBLE;
    if(strcmp(n, "int") == 0) return EXPR_TYPE_INT;
    return EXPR_TYPE_PTR;
}

/* CastKind 是否整型族（统一 INT64 存储栈） */
static int cast_is_intfamily(CastKind ck) {
    return ck == CAST_INT || ck == CAST_SHORT || ck == CAST_USHORT ||
           ck == CAST_INT8 || ck == CAST_INT16 || ck == CAST_INT32 || ck == CAST_INT64 ||
           ck == CAST_LONG || ck == CAST_LONGLONG ||
           ck == CAST_UINT8 || ck == CAST_UINT16 || ck == CAST_UINT32 ||
           ck == CAST_UINT || ck == CAST_UINT64 || ck == CAST_ULONG ||
           ck == CAST_UCHAR || ck == CAST_BYTE ||
           ck == CAST_ASCII || ck == CAST_SIZE_T || ck == CAST_SSIZE_T ||
           ck == CAST_CHAR || ck == CAST_BOOL;
}

/* CastKind 是否浮点族（统一 DOUBLE 存储栈） */
static int cast_is_floatfamily(CastKind ck) {
    return ck == CAST_DOUBLE || ck == CAST_FLOAT || ck == CAST_LONG_DOUBLE;
}

/* 编译类型化数组字面量：逐元素按目标 CastKind 编译到 typed 栈，
   再 emit 对应的 *_ARRAY_LIT（a=元素数，b=元素 ValueType），数组压 VALUE 栈。
   via_cast=1 时元素走 AST_CAST（(T)[...] 路径），否则走 AST_TYPE_ANNOTATION（<T>[...]）。 */
static void compile_typed_array_lit(Ctx* c, CastKind ct, AstNode* elems, int via_cast) {
    AstNode** argv = NULL;
    int argc = 0, acap = 0;
    collect_call_args(elems, &argv, &argc, &acap);
    for(int i = 0; i < argc; i++) {
        AstNode* e = argv[i];
        /* 元素本身已是同类型标注/转型（如 <decimal>[<decimal>"1.5"]）：
         * 直接编译，重复包装会让 FROM_STRING 二次触发、把对象当字符串解析 */
        int same_ann  = !via_cast && e->type == AST_TYPE_ANNOTATION &&
                        e->u.type_annotation.cast_type == ct;
        int same_cast = via_cast && e->type == AST_CAST &&
                        e->u.cast.cast_type == ct;
        if(same_ann || same_cast) {
            c_expr(c, e);
            continue;
        }
        AstNode elem_ann;
        memset(&elem_ann, 0, sizeof(elem_ann));
        if(via_cast) {
            elem_ann.type = AST_CAST;
            elem_ann.u.cast.cast_type = ct;
            elem_ann.u.cast.child = e;
        } else {
            elem_ann.type = AST_TYPE_ANNOTATION;
            elem_ann.u.type_annotation.cast_type = ct;
            elem_ann.u.type_annotation.expr = e;
        }
        c_expr(c, &elem_ann);
    }
    int op = cast_is_intfamily(ct)   ? OPC_INT64_ARRAY_LIT :
             cast_is_floatfamily(ct) ? OPC_DOUBLE_ARRAY_LIT :
                                       OPC_PTR_ARRAY_LIT;
    emit(c, op, argc, (int)castkind_to_valtype(ct));
    free(argv);
}

/* 编译表达式，返回表达式类型 */
ExprType c_expr(Ctx* c, AstNode* node) {
    if(!node) return EXPR_TYPE_NONE;
    
    switch(node->type) {
    case AST_INT: {
        /* 整数字面量：小常量内嵌，大常量走常量池 */
        int64_t val = node->u.inum;
        if(val >= INT32_MIN && val <= INT32_MAX) {
            /* 小常量（int32 范围）：直接内嵌在指令里 */
            emit(c, OPC_PUSH_INT64_CONST, (int)val, 0);
        } else {
            /* 大常量（超出 int32 范围）：走常量池 */
            int idx = bf_add_i64_const(c->fn, val);
            emit(c, OPC_PUSH_CONST_IDX, idx, 0);
        }
        return EXPR_TYPE_INT;
    }

    case AST_NUM: {
        /* 浮点数字面量：走常量池 */
        double val = node->u.num;
        int idx = bf_add_double_const(c->fn, val);
        emit(c, OPC_PUSH_CONST_IDX, idx, 0);
        return EXPR_TYPE_DOUBLE;
    }

    case AST_BOOL: {
        /* 布尔字面量：小常量（0/1），直接内嵌 */
        int64_t val = node->u.bval ? 1 : 0;
        emit(c, OPC_PUSH_INT64_CONST, (int)val, 0);
        return EXPR_TYPE_INT;
    }

    case AST_CHAR: {
        /* 字符字面量：小常量（0~255），直接内嵌 */
        int64_t val = (int64_t)node->u.ch;
        emit(c, OPC_PUSH_INT64_CONST, (int)val, 0);
        return EXPR_TYPE_INT;
    }

    case AST_STRING: {
        /* 字符串字面量：走统一常量池，压入 PTR 栈 */
        int idx = bf_add_str_const(c->fn, node->u.sval);
        emit(c, OPC_PUSH_CONST_IDX, idx, 0);
        return EXPR_TYPE_PTR;
    }

    case AST_NONE: {
        /* null 字面量：发射 PUSH_NONE，运行时压入 VALUE 栈的 NONE */
        emit(c, OPC_PUSH_NONE, 0, 0);
        return EXPR_TYPE_NONE;
    }

    case AST_VAR: {
        /* 变量：查找符号表，根据类型选择加载指令 */
        const char* name = node->u.varname;
        int idx = c_find_var(c, name);
        if(idx < 0) {
            /* 未声明的局部变量：若是函数名，作为函数值引用 */
            BytecodeFunc* fn = ir_func_table_lookup(name);
            if(fn) {
                int sym = c_add_var(c, name, EXPR_TYPE_NONE);
                emit(c, OPC_GETFUNC, sym, 0);
                return EXPR_TYPE_NONE;
            }
            /* 否则当作 VALUE 栈的未声明变量 */
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

    case AST_FUNCREF: {
        /* 函数名引用（函数作为值）：压入函数值 */
        const char* fname = node->u.varname;
        int sym = c_add_var(c, fname, EXPR_TYPE_NONE);
        emit(c, OPC_GETFUNC, sym, 0);
        return EXPR_TYPE_NONE;
    }

    case AST_FUNC_DEF: {
        /* 匿名函数表达式（lambda）：有捕获则 MKCLOSURE，否则普通函数值 */
        const char* lname = node->u.func_def.name;
        int sym = c_add_var(c, lname, EXPR_TYPE_NONE);
        int ncap = lambda_capture_count(lname);
        if(ncap > 0) {
            emit(c, OPC_MKCLOSURE, sym, 0);
        } else {
            emit(c, OPC_GETFUNC, sym, 0);
        }
        return EXPR_TYPE_NONE;
    }

    case AST_CAST: {
        /* 类型强转 (type)expr：编译子表达式，根据目标类型 emit 转换指令 */
        CastKind ct = node->u.cast.cast_type;
        /* cast 目标是数组字面量（(T)[e1,e2]）：逐元素强转构造类型化数组 */
        if(node->u.cast.child && node->u.cast.child->type == AST_ARRAY_LIT) {
            compile_typed_array_lit(c, ct, node->u.cast.child->u.array_lit.elems, 1);
            return EXPR_TYPE_NONE;
        }
        ExprType child_type = c_expr(c, node->u.cast.child);
        CastKind child_ct = c_expr_cast_type(c, node->u.cast.child);
        /* 转成 string：根据源类型选择转换指令 */
        if(ct == CAST_STRING) {
            if(child_type == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_TO_STRING, 0, 0);
            } else if(child_type == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
            } else if(child_type == EXPR_TYPE_PTR && child_ct == CAST_BIGINT) {
                emit(c, OPC_BIGINT_TO_STRING, 0, 0);
            } else if(child_type == EXPR_TYPE_PTR && child_ct == CAST_DECIMAL) {
                emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
            }
            return EXPR_TYPE_PTR;
        }
        /* 转成 int：根据源类型 emit 转换指令 */
        if(ct == CAST_INT || ct == CAST_SHORT || ct == CAST_USHORT || ct == CAST_INT8 || ct == CAST_INT16 ||
           ct == CAST_INT32 || ct == CAST_INT64 || ct == CAST_LONG || ct == CAST_LONGLONG ||
           ct == CAST_UINT8 || ct == CAST_UINT16 || ct == CAST_UINT32 || ct == CAST_UINT ||
           ct == CAST_UINT64 || ct == CAST_ULONG || ct == CAST_UCHAR || ct == CAST_BYTE ||
           ct == CAST_ASCII || ct == CAST_SIZE_T || ct == CAST_SSIZE_T || ct == CAST_CHAR || ct == CAST_BOOL) {
            /* double → int：截断 */
            if(child_type == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_TO_INT64, 0, 0);
            }
            /* ptr → int：取整数地址 */
            else if(child_type == EXPR_TYPE_PTR) {
                emit(c, OPC_PTR_TO_INT64, 0, 0);
            }
            /* bigint → int：截断（后续实现） */
            /* decimal → int：截断（后续实现） */
            return EXPR_TYPE_INT;
        }
        /* 转成 double：根据源类型 emit 转换指令 */
        if(ct == CAST_DOUBLE || ct == CAST_FLOAT || ct == CAST_LONG_DOUBLE) {
            /* int → double */
            if(child_type == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_TO_DOUBLE, 0, 0);
            }
            return EXPR_TYPE_DOUBLE;
        }
        /* 转成 ptr：整数地址 → 裸指针 */
        if(ct == CAST_PTR) {
            if(child_type == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_TO_PTR, 0, 0);
            } else if(child_type == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_TO_INT64, 0, 0);
                emit(c, OPC_INT64_TO_PTR, 0, 0);
            }
            return EXPR_TYPE_PTR;
        }
        return child_type;
    }

    case AST_TYPE_ANNOTATION: {
        /* 类型标注 <type>expr：编译子表达式，标记类型 */
        CastKind ct = node->u.type_annotation.cast_type;
        AstNode* ann_child = node->u.type_annotation.expr;
        /* 标注目标是数组字面量（<T>[e1,e2]）：逐元素转型构造类型化数组 */
        if(ann_child && ann_child->type == AST_ARRAY_LIT) {
            compile_typed_array_lit(c, ct, ann_child->u.array_lit.elems, 0);
            return EXPR_TYPE_NONE;
        }
        /* bigint/decimal 字面量快速路径：下面的标注分支会自己把字面量压成字符串常量。
           若先调 c_expr，子表达式会先压一次值，标注又压一次 → PTR 栈残留原始指针；
           后续二元运算会把该 char* 当成 BigInt*/
        int ann_lit = ann_child && (ann_child->type == AST_INT ||
                                    ann_child->type == AST_NUM ||
                                    ann_child->type == AST_STRING);
        ExprType child_type;
        if((ct == CAST_BIGINT || ct == CAST_DECIMAL || ct == CAST_BITDECIMAL) && ann_lit) {
            child_type = (ann_child->type == AST_NUM) ? EXPR_TYPE_DOUBLE :
                         (ann_child->type == AST_STRING) ? EXPR_TYPE_PTR : EXPR_TYPE_INT;
        } else {
            child_type = c_expr(c, ann_child);
        }
        /* 根据 CastKind 返回表达式类型，必要时 emit 跨栈转换指令 */
        if(ct == CAST_INT || ct == CAST_SHORT || ct == CAST_USHORT || ct == CAST_INT8 || ct == CAST_INT16 ||
           ct == CAST_INT32 || ct == CAST_INT64 || ct == CAST_LONG || ct == CAST_LONGLONG ||
           ct == CAST_UINT8 || ct == CAST_UINT16 || ct == CAST_UINT32 || ct == CAST_UINT ||
           ct == CAST_UINT64 || ct == CAST_ULONG || ct == CAST_UCHAR || ct == CAST_BYTE ||
           ct == CAST_ASCII || ct == CAST_SIZE_T || ct == CAST_SSIZE_T || ct == CAST_CHAR || ct == CAST_BOOL) {
            /* 如果子表达式是 DOUBLE，需要转成 INT */
            if(child_type == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_TO_INT64, 0, 0);
            }
            /* ptr → int：取整数地址 */
            else if(child_type == EXPR_TYPE_PTR) {
                emit(c, OPC_PTR_TO_INT64, 0, 0);
            }
            return EXPR_TYPE_INT;
        } else if(ct == CAST_PTR) {
            /* <ptr>expr：整数地址 → 裸指针，压 PTR 栈 */
            if(child_type == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_TO_PTR, 0, 0);
            } else if(child_type == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_TO_INT64, 0, 0);
                emit(c, OPC_INT64_TO_PTR, 0, 0);
            }
            return EXPR_TYPE_PTR;
        } else if(ct == CAST_DOUBLE || ct == CAST_FLOAT || ct == CAST_LONG_DOUBLE) {
            /* 如果子表达式是 INT，需要转成 DOUBLE */
            if(child_type == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_TO_DOUBLE, 0, 0);
            }
            return EXPR_TYPE_DOUBLE;
        } else if(ct == CAST_BIGINT) {
            /* <bigint>expr：从字符串创建 bigint 对象
             * 方案 B：如果子表达式是字面量，直接把字面量转成字符串，零转换开销
             * 如果不是字面量，还是先识别成原来的类型，再转换 */
            AstNode* child = node->u.type_annotation.expr;
            if(child->type == AST_INT) {
                /* 整数字面量：直接把整数转成字符串，零转换开销 */
                int64_t val = child->u.inum;
                char buf[32];
                snprintf(buf, sizeof(buf), "%lld", val);
                int idx = bf_add_str_const(c->fn, buf);
                emit(c, OPC_PUSH_CONST_IDX, idx, 0);
            } else if(child->type == AST_NUM) {
                /* 浮点数字面量：直接把浮点数转成字符串，零转换开销 */
                double val = child->u.num;
                char buf[64];
                snprintf(buf, sizeof(buf), "%.15g", val);
                int idx = bf_add_str_const(c->fn, buf);
                emit(c, OPC_PUSH_CONST_IDX, idx, 0);
            } else if(child->type == AST_STRING) {
                /* 字符串字面量：直接压入字符串常量，零转换开销 */
                int idx = bf_add_str_const(c->fn, child->u.sval);
                emit(c, OPC_PUSH_CONST_IDX, idx, 0);
            } else {
                /* 非字面量：先识别成原来的类型，再转换 */
                if(child_type == EXPR_TYPE_INT) {
                    emit(c, OPC_INT64_TO_STRING, 0, 0);
                } else if(child_type == EXPR_TYPE_DOUBLE) {
                    emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
                }
            }
            emit(c, OPC_BIGINT_FROM_STRING, 0, 0);
            return EXPR_TYPE_PTR;
        } else if(ct == CAST_DECIMAL) {
            /* <decimal>expr：从字符串创建 decimal 对象
             * 方案 B：如果子表达式是字面量，直接把字面量转成字符串，零转换开销
             * 如果不是字面量，还是先识别成原来的类型，再转换 */
            AstNode* child = node->u.type_annotation.expr;
            if(child->type == AST_INT) {
                /* 整数字面量：直接把整数转成字符串，零转换开销 */
                int64_t val = child->u.inum;
                char buf[32];
                snprintf(buf, sizeof(buf), "%lld", val);
                int idx = bf_add_str_const(c->fn, buf);
                emit(c, OPC_PUSH_CONST_IDX, idx, 0);
            } else if(child->type == AST_NUM) {
                /* 浮点数字面量：直接把浮点数转成字符串，零转换开销 */
                double val = child->u.num;
                char buf[64];
                snprintf(buf, sizeof(buf), "%.15g", val);
                int idx = bf_add_str_const(c->fn, buf);
                emit(c, OPC_PUSH_CONST_IDX, idx, 0);
            } else if(child->type == AST_STRING) {
                /* 字符串字面量：直接压入字符串常量，零转换开销 */
                int idx = bf_add_str_const(c->fn, child->u.sval);
                emit(c, OPC_PUSH_CONST_IDX, idx, 0);
            } else {
                /* 其他表达式：先识别成原来的类型，再转换 */
                if(child_type == EXPR_TYPE_DOUBLE) {
                    emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
                } else if(child_type == EXPR_TYPE_INT) {
                    emit(c, OPC_INT64_TO_STRING, 0, 0);
                }
            }
            emit(c, OPC_DECIMAL_FROM_STRING, 0, 0);
            return EXPR_TYPE_PTR;
        } else if(ct == CAST_BITDECIMAL) {
            /* <bitdecimal>expr：从字符串创建 bitdecimal 对象（基于 GMP mpf_t）
             * 方案 B：如果子表达式是字面量，直接把字面量转成字符串，零转换开销
             * 如果不是字面量，还是先识别成原来的类型，再转换 */
            AstNode* child = node->u.type_annotation.expr;
            if(child->type == AST_INT) {
                /* 整数字面量：直接把整数转成字符串，零转换开销 */
                int64_t val = child->u.inum;
                char buf[32];
                snprintf(buf, sizeof(buf), "%lld", val);
                int idx = bf_add_str_const(c->fn, buf);
                emit(c, OPC_PUSH_CONST_IDX, idx, 0);
            } else if(child->type == AST_NUM) {
                /* 浮点数字面量：直接把浮点数转成字符串，零转换开销 */
                double val = child->u.num;
                char buf[64];
                snprintf(buf, sizeof(buf), "%.15g", val);
                int idx = bf_add_str_const(c->fn, buf);
                emit(c, OPC_PUSH_CONST_IDX, idx, 0);
            } else if(child->type == AST_STRING) {
                /* 字符串字面量：直接压入字符串常量，零转换开销 */
                int idx = bf_add_str_const(c->fn, child->u.sval);
                emit(c, OPC_PUSH_CONST_IDX, idx, 0);
            } else {
                /* 非字面量：按源类型精确转换（int/double 走专用指令，字符串/高精度对象经字符串） */
                CastKind child_ct = c_expr_cast_type(c, child);
                if(child_type == EXPR_TYPE_INT) {
                    emit(c, OPC_BITDECIMAL_FROM_INT64, 0, 0);
                    return EXPR_TYPE_PTR;
                } else if(child_type == EXPR_TYPE_DOUBLE) {
                    emit(c, OPC_BITDECIMAL_FROM_DOUBLE, 0, 0);
                    return EXPR_TYPE_PTR;
                } else if(child_type == EXPR_TYPE_PTR && child_ct == CAST_BIGINT) {
                    emit(c, OPC_BIGINT_TO_STRING, 0, 0);
                } else if(child_type == EXPR_TYPE_PTR && child_ct == CAST_DECIMAL) {
                    emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
                }
                /* PTR(string) 及其他：字符串形式 FROM_STRING */
                emit(c, OPC_BITDECIMAL_FROM_STRING, 0, 0);
                return EXPR_TYPE_PTR;
            }
            emit(c, OPC_BITDECIMAL_FROM_STRING, 0, 0);
            return EXPR_TYPE_PTR;
        } else if(ct == CAST_STRING) {
            /* string 是堆分配对象，走 PTR 栈；必要时从 INT/DOUBLE 转换 */
            if(child_type == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_TO_STRING, 0, 0);
            } else if(child_type == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
            }
            return EXPR_TYPE_PTR;
        }
        return child_type;
    }

    case AST_CALL: {
        /* 函数调用：识别内置函数 type() */
        const char* func_name = node->u.call.name;
        AstNode* args = node->u.call.args;
        int argc = 0;
        /* 计算参数个数 */
        AstNode* p = args;
        while(p) {
            argc++;
            if(p->type == AST_SEQ) {
                p = p->u.seq.second;
            } else {
                break;
            }
        }
        /* 识别内置函数 */
        if(strcmp(func_name, "type") == 0 && argc == 1) {
            /* type(x)：运行时求值（下标取值/typed array 等动态情形编译期无法确定），
             * 参数编译到 VALUE 栈，BUILTIN_TYPE 返回类型名字符串 Value */
            AstNode* arg = args;
            if(args && args->type == AST_SEQ) {
                arg = args->u.seq.first;
            }
            c_expr_to_value(c, arg);
            emit(c, OPC_BUILTIN, BUILTIN_TYPE, 1);
            return EXPR_TYPE_NONE;
        }
        /* 用户自定义函数：查函数表 + AST 表 */
        {
            /* 内置函数 len(x)：数组/字符串长度，返回 int Value */
            if(strcmp(func_name, "len") == 0 && argc == 1) {
                AstNode* arg = args;
                if(args && args->type == AST_SEQ) arg = args->u.seq.first;
                c_expr_to_value(c, arg);
                emit(c, OPC_BUILTIN, BUILTIN_LEN, 1);
                return EXPR_TYPE_NONE;
            }
            BytecodeFunc* callee = ir_func_table_lookup(func_name);
            AstNode* def_ast = func_ast_lookup(func_name);
            if(callee && def_ast) {
                return compile_user_call(c, callee, def_ast, args, 1);
            }
            /* 动态调用：func_name 是持有函数值的变量 */
            {
                /* 编译 callee 表达式（变量名 → 函数值） */
                AstNode callee_var;
                memset(&callee_var, 0, sizeof(callee_var));
                callee_var.type = AST_VAR;
                callee_var.u.varname = func_name;
                c_expr(c, &callee_var);
                /* 收集并编译实参，全部转 VALUE */
                int dargc = 0, dacap = 0;
                AstNode** dargv = NULL;
                collect_call_args(args, &dargv, &dargc, &dacap);
                for(int i = 0; i < dargc; i++) {
                    c_expr_to_value(c, dargv[i]);
                }
                free(dargv);
                emit(c, OPC_CALLV, 0, dargc);
                return EXPR_TYPE_NONE;
            }
        }
        /* 未识别函数 */
        fprintf(stderr, "IR: unknown function %s\n", func_name);
        return EXPR_TYPE_NONE;
    }

    case AST_INDEX: {
        /* 数组/字符串下标读 arr[i]：arr、i 目标 VALUE，弹 i 再弹 arr，压元素 */
        c_expr_to_value(c, node->u.index.arr);
        c_expr_to_value(c, node->u.index.idx);
        emit(c, OPC_INDEX_GET, 0, 0);
        return EXPR_TYPE_NONE;
    }

    case AST_ARRAY_LIT: {
        /* 数组字面量 [e1,e2,...]：各元素目标 VALUE 压栈，ARRAY_LIT 弹出组装 */
        AstNode** argv = NULL;
        int argc = 0, acap = 0;
        collect_call_args(node->u.array_lit.elems, &argv, &argc, &acap);
        for(int i = 0; i < argc; i++) {
            c_expr_to_value(c, argv[i]);
        }
        emit(c, OPC_ARRAY_LIT, 0, argc);
        free(argv);
        return EXPR_TYPE_NONE;
    }

    case AST_MAP_LIT: {
        /* 字典字面量 {k1:v1,...}：键、值交替目标 VALUE 压栈，MAP_LIT 弹出组装 */
        AstNode** ev = NULL;
        int ecnt = 0, ecap = 0;
        collect_call_args(node->u.map_lit.entries, &ev, &ecnt, &ecap);
        for(int i = 0; i < ecnt; i++) {
            AstNode* e = ev[i];
            if(e && e->type == AST_MAP_ENTRY) {
                c_expr_to_value(c, e->u.map_entry.key);
                c_expr_to_value(c, e->u.map_entry.value);
            }
        }
        emit(c, OPC_MAP_LIT, 0, ecnt);
        free(ev);
        return EXPR_TYPE_NONE;
    }

    case AST_INDEX_ASSIGN: {
        /* 下标写 arr[i] = v（表达式值为 v）：arr,idx,v 目标 VALUE 压栈，INDEX_SET */
        c_expr_to_value(c, node->u.index_assign.arr);
        c_expr_to_value(c, node->u.index_assign.idx);
        c_expr_to_value(c, node->u.index_assign.value);
        emit(c, OPC_INDEX_SET, 0, 0);
        return EXPR_TYPE_NONE;
    }

    case AST_DYN_CALL: {
        /* 动态调用 callee(args)：callee 与实参全部目标 VALUE + CALLV */
        c_expr_to_value(c, node->u.dyn_call.callee);
        AstNode** argv = NULL;
        int argc = 0, acap = 0;
        collect_call_args(node->u.dyn_call.args, &argv, &argc, &acap);
        for(int i = 0; i < argc; i++) {
            c_expr_to_value(c, argv[i]);
        }
        emit(c, OPC_CALLV, 0, argc);
        return EXPR_TYPE_NONE;
    }

    case AST_UNARY: {
        /* 一元运算：负号 */
        ExprType child_type = c_expr(c, node->u.uny.child);
        if(node->u.uny.op == OP_UNARY_MINUS) {
            if(child_type == EXPR_TYPE_NONE) {
                emit(c, OPC_VNEG, 0, 0);   /* 动态 Value 一元负 */
            } else {
                emit(c, OPC_NEG, (int)child_type, 0);
            }
        }
        return child_type;
    }

    case AST_BINOP: {
        /* 二元运算 */
        /* 特殊处理：字符串拼接（PTR 栈）需要按顺序转换操作数 */
        ExprType result = arith_get_expr_type(c, node);
        CastKind lt_cast = c_expr_cast_type(c, node->u.bin.left);
        CastKind rt_cast = c_expr_cast_type(c, node->u.bin.right);

        /* bigint 运算：至少一个操作数是 bigint */
        if((lt_cast == CAST_BIGINT || rt_cast == CAST_BIGINT) && lt_cast != CAST_STRING && rt_cast != CAST_STRING) {
            /* 编译左操作数 */
            ExprType lt = c_expr(c, node->u.bin.left);
            /* 如果左操作数不是 bigint，转成 bigint */
            if(lt_cast != CAST_BIGINT) {
                if(lt == EXPR_TYPE_INT) {
                    emit(c, OPC_INT64_TO_STRING, 0, 0);
                } else if(lt == EXPR_TYPE_DOUBLE) {
                    emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
                } else if(lt == EXPR_TYPE_PTR && lt_cast == CAST_DECIMAL) {
                    emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
                } else if(lt == EXPR_TYPE_PTR && lt_cast == CAST_BITDECIMAL) {
                    emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0);
                }
                emit(c, OPC_BIGINT_FROM_STRING, 0, 0);
            }
            /* 编译右操作数 */
            ExprType rt = c_expr(c, node->u.bin.right);
            /* 如果右操作数不是 bigint，转成 bigint */
            if(rt_cast != CAST_BIGINT) {
                if(rt == EXPR_TYPE_INT) {
                    emit(c, OPC_INT64_TO_STRING, 0, 0);
                } else if(rt == EXPR_TYPE_DOUBLE) {
                    emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
                } else if(rt == EXPR_TYPE_PTR && rt_cast == CAST_DECIMAL) {
                    emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
                } else if(rt == EXPR_TYPE_PTR && rt_cast == CAST_BITDECIMAL) {
                    emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0);
                }
                emit(c, OPC_BIGINT_FROM_STRING, 0, 0);
            }
            switch(node->u.bin.op) {
            case OP_ADD: emit(c, OPC_BIGINT_ADD, 0, 0); break;
            case OP_SUB: emit(c, OPC_BIGINT_SUB, 0, 0); break;
            case OP_MUL: emit(c, OPC_BIGINT_MUL, 0, 0); break;
            case OP_DIV: emit(c, OPC_BIGINT_DIV, 0, 0); break;
            default: break;
            }
            return EXPR_TYPE_PTR;
        }

        /* bitdecimal 运算：至少一个操作数是 bitdecimal（吸收 int/double/decimal） */
        if((lt_cast == CAST_BITDECIMAL || rt_cast == CAST_BITDECIMAL) && lt_cast != CAST_STRING && rt_cast != CAST_STRING) {
            /* 编译左操作数 */
            ExprType lt = c_expr(c, node->u.bin.left);
            /* 如果左操作数不是 bitdecimal，转成 bitdecimal */
            if(lt_cast != CAST_BITDECIMAL) {
                if(lt == EXPR_TYPE_INT) {
                    emit(c, OPC_BITDECIMAL_FROM_INT64, 0, 0);
                } else if(lt == EXPR_TYPE_DOUBLE) {
                    emit(c, OPC_BITDECIMAL_FROM_DOUBLE, 0, 0);
                } else if(lt == EXPR_TYPE_PTR && lt_cast == CAST_DECIMAL) {
                    emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
                    emit(c, OPC_BITDECIMAL_FROM_STRING, 0, 0);
                }
            }
            /* 编译右操作数 */
            ExprType rt = c_expr(c, node->u.bin.right);
            /* 如果右操作数不是 bitdecimal，转成 bitdecimal */
            if(rt_cast != CAST_BITDECIMAL) {
                if(rt == EXPR_TYPE_INT) {
                    emit(c, OPC_BITDECIMAL_FROM_INT64, 0, 0);
                } else if(rt == EXPR_TYPE_DOUBLE) {
                    emit(c, OPC_BITDECIMAL_FROM_DOUBLE, 0, 0);
                } else if(rt == EXPR_TYPE_PTR && rt_cast == CAST_DECIMAL) {
                    emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
                    emit(c, OPC_BITDECIMAL_FROM_STRING, 0, 0);
                }
            }
            switch(node->u.bin.op) {
            case OP_ADD: emit(c, OPC_BITDECIMAL_ADD, 0, 0); break;
            case OP_SUB: emit(c, OPC_BITDECIMAL_SUB, 0, 0); break;
            case OP_MUL: emit(c, OPC_BITDECIMAL_MUL, 0, 0); break;
            case OP_DIV: emit(c, OPC_BITDECIMAL_DIV, 0, 0); break;
            default: break;
            }
            return EXPR_TYPE_PTR;
        }
    
        /* decimal 运算：至少一个操作数是 decimal */
        if((lt_cast == CAST_DECIMAL || rt_cast == CAST_DECIMAL) && lt_cast != CAST_STRING && rt_cast != CAST_STRING) {
            /* 编译左操作数 */
            ExprType lt = c_expr(c, node->u.bin.left);
            /* 如果左操作数不是 decimal，转成 decimal */
            if(lt_cast != CAST_DECIMAL) {
                if(lt == EXPR_TYPE_INT) {
                    emit(c, OPC_INT64_TO_STRING, 0, 0);
                } else if(lt == EXPR_TYPE_DOUBLE) {
                    emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
                } else if(lt == EXPR_TYPE_PTR && lt_cast == CAST_BIGINT) {
                    emit(c, OPC_BIGINT_TO_STRING, 0, 0);
                }
                emit(c, OPC_DECIMAL_FROM_STRING, 0, 0);
            }
            /* 编译右操作数 */
            ExprType rt = c_expr(c, node->u.bin.right);
            /* 如果右操作数不是 decimal，转成 decimal */
            if(rt_cast != CAST_DECIMAL) {
                if(rt == EXPR_TYPE_INT) {
                    emit(c, OPC_INT64_TO_STRING, 0, 0);
                } else if(rt == EXPR_TYPE_DOUBLE) {
                    emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
                } else if(rt == EXPR_TYPE_PTR && rt_cast == CAST_BIGINT) {
                    emit(c, OPC_BIGINT_TO_STRING, 0, 0);
                }
                emit(c, OPC_DECIMAL_FROM_STRING, 0, 0);
            }
            switch(node->u.bin.op) {
            case OP_ADD: emit(c, OPC_DECIMAL_ADD, 0, 0); break;
            case OP_SUB: emit(c, OPC_DECIMAL_SUB, 0, 0); break;
            case OP_MUL: emit(c, OPC_DECIMAL_MUL, 0, 0); break;
            case OP_DIV: emit(c, OPC_DECIMAL_DIV, 0, 0); break;
            default: break;
            }
            return EXPR_TYPE_PTR;
        }

        if(result == EXPR_TYPE_PTR && (node->u.bin.op == OP_ADD || node->u.bin.op == OP_MUL || node->u.bin.op == OP_DIV || node->u.bin.op == OP_SUB)) {
            /* 字符串运算：先编译左操作数，立即转换；再编译右操作数，立即转换 */
            ExprType lt = c_expr(c, node->u.bin.left);
            if(lt == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_TO_STRING, 0, 0);
            } else if(lt == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
            } else if(lt == EXPR_TYPE_PTR && lt_cast == CAST_BIGINT) {
                emit(c, OPC_BIGINT_TO_STRING, 0, 0);
            } else if(lt == EXPR_TYPE_PTR && lt_cast == CAST_DECIMAL) {
                emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
            } else if(lt == EXPR_TYPE_PTR && lt_cast == CAST_BITDECIMAL) {
                emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0);
            }
            ExprType rt = c_expr(c, node->u.bin.right);
            if(rt == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_TO_STRING, 0, 0);
            } else if(rt == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
            } else if(rt == EXPR_TYPE_PTR && rt_cast == CAST_BIGINT) {
                emit(c, OPC_BIGINT_TO_STRING, 0, 0);
            } else if(rt == EXPR_TYPE_PTR && rt_cast == CAST_DECIMAL) {
                emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
            } else if(rt == EXPR_TYPE_PTR && rt_cast == CAST_BITDECIMAL) {
                emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0);
            }
            switch(node->u.bin.op) {
            case OP_ADD:
                emit(c, OPC_PTR_ADD, 0, 0);
                break;
            case OP_MUL:
                emit(c, OPC_PTR_MUL, 0, 0);
                break;
            case OP_DIV:
                emit(c, OPC_PTR_DIV, 0, 0);
                break;
            case OP_SUB:
                emit(c, OPC_PTR_SUB, 0, 0);
                break;
            default: break;
            }
            return EXPR_TYPE_PTR;
        }

        /* 普通二元运算：先编译左操作数，立即类型提升，再编译右操作数 */
        ExprType lt = c_expr(c, node->u.bin.left);
        /* 类型提升：左操作数立即转换，保证栈顺序正确 */
        if(result == EXPR_TYPE_DOUBLE && lt == EXPR_TYPE_INT) {
            emit(c, OPC_INT64_TO_DOUBLE, 0, 0);
        } else if(result == EXPR_TYPE_NONE && lt != EXPR_TYPE_NONE) {
            emit_to_dynamic(c, lt, c_expr_cast_type(c, node->u.bin.left));
        }
        ExprType rt = c_expr(c, node->u.bin.right);
        /* 类型提升：右操作数立即转换，保证栈顺序正确 */
        if(result == EXPR_TYPE_DOUBLE && rt == EXPR_TYPE_INT) {
            emit(c, OPC_INT64_TO_DOUBLE, 0, 0);
        } else if(result == EXPR_TYPE_NONE && rt != EXPR_TYPE_NONE) {
            emit_to_dynamic(c, rt, c_expr_cast_type(c, node->u.bin.right));
        }

        /* 根据运算符和类型选择指令 */
        switch(node->u.bin.op) {
        case OP_ADD:
            if(result == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_ADD, 0, 0);
            } else if(result == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_ADD, 0, 0);
            } else {
                emit(c, OPC_VADD, 0, 0);
            }
            break;
        case OP_SUB:
            if(result == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_SUB, 0, 0);
            } else if(result == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_SUB, 0, 0);
            } else {
                emit(c, OPC_VSUB, 0, 0);
            }
            break;
        case OP_MUL:
            if(result == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_MUL, 0, 0);
            } else if(result == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_MUL, 0, 0);
            } else {
                emit(c, OPC_VMUL, 0, 0);
            }
            break;
        case OP_DIV:
            if(result == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_DIV, 0, 0);
            } else if(result == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_DIV, 0, 0);
            } else {
                emit(c, OPC_VDIV, 0, 0);
            }
            break;
        case OP_MOD:
            if(result == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_MOD, 0, 0);
            } else {
                emit(c, OPC_VMOD, 0, 0);  /* double 不支持 mod，动态走 VMOD */
            }
            break;
        case OP_GT: case OP_LT: case OP_GE:
        case OP_LE: case OP_EQ: case OP_NE: {
            /* 比较结果统一压 INT64 栈（0/1），供条件跳转直接使用 */
            if(result == EXPR_TYPE_DOUBLE) {
                switch(node->u.bin.op) {
                case OP_GT: emit(c, OPC_DOUBLE_GT, 0, 0); break;
                case OP_LT: emit(c, OPC_DOUBLE_LT, 0, 0); break;
                case OP_GE: emit(c, OPC_DOUBLE_GE, 0, 0); break;
                case OP_LE: emit(c, OPC_DOUBLE_LE, 0, 0); break;
                case OP_EQ: emit(c, OPC_DOUBLE_EQ, 0, 0); break;
                case OP_NE: emit(c, OPC_DOUBLE_NE, 0, 0); break;
                default: break;
                }
            } else if(result == EXPR_TYPE_INT) {
                switch(node->u.bin.op) {
                case OP_GT: emit(c, OPC_INT64_GT, 0, 0); break;
                case OP_LT: emit(c, OPC_INT64_LT, 0, 0); break;
                case OP_GE: emit(c, OPC_INT64_GE, 0, 0); break;
                case OP_LE: emit(c, OPC_INT64_LE, 0, 0); break;
                case OP_EQ: emit(c, OPC_INT64_EQ, 0, 0); break;
                case OP_NE: emit(c, OPC_INT64_NE, 0, 0); break;
                default: break;
                }
            } else {
                /* 动态：通用 Value 比较，结果 bool Value 在 VALUE 栈 */
                switch(node->u.bin.op) {
                case OP_GT: emit(c, OPC_VGT, 0, 0); break;
                case OP_LT: emit(c, OPC_VLT, 0, 0); break;
                case OP_GE: emit(c, OPC_VGE, 0, 0); break;
                case OP_LE: emit(c, OPC_VLE, 0, 0); break;
                case OP_EQ: emit(c, OPC_VEQ, 0, 0); break;
                case OP_NE: emit(c, OPC_VNE, 0, 0); break;
                default: break;
                }
                return EXPR_TYPE_NONE;
            }
            return EXPR_TYPE_INT;
        }
        default:
            break;
        }
        return result;
    }

    case AST_TERNARY: {
        /* 三元 cond ? t : f：预判两分支类型，统一到目标类型，结果压对应栈 */
        const char* name_t = c_expr_type_name(c, node->u.ternary.true_expr);
        const char* name_f = c_expr_type_name(c, node->u.ternary.false_expr);
        const char* name_target = ternary_target_name(name_t, name_f);

        /* 条件（结果在 INT64 栈），假则跳到 else */
        c_expr(c, node->u.ternary.cond);
        int jfalse = emit_here(c, OPC_JMP_IF_FALSE, 0, 0);

        /* true 分支：编译后按目标类型转换 */
        c_expr(c, node->u.ternary.true_expr);
        ternary_cast_to(c, name_t, name_target);
        int jend = emit_here(c, OPC_JMP, 0, 0);

        /* else 分支 */
        patch_to(c, jfalse);
        c_expr(c, node->u.ternary.false_expr);
        ternary_cast_to(c, name_f, name_target);
        patch_to(c, jend);

        return ternary_name_to_exprtype(name_target);
    }

    case AST_SEQ: {
        /* 语句序列：编译所有语句，返回最后一个表达式的类型 */
        ExprType last_type = EXPR_TYPE_NONE;
        AstNode* cur = node;
        while(cur && cur->type == AST_SEQ) {
            last_type = c_expr(c, cur->u.seq.first);
            cur = cur->u.seq.second;
        }
        if(cur) {
            last_type = c_expr(c, cur);
        }
        return last_type;
    }

    case AST_ASSIGN: {
        /* 赋值语句：编译右值，返回其类型 */
        ExprType rt = c_expr(c, node->u.assign.expr);
        CastKind cast_type = c_expr_cast_type(c, node->u.assign.expr);
        int var_idx = c_add_var(c, node->u.assign.varname, rt);
        /* 记录精确类型到 BytecodeFunc 的 var_type_tags */
        int bf_idx = bf_sym(c->fn, node->u.assign.varname);
        c->fn->var_type_tags[bf_idx] = (int)cast_type;
        if(rt == EXPR_TYPE_INT) {
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
        } else if(rt == EXPR_TYPE_DOUBLE) {
            emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
        } else if(rt == EXPR_TYPE_PTR) {
            emit(c, OPC_STORE_PTR_VAR, var_idx, 0);
        } else {
            emit(c, OPC_STORE_VAR, var_idx, 0);
        }
        return rt;
    }

    default:
        fprintf(stderr, "IR: unknown expr type %d\n", node->type);
        return EXPR_TYPE_NONE;
    }
}

/* ============================================================
 * 语句编译
 * ============================================================ */

/* 获取表达式的类型名字符串（编译期推断，用于 type() 函数） */
static const char* c_expr_type_name(Ctx* c, AstNode* node) {
    if(!node) return "null";

    /* 字面量 */
    if(node->type == AST_INT) return "int";
    if(node->type == AST_NUM) return "double";
    if(node->type == AST_BOOL) return "bool";
    if(node->type == AST_CHAR) return "char";
    if(node->type == AST_STRING) return "string";
    if(node->type == AST_NONE) return "null";

    /* 类型标注 <int>expr → 返回标注的类型 */
    if(node->type == AST_TYPE_ANNOTATION) {
        return castkind_to_name(node->u.type_annotation.cast_type);
    }

    /* 强转 (int)expr → 返回强转的类型 */
    if(node->type == AST_CAST) {
        return castkind_to_name(node->u.cast.cast_type);
    }

    /* 变量：查符号表，返回变量的精确类型 */
    if(node->type == AST_VAR) {
        int bf_idx = bf_sym(c->fn, node->u.varname);
        if(bf_idx >= 0 && bf_idx < c->fn->sym_cnt) {
            CastKind ck = (CastKind)c->fn->var_type_tags[bf_idx];
            return castkind_to_name(ck);
        }
        return "unknown";
    }

    /* 二元运算：根据左右操作数类型推导 */
    /* 注意：优先级顺序必须与 c_expr_cast_type 和 c_expr 中的分支顺序完全一致 */
    if(node->type == AST_BINOP) {
        const char* lt = c_expr_type_name(c, node->u.bin.left);
        const char* rt = c_expr_type_name(c, node->u.bin.right);
        /* 1. string 优先级最高（字符串拼接） */
        if(strcmp(lt, "string") == 0 || strcmp(rt, "string") == 0) return "string";
        /* 2. bigint 次之 */
        if(strcmp(lt, "bigint") == 0 || strcmp(rt, "bigint") == 0) return "bigint";
        /* 3. decimal 再次之 */
        if(strcmp(lt, "decimal") == 0 || strcmp(rt, "decimal") == 0) return "decimal";
        /* 4. 浮点类型（float 自动提升为 double） */
        if(strcmp(lt, "double") == 0 || strcmp(rt, "double") == 0) return "double";
        if(strcmp(lt, "float") == 0 || strcmp(rt, "float") == 0) return "double";
        /* 5. 其他都是 int */
        return "int";
    }

    /* 赋值语句：返回右值的类型 */
    if(node->type == AST_ASSIGN) {
        return c_expr_type_name(c, node->u.assign.expr);
    }

    /* 一元运算：递归分析子表达式 */
    if(node->type == AST_UNARY) {
        return c_expr_type_name(c, node->u.uny.child);
    }

    return "unknown";
}

/* 获取表达式的精确类型（CastKind），用于打印格式化 */
static CastKind c_expr_cast_type(Ctx* c, AstNode* node) {
    if(!node) return CAST_NONE;

    /* 字面量 */
    if(node->type == AST_INT) return CAST_INT;
    if(node->type == AST_NUM) return CAST_DOUBLE;
    if(node->type == AST_BOOL) return CAST_BOOL;
    if(node->type == AST_CHAR) return CAST_CHAR;
    if(node->type == AST_STRING) return CAST_STRING;

    /* 类型标注 */
    if(node->type == AST_TYPE_ANNOTATION) {
        return node->u.type_annotation.cast_type;
    }
    if(node->type == AST_CAST) {
        return node->u.cast.cast_type;
    }

    /* 变量：查符号表 */
    if(node->type == AST_VAR) {
        int idx = c_find_var(c, node->u.varname);
        if(idx >= 0) {
            /* 从 BytecodeFunc 的 var_type_tags 获取 */
            int bf_idx = bf_sym(c->fn, node->u.varname);
            if(bf_idx >= 0 && bf_idx < c->fn->sym_cnt) {
                return (CastKind)c->fn->var_type_tags[bf_idx];
            }
        }
    }

    /* 函数调用：取 callee 返回类型标注（无标注 → NONE） */
    if(node->type == AST_CALL) {
        BytecodeFunc* callee = ir_func_table_lookup(node->u.call.name);
        if(callee && callee->ret_type_name)
            return ir_type_name_to_castkind(callee->ret_type_name);
        return CAST_NONE;
    }

    /* 二元运算：递归判断（严格遵循 C/C++ 算术类型提升规则） */
    if(node->type == AST_BINOP) {
        CastKind lt = c_expr_cast_type(c, node->u.bin.left);
        CastKind rt = c_expr_cast_type(c, node->u.bin.right);
        
        /* 1. string 优先级最高：任何类型 + string 都是字符串拼接 */
        if(lt == CAST_STRING || rt == CAST_STRING) return CAST_STRING;
        
        /* 2. bigint 次之：bigint 吸收所有类型（除 string） */
        if(lt == CAST_BIGINT || rt == CAST_BIGINT) return CAST_BIGINT;

        /* 2.5 bitdecimal 再次之：bitdecimal 吸收所有类型（除 string/bigint） */
        if(lt == CAST_BITDECIMAL || rt == CAST_BITDECIMAL) return CAST_BITDECIMAL;

        /* 3. decimal 再次之：decimal 吸收所有类型（除 string/bigint/bitdecimal） */
        if(lt == CAST_DECIMAL || rt == CAST_DECIMAL) return CAST_DECIMAL;
        
        /* 4. 浮点类型提升（C/C++ 规则：float 自动提升为 double） */
        if(lt == CAST_LONG_DOUBLE || rt == CAST_LONG_DOUBLE) return CAST_LONG_DOUBLE;
        if(lt == CAST_DOUBLE || rt == CAST_DOUBLE) return CAST_DOUBLE;
        if(lt == CAST_FLOAT || rt == CAST_FLOAT) return CAST_DOUBLE;  /* float 提升为 double */
        
        /* 5. 整数类型提升（C/C++ 规则：窄类型自动扩到 int） */
        /* 这里统一返回 CAST_INT，因为所有窄类型都映射到 INT64 栈 */
        return CAST_INT;
    }

    return CAST_NONE;
}

/* 把栈顶值从 from 转换为目标 ExprType（仅处理已支持的跨栈转换） */
static void emit_value_cast(Ctx* c, ExprType from, ExprType to) {
    if(from == to) return;
    if(to == EXPR_TYPE_NONE) return;   /* 目标 VALUE：装箱由 emit_to_dynamic 负责 */
    if(from == EXPR_TYPE_NONE) {
        /* 源 VALUE（动态） -> 目标 typed：拆箱 */
        if(to == EXPR_TYPE_INT)
            emit(c, OPC_UNBOX_INT64, 0, 0);
        else if(to == EXPR_TYPE_DOUBLE)
            emit(c, OPC_UNBOX_DOUBLE, 0, 0);
        /* VALUE -> PTR 无通用拆箱，暂不支持 */
        return;
    }
    if(to == EXPR_TYPE_DOUBLE && from == EXPR_TYPE_INT)
        emit(c, OPC_INT64_TO_DOUBLE, 0, 0);
    else if(to == EXPR_TYPE_INT && from == EXPR_TYPE_DOUBLE)
        emit(c, OPC_DOUBLE_TO_INT64, 0, 0);
    else if(to == EXPR_TYPE_PTR) {
        /* 数值 -> string（结果 PTR 栈）；数值到其它 PTR 类型无意义 */
        if(from == EXPR_TYPE_INT)
            emit(c, OPC_INT64_TO_STRING, 0, 0);
        else if(from == EXPR_TYPE_DOUBLE)
            emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
    }
    /* PTR/string 与数值的其它跨类转换当前不支持，保持原样 */
}

/* 判断表达式是否"数值型"（可能产生数值 VALUE）。
   用于决定赋值给已 typed 变量时是 unbox 还是改类型为 NONE。 */
static int is_numeric_expr(AstNode* n) {
    if(!n) return 0;
    switch(n->type) {
    case AST_INT: case AST_NUM: case AST_BOOL: case AST_CHAR:
    case AST_BINOP: case AST_UNARY: case AST_CAST:
        return 1;
    default:
        return 0;
    }
}

/* typed 栈 -> VALUE 栈装箱：实参类型已知、形参为动态 NONE 时使用。
   ck 为实参 CastKind（PTR 装箱需要，决定包装成何种 Value）。 */
static void emit_to_dynamic(Ctx* c, ExprType from, CastKind ck) {
    if (from == EXPR_TYPE_NONE) return;   /* 已在 VALUE 栈 */
    if (from == EXPR_TYPE_INT)
        emit(c, OPC_BOX_INT64, 0, 0);
    else if (from == EXPR_TYPE_DOUBLE)
        emit(c, OPC_BOX_DOUBLE, 0, 0);
    else /* PTR */
        emit(c, OPC_BOX_PTR, (int)ck, 0);
}

/* ===== 上下文目标类型（自顶向下）编译 =====
 * 当使用处需要 VALUE 栈（无标注形参/动态调用实参/数组元素/下标）时，
 * 直接按目标 VALUE 编译表达式，消除"typed 压栈 + BOX"开销。 */

/* cast 是否为特殊/非标量数值类型（这些仍走各自专用路径 + box） */
static int cast_is_special_value(CastKind k) {
    return k == CAST_STRING || k == CAST_BIGINT ||
           k == CAST_DECIMAL || k == CAST_BITDECIMAL;
}

/* 二元运算符对应的通用 Value 操作码；不可直接走 Value 时返回 -1 */
static int binop_value_opcode(int op) {
    switch(op) {
    case OP_ADD: return OPC_VADD;
    case OP_SUB: return OPC_VSUB;
    case OP_MUL: return OPC_VMUL;
    case OP_DIV: return OPC_VDIV;
    case OP_MOD: return OPC_VMOD;
    case OP_GT:  return OPC_VGT;
    case OP_LT:  return OPC_VLT;
    case OP_GE:  return OPC_VGE;
    case OP_LE:  return OPC_VLE;
    case OP_EQ:  return OPC_VEQ;
    case OP_NE:  return OPC_VNE;
    default:     return -1;
    }
}

/* 兜底：按自然类型编译，再按需 BOX 到 VALUE（已是 NONE 时 emit_to_dynamic 无操作） */
static void c_value_fallback(Ctx* c, AstNode* node) {
    ExprType t = c_expr(c, node);
    emit_to_dynamic(c, t, c_expr_cast_type(c, node));
}

/* 编译表达式，保证结果落在 VALUE 栈 */
static void c_expr_to_value(Ctx* c, AstNode* node) {
    if(!node) return;
    switch(node->type) {
    /* 字面量：直接构造 Value，零 typed 压栈、零 BOX */
    case AST_INT: {
        int64_t v = node->u.inum;
        if(v >= INT32_MIN && v <= INT32_MAX)
            emit(c, OPC_PUSH_INT_VAL, (int)v, 0);
        else {
            int idx = bf_add_i64_const(c->fn, v);
            emit(c, OPC_PUSH_CONST_VAL, idx, 0);
        }
        return;
    }
    case AST_BOOL: emit(c, OPC_PUSH_INT_VAL, node->u.bval ? 1 : 0, 0); return;
    case AST_CHAR: emit(c, OPC_PUSH_INT_VAL, (int)(int64_t)node->u.ch, 0); return;
    case AST_NUM: {
        int idx = bf_add_double_const(c->fn, node->u.num);
        emit(c, OPC_PUSH_CONST_VAL, idx, 0);
        return;
    }
    case AST_STRING: {
        int idx = bf_add_str_const(c->fn, node->u.sval);
        emit(c, OPC_PUSH_CONST_VAL, idx, 0);
        return;
    }
    case AST_NONE: emit(c, OPC_PUSH_NONE, 0, 0); return;

    /* 变量：仅当本身就是动态变量时直接 LOAD_VAR；typed 存储无法避免一次 BOX */
    case AST_VAR: {
        int idx = c_find_var(c, node->u.varname);
        if(idx >= 0 && c->var_types[idx] == EXPR_TYPE_NONE) {
            emit(c, OPC_LOAD_VAR, idx, 0);
            return;
        }
        c_value_fallback(c, node);
        return;
    }

    /* 一元负号：子树目标 VALUE + VNEG */
    case AST_UNARY: {
        if(node->u.uny.op == OP_UNARY_MINUS &&
           !cast_is_special_value(c_expr_cast_type(c, node->u.uny.child))) {
            c_expr_to_value(c, node->u.uny.child);
            emit(c, OPC_VNEG, 0, 0);
            return;
        }
        c_value_fallback(c, node);
        return;
    }

    /* 二元运算：左右子树均目标 VALUE，发通用 Value 指令 */
    case AST_BINOP: {
        int vop = binop_value_opcode(node->u.bin.op);
        CastKind lk = c_expr_cast_type(c, node->u.bin.left);
        CastKind rk = c_expr_cast_type(c, node->u.bin.right);
        if(vop >= 0 && !cast_is_special_value(lk) && !cast_is_special_value(rk)) {
            c_expr_to_value(c, node->u.bin.left);
            c_expr_to_value(c, node->u.bin.right);
            emit(c, (OpCode)vop, 0, 0);
            return;
        }
        c_value_fallback(c, node);
        return;
    }

    default:
        c_value_fallback(c, node);
        return;
    }
}

/* 编译语句 */
/* 编译条件表达式，发出"假则跳转"（目标待回填），返回 jmp 指令序号。
 * 已知类型条件（比较/逻辑运算）在 INT64 栈；动态条件（无类型标注）在 VALUE 栈。 */
static int emit_cond_jump_if_false(Ctx* c, AstNode* cond) {
    ExprType t = c_expr(c, cond);
    if(t == EXPR_TYPE_NONE)
        return emit_here(c, OPC_JMP_IF_FALSE_V, 0, 0);
    return emit_here(c, OPC_JMP_IF_FALSE, 0, 0);
}

/* ===== 循环控制层（break/continue 出口登记） ===== */

static Layer* layer_top(Ctx* c) {
    if(c->layer_depth <= 0) return NULL;
    return &c->layers[c->layer_depth - 1];
}

static void layer_push(Ctx* c, int kind) {
    if(c->layer_depth >= c->layers_cap) {
        c->layers_cap = c->layers_cap ? c->layers_cap * 2 : 8;
        c->layers = (Layer*)realloc(c->layers, sizeof(Layer) * c->layers_cap);
        if(!c->layers) { perror("layer_push"); exit(EXIT_FAILURE); }
    }
    Layer* L = &c->layers[c->layer_depth++];
    L->kind = kind;
    L->brk = NULL; L->brk_cnt = L->brk_cap = 0;
    L->cont = NULL; L->cont_cnt = L->cont_cap = 0;
    L->brk_fin = NULL; L->brk_fin_cnt = L->brk_fin_cap = 0;
    L->cont_fin = NULL; L->cont_fin_cnt = L->cont_fin_cap = 0;
    L->cont_target = -1;
}

static void int_list_add(int** arr, int* cnt, int* cap, int v) {
    if(*cnt >= *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *arr = (int*)realloc(*arr, sizeof(int) * *cap);
        if(!*arr) { perror("int_list_add"); exit(EXIT_FAILURE); }
    }
    (*arr)[(*cnt)++] = v;
}

static void layer_pop(Ctx* c) {
    if(c->layer_depth <= 0) return;
    Layer* L = &c->layers[--c->layer_depth];
    free(L->brk); free(L->cont);
    free(L->brk_fin); free(L->cont_fin);
}

/* ===== try-finally 编译上下文：使 try 块内 return 编译为 PEND_RETURN =====
   Ctx.fin_* 是按嵌套层组织的待回填列表：
   fin_jmp[d]  = 正常路径跳 finally 的 JMP 位置（回填 a）；
   fin_pend[d] = try 内 return 的 PEND_RETURN 位置（回填 b）。 */

static void fin_enter(Ctx* c) {
    if(c->fin_depth >= c->fin_cap) {
        c->fin_cap = c->fin_cap ? c->fin_cap * 2 : 8;
        c->fin_pend     = (int**)realloc(c->fin_pend,     sizeof(int*) * c->fin_cap);
        c->fin_pend_n   = (int*) realloc(c->fin_pend_n,   sizeof(int)  * c->fin_cap);
        c->fin_pend_cap = (int*) realloc(c->fin_pend_cap, sizeof(int)  * c->fin_cap);
        c->fin_jmp      = (int**)realloc(c->fin_jmp,      sizeof(int*) * c->fin_cap);
        c->fin_jmp_n    = (int*) realloc(c->fin_jmp_n,    sizeof(int)  * c->fin_cap);
        c->fin_jmp_cap  = (int*) realloc(c->fin_jmp_cap,  sizeof(int)  * c->fin_cap);
        if(!c->fin_pend || !c->fin_pend_n || !c->fin_pend_cap ||
           !c->fin_jmp  || !c->fin_jmp_n  || !c->fin_jmp_cap) {
            perror("fin_enter"); exit(EXIT_FAILURE);
        }
    }
    int d = c->fin_depth++;
    c->fin_pend[d] = NULL; c->fin_pend_n[d] = c->fin_pend_cap[d] = 0;
    c->fin_jmp[d]  = NULL; c->fin_jmp_n[d]  = c->fin_jmp_cap[d]  = 0;
}

static void fin_add_pend(Ctx* c, int pos) {
    int d = c->fin_depth - 1;
    int_list_add(&c->fin_pend[d], &c->fin_pend_n[d], &c->fin_pend_cap[d], pos);
}

static void fin_add_jmp(Ctx* c, int pos) {
    int d = c->fin_depth - 1;
    int_list_add(&c->fin_jmp[d], &c->fin_jmp_n[d], &c->fin_jmp_cap[d], pos);
}

/* fin_pc 已确定：回填当前层所有跳转并退出该上下文 */
static void fin_resolve_exit(Ctx* c, int fin_pc) {
    int d = c->fin_depth - 1;
    for(int i = 0; i < c->fin_jmp_n[d]; i++)
        bf_patch(c->fn, c->fin_jmp[d][i], fin_pc);   /* JMP.a */
    for(int i = 0; i < c->fin_pend_n[d]; i++)
        c->fn->code[c->fin_pend[d][i]].b = fin_pc;    /* PEND_RETURN.b */
    free(c->fin_pend[d]);
    free(c->fin_jmp[d]);
    c->fin_depth--;
}

/* patch 列表中全部 JMP 到当前位置 */
static void patch_list_here(Ctx* c, int* list, int cnt) {
    for(int i = 0; i < cnt; i++)
        patch_to(c, list[i]);
}

/* FIN_PUSH 列表的 b 回填到当前 pc（try-finally 内 break 出口） */
static void patch_fin_list_here(Ctx* c, int* list, int cnt) {
    for(int i = 0; i < cnt; i++)
        c->fn->code[list[i]].b = c->fn->code_len;
}

/* 编译 for 的 init/update 子句：赋值/序列（赋值自带 STORE，不留栈）。
 * SEQ 走 c_stmt 的迭代编译；单个赋值走 c_expr；其它表达式直接编译。 */
static void compile_for_effect(Ctx* c, AstNode* n) {
    if(!n) return;
    if(n->type == AST_SEQ)
        c_stmt(c, n);
    else
        c_expr(c, n);
}

/* 收集实参：ast_arg_append 构建左倾 SEQ 树（(((a,b),c),d)），
 * 用中序遍历得到左至右顺序 [a,b,c,d]。 */
static void collect_call_args(AstNode* n, AstNode*** argv, int* argc, int* acap) {
    if(!n) return;
    if(n->type == AST_SEQ) {
        collect_call_args(n->u.seq.first, argv, argc, acap);
        collect_call_args(n->u.seq.second, argv, argc, acap);
    } else {
        if(*argc >= *acap) {
            *acap = *acap ? *acap * 2 : 8;
            *argv = (AstNode**)realloc(*argv, sizeof(AstNode*) * *acap);
            if(!*argv) { perror("collect_call_args argv"); exit(EXIT_FAILURE); }
        }
        (*argv)[(*argc)++] = n;
    }
}

/* 编译用户自定义函数调用。
 * 实参按形参声明左至右绑定，按形参类型转换；缺实参用默认值；个数校验。
 * 发 OPC_CALL(a=callsite 下标, b=绑定参数个数)，返回 callee 返回类型。
 * 注意：可变形参 ...args 由 Task 9 处理，本函数遇到即停（多余实参 Task 9 再组装）。 */
static ExprType compile_user_call(Ctx* c, BytecodeFunc* callee, AstNode* def_ast, AstNode* args, int keep_result)
{
    /* 1. 收集实参到数组（左至右，中序遍历左倾 SEQ 树） */
    int argc = 0, acap = 0;
    AstNode** argv = NULL;
    collect_call_args(args, &argv, &argc, &acap);

    /* 2. 逐形参绑定（可变形参前停止） */
    int bound = 0;
    int* ref_flags = (int*)calloc(argc > 0 ? argc : 1, sizeof(int));
    int* ref_slots = (int*)malloc((argc > 0 ? argc : 1) * sizeof(int));
    for(int i = 0; i < (argc > 0 ? argc : 1); i++) ref_slots[i] = -1;
    for(AstNode* p = def_ast->u.func_def.params; p; p = p->u.param.next) {
        if(p->u.param.is_ellipsis) break;      /* 可变槽 Task 9 */
        int slot = bound;
        /* 形参类型（slot==形参符号下标；var_type_tags 初值 -1） */
        CastKind pck = (slot < callee->sym_cnt) ? (CastKind)callee->var_type_tags[slot] : CAST_NONE;
        ExprType param_et = castkind_to_exprtype(pck);

        if(p->u.param.is_ref && slot < argc) {
            /* ref 形参：实参必须是左值（当前支持变量） */
            AstNode* arg = argv[slot];
            if(arg->type != AST_VAR) {
                fprintf(stderr, "IR: 调用 %s 的 ref 形参 %s 需要左值（变量）\n",
                        callee->name ? callee->name : "?", p->u.param.name);
            } else {
                int cslot = c_find_var(c, arg->u.varname);
                if(cslot >= 0) {
                    ref_flags[slot] = 1;
                    ref_slots[slot] = cslot;
                }
            }
        }

        if(slot < argc) {
            if(param_et != EXPR_TYPE_NONE) {
                ExprType at = c_expr(c, argv[slot]);
                emit_value_cast(c, at, param_et);   /* typed -> typed */
            } else {
                c_expr_to_value(c, argv[slot]);      /* 上下文目标 VALUE，免 BOX */
            }
        } else if(p->u.param.default_val) {
            if(param_et != EXPR_TYPE_NONE) {
                ExprType at = c_expr(c, p->u.param.default_val);
                emit_value_cast(c, at, param_et);
            } else {
                c_expr_to_value(c, p->u.param.default_val);
            }
        } else {
            fprintf(stderr, "IR: 调用 %s 缺少第 %d 个必填参数\n",
                    callee->name ? callee->name : "?", slot + 1);
        }
        bound++;
    }

    /* 3. 可变参数：超出普通形参的实参组装为数组，绑定到可变槽（PTR/VALUE） */
    int total = bound;
    if(callee->has_variadic) {
        int extra = argc - bound;
        if(extra < 0) extra = 0;
        for(int i = 0; i < extra; i++) {
            c_expr_to_value(c, argv[bound + i]);
        }
        emit(c, OPC_ARRAY_LIT, 0, extra);   /* 弹 extra 个 VALUE，压数组 */
        total = bound + 1;                  /* 数组作为第 bound 个槽（可变形参） */
    } else if(argc > bound) {
        fprintf(stderr, "IR: 调用 %s 实参过多：%d 个，最多 %d 个\n",
                callee->name ? callee->name : "?", argc, bound);
    }

    /* 4. 按 callee 返回标注确定结果类型（无标注→动态 NONE），供 callsite 记录压栈目标 */
    ExprType ret_et = EXPR_TYPE_NONE;
    if(callee->ret_type_name) {
        CastKind rck = ir_type_name_to_castkind(callee->ret_type_name);
        ret_et = castkind_to_exprtype(rck);
    }

    /* 5. 登记调用点并发 CALL */
    int cs = bf_add_callsite(c->fn, callee->name ? callee->name : "?",
                             total, keep_result, (int)ret_et);
    CallSite* csp = &c->fn->callsites[cs];
    for(int i = 0; i < bound; i++) {
        csp->arg_is_ref[i] = ref_flags[i];
        csp->arg_ref_slots[i] = ref_slots[i];
    }
    free(ref_flags);
    free(ref_slots);
    emit(c, OPC_CALL, cs, total);
    free(argv);

    return ret_et;
}

void c_stmt(Ctx* c, AstNode* node) {
    if(!node) return;

    switch(node->type) {
    case AST_PRINT: {
        /* print 语句：支持多参数（arg_list 为 SEQ 链），逐个编译并打印 */
        AstNode* args = node->u.print.args;
        if(args) {
            AstNode** argv = NULL;
            int argc = 0, acap = 0;
            collect_call_args(args, &argv, &argc, &acap);
            for(int i = 0; i < argc; i++) {
                ExprType arg_type = c_expr(c, argv[i]);
                CastKind cast_type = c_expr_cast_type(c, argv[i]);
                if(arg_type == EXPR_TYPE_INT) {
                    emit(c, OPC_PRINT_INT64, (int)cast_type, 0);
                } else if(arg_type == EXPR_TYPE_DOUBLE) {
                    emit(c, OPC_PRINT_DOUBLE, (int)cast_type, 0);
                } else if(arg_type == EXPR_TYPE_PTR) {
                    if(cast_type == CAST_BIGINT) {
                        emit(c, OPC_PRINT_BIGINT, (int)cast_type, 0);
                    } else if(cast_type == CAST_DECIMAL) {
                        emit(c, OPC_PRINT_DECIMAL, (int)cast_type, 0);
                    } else if(cast_type == CAST_BITDECIMAL) {
                        /* 无 PRINT_BITDECIMAL：先转字符串再打印 */
                        emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0);
                        emit(c, OPC_PRINT_PTR, 0, 0);
                    } else {
                        emit(c, OPC_PRINT_PTR, (int)cast_type, 0);
                    }
                } else {
                    emit(c, OPC_PRINT, (int)cast_type, 0);
                }
            }
            free(argv);
        }
        break;
    }

    case AST_ASSIGN: {
        /* 赋值语句：注册变量，根据表达式类型选择存储指令 */
        const char* var_name = node->u.assign.varname;
        ExprType rt = c_expr(c, node->u.assign.expr);
        CastKind cast_type = c_expr_cast_type(c, node->u.assign.expr);
        /* 保持变量已有类型：若已声明为 typed，将 RHS 转换到该类型存储，
           避免 load/store 槽位错位（int_slots vs vals）。 */
        int exist_idx = c_find_var(c, var_name);
        ExprType target_et;
        if (exist_idx >= 0) {
            target_et = c->var_types[exist_idx];
        } else {
            target_et = rt;
        }
        /* 判断 typed->typed 是否存在真实转换（仅 INT<->DOUBLE）；
           不存在则放弃旧类型，变量改用 RHS 实际类型，避免栈错位。 */
        if (rt != target_et && rt != EXPR_TYPE_NONE && target_et != EXPR_TYPE_NONE) {
            int convertible = (rt == EXPR_TYPE_INT || rt == EXPR_TYPE_DOUBLE) &&
                              (target_et == EXPR_TYPE_INT || target_et == EXPR_TYPE_DOUBLE);
            if (!convertible) target_et = rt;
        }
        int var_idx = c_add_var(c, var_name, target_et);
        int bf_idx = bf_sym(c->fn, var_name);
        /* 若 RHS 类型与目标类型不同，进行转换 */
        if (rt != target_et) {
            if (target_et == EXPR_TYPE_NONE) {
                /* 目标是动态 VALUE：typed -> box */
                emit_to_dynamic(c, rt, cast_type);
            } else if (rt == EXPR_TYPE_NONE) {
                /* RHS 是动态 VALUE：
                   - 若 RHS 是数值型表达式（算术/字面量），unbox 到目标 typed
                   - 否则（函数返回/数组/lambda/字符串/null），变量改类型为 NONE */
                if (is_numeric_expr(node->u.assign.expr) &&
                    (target_et == EXPR_TYPE_INT || target_et == EXPR_TYPE_DOUBLE)) {
                    if (target_et == EXPR_TYPE_INT)
                        emit(c, OPC_UNBOX_INT64, 0, 0);
                    else
                        emit(c, OPC_UNBOX_DOUBLE, 0, 0);
                } else {
                    target_et = EXPR_TYPE_NONE;
                    c_add_var(c, var_name, EXPR_TYPE_NONE);
                }
            } else {
                /* typed -> typed 真实转换（INT<->DOUBLE） */
                emit_value_cast(c, rt, target_et);
            }
        }
        /* 记录精确类型到 BytecodeFunc 的 var_type_tags */
        if (target_et == EXPR_TYPE_INT)
            c->fn->var_type_tags[bf_idx] = (int)CAST_INT64;
        else if (target_et == EXPR_TYPE_DOUBLE)
            c->fn->var_type_tags[bf_idx] = (int)CAST_DOUBLE;
        else if (target_et == EXPR_TYPE_PTR)
            c->fn->var_type_tags[bf_idx] = (int)cast_type;
        else
            c->fn->var_type_tags[bf_idx] = (int)CAST_NONE;
        if(target_et == EXPR_TYPE_INT) {
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
        } else if(target_et == EXPR_TYPE_DOUBLE) {
            emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
        } else if(target_et == EXPR_TYPE_PTR) {
            emit(c, OPC_STORE_PTR_VAR, var_idx, 0);
        } else {
            emit(c, OPC_STORE_VAR, var_idx, 0);
        }
        break;
    }
    
    case AST_SEQ: {
        /* 语句序列：迭代遍历，避免长链表导致栈溢出 */
        /* AST_SEQ 可能是左偏树或右偏树，用栈模拟递归。
           栈动态扩容：固定容量会在长程序中静默丢弃 SEQ 节点，
           导致超出深度的语句既不编译也不执行。 */
        int stack_cap = 256;
        AstNode** stack = (AstNode**)malloc(sizeof(AstNode*) * stack_cap);
        if(!stack) { perror("c_stmt AST_SEQ"); exit(EXIT_FAILURE); }
        int sp = 0;
        AstNode* cur = node;

        while(cur || sp > 0) {
            /* 向左遍历到底，把路径上的节点压栈 */
            while(cur && cur->type == AST_SEQ) {
                if(sp >= stack_cap) {
                    stack_cap *= 2;
                    stack = (AstNode**)realloc(stack, sizeof(AstNode*) * stack_cap);
                    if(!stack) { perror("c_stmt AST_SEQ realloc"); exit(EXIT_FAILURE); }
                }
                stack[sp++] = cur;
                cur = cur->u.seq.first;
            }
            /* 处理叶子节点 */
            if(cur) {
                c_stmt(c, cur);
            }
            /* 弹出栈顶，处理 second */
            if(sp > 0) {
                AstNode* n = stack[--sp];
                cur = n->u.seq.second;
            } else {
                cur = NULL;
            }
        }
        free(stack);
        break;
    }

    case AST_BLOCK: {
        /* 块语句：编译块内语句序列 */
        c_stmt(c, node->u.block.stmts);
        break;
    }

    case AST_CALL: {
        /* 表达式语句调用：编译但丢弃返回值（keep_result=0） */
        const char* call_name = node->u.call.name;
        BytecodeFunc* callee = ir_func_table_lookup(call_name);
        AstNode* def_ast = func_ast_lookup(call_name);
        if(callee && def_ast) {
            compile_user_call(c, callee, def_ast, node->u.call.args, 0);
        } else {
            fprintf(stderr, "IR: unknown function %s\n", call_name);
        }
        break;
    }

    case AST_INDEX_ASSIGN: {
        /* 下标写语句：编译（INDEX_SET 压回 v），丢弃结果 */
        c_expr(c, node);
        emit(c, OPC_POP, 0, 0);
        break;
    }

    case AST_DYN_CALL: {
        /* 动态调用语句：编译并丢弃返回值（POP） */
        c_expr(c, node);
        emit(c, OPC_POP, 0, 0);
        break;
    }

    case AST_TRY: {
        /* 统一布局（单/多 catch + finally 任意组合）：
           TRY catch_pc fin_pc; <body>; JMP fin/after;
           catch_pc: 每子句 CATCH_MATCH→GET_ERR→STORE var→<body>→JMP;
                     全不匹配 → RETHROW（经 finally）或直接传播；
           fin_pc: <finally>; FINISH;
           after: ENDTRY
           body/clause 内 return 编译为 PEND_RETURN，经 finally 后才真正返回。 */
        AstNode* body    = node->u.trynode.body;
        AstNode* handler = node->u.trynode.catch_body;
        const char* evar = node->u.trynode.catch_var;
        AstNode* fin     = node->u.trynode.finally_body;
        int has_fin = (fin != NULL);

        /* 统一 clause 视图：单 catch（旧字段）虚拟成一个 catch-all 子句；多 catch 用数组 */
        CatchClause single; CatchClause* cl = NULL; int nclauses = 0;
        if(handler) {
            single.type = NULL; single.var = (char*)evar; single.body = handler;
            cl = &single; nclauses = 1;
        } else if(node->u.trynode.catch_count > 0) {
            cl = node->u.trynode.catches;
            nclauses = node->u.trynode.catch_count;
        }

        int try_pos = emit_here(c, OPC_TRY, 0, 0);
        if(has_fin) fin_enter(c);

        c_stmt(c, body);
        int body_jmp = emit_here(c, OPC_JMP, 0, 0);

        int catch_pc = 0;
        int* after_jmps = NULL; int after_n = 0, after_cap = 0;  /* 无 fin 时跳 after 的位置 */

        if(nclauses > 0) {
            catch_pc = c->fn->code_len;
            int prev_match = -1;
            for(int i = 0; i < nclauses; i++) {
                int entrance = c->fn->code_len;
                if(prev_match >= 0) c->fn->code[prev_match].b = entrance;
                int tc = -1;
                if(cl[i].type) tc = bf_add_str_const(c->fn, cl[i].type);
                prev_match = emit_here(c, OPC_CATCH_MATCH, tc, 0);
                emit(c, OPC_GET_ERR, 0, 0);
                if(cl[i].var) {
                    int vi = bf_sym(c->fn, cl[i].var);
                    emit(c, OPC_STORE_VAR, vi, 0);
                } else {
                    emit(c, OPC_POP, 0, 0);
                }
                c_stmt(c, cl[i].body);
                int end_jmp = emit_here(c, OPC_JMP, 0, 0);
                if(has_fin) fin_add_jmp(c, end_jmp);
                else int_list_add(&after_jmps, &after_n, &after_cap, end_jmp);
            }
            /* 所有子句均不匹配 */
            int no_match = c->fn->code_len;
            c->fn->code[prev_match].b = no_match;
            if(has_fin) {
                emit(c, OPC_FIN_PUSH, 2, 0);           /* RETHROW 完成动作 */
                int j = emit_here(c, OPC_JMP, 0, 0);
                fin_add_jmp(c, j);
            } else {
                emit(c, OPC_ENDTRY, 0, 0);    /* 先弹当前 try */
                emit(c, OPC_GET_ERR, 0, 0);
                emit(c, OPC_THROW, 0, 0);
            }
        }

        if(has_fin) {
            int fin_pc = c->fn->code_len;
            fin_add_jmp(c, body_jmp);
            c->fn->code[try_pos].b = fin_pc;
            fin_resolve_exit(c, fin_pc);   /* 回填全部 JMP.a 与 PEND_RETURN.b */
            c_stmt(c, fin);
            emit(c, OPC_FINISH, 0, 0);
        }

        int after_pc = c->fn->code_len;
        if(!has_fin) {
            bf_patch(c->fn, body_jmp, after_pc);
            for(int i = 0; i < after_n; i++) bf_patch(c->fn, after_jmps[i], after_pc);
            free(after_jmps);
            c->fn->code[try_pos].b = 0;
        }
        c->fn->code[try_pos].a = catch_pc;
        emit(c, OPC_ENDTRY, 0, 0);
        break;
    }

    case AST_THROW: {
        /* throw expr：把值统一到 VALUE 栈后 THROW */
        AstNode* e = node->u.thrownode.expr;
        ExprType t = c_expr(c, e);
        if(t != EXPR_TYPE_NONE)
            emit_to_dynamic(c, t, c_expr_cast_type(c, e));
        emit(c, OPC_THROW, 0, 0);
        break;
    }

    case AST_RETURN: {
        /* 返回语句：无值 RETURN_NIL；有值 c_expr 后 RETURN（a=返回 ExprType）。
           在 try-finally 内：编译为 PEND_RETURN，先执行 finally 再真正返回。 */
        AstNode* rv = node->u.ret.ret_val;

        if(c->fin_depth > 0) {
            int pr;
            if(!rv) {
                pr = emit_here(c, OPC_PEND_RETURN, 1, 0);   /* 无值返回 */
            } else {
                ExprType vt = c_expr(c, rv);
                ExprType target = vt;
                CastKind ret_ck = c_expr_cast_type(c, rv);
                if(c->fn->ret_type_name) {
                    CastKind tck = ir_type_name_to_castkind(c->fn->ret_type_name);
                    ExprType declared = castkind_to_exprtype(tck);
                    if(declared != EXPR_TYPE_NONE) {
                        emit_value_cast(c, vt, declared);
                        target = declared;
                        ret_ck = tck;
                    }
                }
                if(target != EXPR_TYPE_NONE)
                    emit_to_dynamic(c, target, ret_ck);
                pr = emit_here(c, OPC_PEND_RETURN, 0, 0);
            }
            fin_add_pend(c, pr);
            break;
        }

        if(!rv) {
            emit(c, OPC_RETURN_NIL, 0, 0);
            break;
        }
        ExprType vt = c_expr(c, rv);
        ExprType target = vt;
        CastKind ret_ck = c_expr_cast_type(c, rv);
        /* 按返回类型标注转换（如 func <int> f(...)） */
        if(c->fn->ret_type_name) {
            CastKind tck = ir_type_name_to_castkind(c->fn->ret_type_name);
            ExprType declared = castkind_to_exprtype(tck);
            if(declared != EXPR_TYPE_NONE) {
                emit_value_cast(c, vt, declared);
                target = declared;
                ret_ck = tck;
            }
        } else {
            /* 无返回标注：返回值统一 box 到 VALUE 栈，使 callsite（ret_stack=NONE）一致 */
            emit_to_dynamic(c, target, ret_ck);
            target = EXPR_TYPE_NONE;
            ret_ck = CAST_NONE;
        }
        /* a=ExprType（RETURN 从对应栈弹）；b=CastKind（PTR 字符串需深拷贝） */
        emit(c, OPC_RETURN, (int)target, (int)ret_ck);
        break;
    }

    case AST_IF_CHAIN: {
        /* if / elif* / else：
         *   cond; JMP_IF_FALSE -> 下一分支; body; JMP -> end
         * 各分支体后的无条件 JMP 全部汇合到末尾。 */
        int jmp_cap = 8, jmp_cnt = 0;
        int* end_jmps = (int*)malloc(sizeof(int) * jmp_cap);
        if(!end_jmps) { perror("AST_IF_CHAIN"); exit(EXIT_FAILURE); }
        #define ADD_END_JMP(pos) do { \
            if(jmp_cnt >= jmp_cap){ jmp_cap *= 2; end_jmps = (int*)realloc(end_jmps, sizeof(int) * jmp_cap); \
                if(!end_jmps){ perror("realloc"); exit(EXIT_FAILURE); } } \
            end_jmps[jmp_cnt++] = (pos); } while(0)

        int jf = emit_cond_jump_if_false(c, node->u.if_chain.cond);
        c_stmt(c, node->u.if_chain.if_body);
        ADD_END_JMP(emit_here(c, OPC_JMP, 0, 0));
        patch_to(c, jf);

        AstNode* e = node->u.if_chain.elif_list;
        while(e && e->type == AST_ELIF) {
            int ejf = emit_cond_jump_if_false(c, e->u.elif.cond);
            c_stmt(c, e->u.elif.body);
            ADD_END_JMP(emit_here(c, OPC_JMP, 0, 0));
            patch_to(c, ejf);
            e = e->u.elif.next;
        }

        if(node->u.if_chain.else_body)
            c_stmt(c, node->u.if_chain.else_body);

        for(int i = 0; i < jmp_cnt; i++)
            patch_to(c, end_jmps[i]);
        free(end_jmps);
        #undef ADD_END_JMP
        break;
    }

    case AST_IF: {
        /* 历史表示 ifnode（parser 当前走 IF_CHAIN；此处兜底支持嵌套 if）：
         *   cond; JMP_IF_FALSE -> else; then; JMP -> end; else; end */
        int jf = emit_cond_jump_if_false(c, node->u.ifnode.cond);
        c_stmt(c, node->u.ifnode.then_stmt);
        if(node->u.ifnode.elif_chain || node->u.ifnode.else_stmt) {
            int je = emit_here(c, OPC_JMP, 0, 0);
            patch_to(c, jf);
            if(node->u.ifnode.elif_chain)
                c_stmt(c, node->u.ifnode.elif_chain);  /* AST_IF 嵌套链 */
            if(node->u.ifnode.else_stmt)
                c_stmt(c, node->u.ifnode.else_stmt);
            patch_to(c, je);
        } else {
            patch_to(c, jf);
        }
        break;
    }

    case AST_BREAK: {
        /* break：直接 JMP 出口；在 try-finally 内则 FIN_PUSH(BREAK) 先执行 finally 再跳 */
        Layer* L = layer_top(c);
        if(!L) { fprintf(stderr, "IR: break outside loop\n"); break; }
        if(c->fin_depth > 0) {
            int fp = emit_here(c, OPC_FIN_PUSH, 3, 0);
            int_list_add(&L->brk_fin, &L->brk_fin_cnt, &L->brk_fin_cap, fp);
            int j = emit_here(c, OPC_JMP, 0, 0);
            fin_add_jmp(c, j);
        } else {
            int_list_add(&L->brk, &L->brk_cnt, &L->brk_cap,
                         emit_here(c, OPC_JMP, 0, 0));
        }
        break;
    }

    case AST_CONTINUE: {
        /* continue：跳 cond/更新头；try-finally 内 FIN_PUSH(CONT) 先执行 finally 再跳 */
        Layer* L = layer_top(c);
        if(!L) { fprintf(stderr, "IR: continue outside loop\n"); break; }
        if(c->fin_depth > 0) {
            int fp;
            if(L->cont_target >= 0) {
                fp = emit_here(c, OPC_FIN_PUSH, 4, L->cont_target);
            } else {
                fp = emit_here(c, OPC_FIN_PUSH, 4, 0);
                int_list_add(&L->cont_fin, &L->cont_fin_cnt, &L->cont_fin_cap, fp);
            }
            int j = emit_here(c, OPC_JMP, 0, 0);
            fin_add_jmp(c, j);
        } else if(L->cont_target >= 0) {
            emit(c, OPC_JMP, L->cont_target, 0);
        } else {
            int_list_add(&L->cont, &L->cont_cnt, &L->cont_cap,
                         emit_here(c, OPC_JMP, 0, 0));
        }
        break;
    }

    case AST_WHILE: {
        /* L_cond: cond; JMP_IF_FALSE -> end; body; JMP L_cond; end: */
        layer_push(c, 0);
        Layer* L = layer_top(c);
        int cond_pc = here(c);
        L->cont_target = cond_pc;
        int jf = emit_cond_jump_if_false(c, node->u.while_node.cond);
        c_stmt(c, node->u.while_node.body);
        emit(c, OPC_JMP, cond_pc, 0);
        patch_to(c, jf);
        patch_list_here(c, L->brk, L->brk_cnt);
        patch_fin_list_here(c, L->brk_fin, L->brk_fin_cnt);
        layer_pop(c);
        break;
    }

    case AST_DO_WHILE: {
        /* L_body: body; L_cond: cond; JMP_IF_TRUE -> L_body; end:
         * continue 跳 L_cond（编译 body 时位置未知，待 patch） */
        layer_push(c, 0);
        Layer* L = layer_top(c);
        int body_pc = here(c);
        c_stmt(c, node->u.while_node.body);
        int cond_pc = here(c);
        for(int i = 0; i < L->cont_cnt; i++)
            bf_patch(c->fn, L->cont[i], cond_pc);
        for(int i = 0; i < L->cont_fin_cnt; i++)
            c->fn->code[L->cont_fin[i]].b = cond_pc;
        ExprType ct = c_expr(c, node->u.while_node.cond);
        if(ct == EXPR_TYPE_NONE)
            emit(c, OPC_JMP_IF_TRUE_V, body_pc, 0);
        else
            emit(c, OPC_JMP_IF_TRUE, body_pc, 0);
        patch_list_here(c, L->brk, L->brk_cnt);
        patch_fin_list_here(c, L->brk_fin, L->brk_fin_cnt);
        layer_pop(c);
        break;
    }

    case AST_FOR: {
        /* init; L_cond: [cond; JMP_IF_FALSE -> end]; body; L_upd: update; JMP L_cond; end:
         * continue 跳 L_upd；无条件（cond=NULL）即永真。 */
        compile_for_effect(c, node->u.for_node.init);
        layer_push(c, 0);
        Layer* L = layer_top(c);
        int cond_pc = here(c);
        int jf = -1;
        if(node->u.for_node.cond)
            jf = emit_cond_jump_if_false(c, node->u.for_node.cond);
        c_stmt(c, node->u.for_node.body);
        int update_pc = here(c);
        for(int i = 0; i < L->cont_cnt; i++)
            bf_patch(c->fn, L->cont[i], update_pc);
        for(int i = 0; i < L->cont_fin_cnt; i++)
            c->fn->code[L->cont_fin[i]].b = update_pc;
        compile_for_effect(c, node->u.for_node.update);
        emit(c, OPC_JMP, cond_pc, 0);
        if(jf >= 0) patch_to(c, jf);
        patch_list_here(c, L->brk, L->brk_cnt);
        patch_fin_list_here(c, L->brk_fin, L->brk_fin_cnt);
        layer_pop(c);
        break;
    }

    /* 函数声明节点：顶层定义已由 func_compile 流程独立编译，main 顺序流中跳过。
     * 嵌套函数定义将在闭包任务（Task 12）中在此真正处理。 */
    case AST_FUNC_DEF:
    case AST_EXTERN_FUNC:
    case AST_PARAM:
        break;

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

/* 类型名 → CastKind（内置类型直接映射；自定义名查 struct/class 表） */
CastKind ir_type_name_to_castkind(const char* n) {
    if(!n) return CAST_NONE;
    if(0==strcmp(n,"int"))         return CAST_INT;
    if(0==strcmp(n,"double"))      return CAST_DOUBLE;
    if(0==strcmp(n,"string"))      return CAST_STRING;
    if(0==strcmp(n,"bool"))        return CAST_BOOL;
    if(0==strcmp(n,"char"))        return CAST_CHAR;
    if(0==strcmp(n,"byte"))        return CAST_BYTE;
    if(0==strcmp(n,"ascii"))       return CAST_ASCII;
    if(0==strcmp(n,"int8"))        return CAST_INT8;
    if(0==strcmp(n,"int16"))       return CAST_INT16;
    if(0==strcmp(n,"int32"))       return CAST_INT32;
    if(0==strcmp(n,"int64"))       return CAST_INT64;
    if(0==strcmp(n,"uint8"))       return CAST_UINT8;
    if(0==strcmp(n,"uint16"))      return CAST_UINT16;
    if(0==strcmp(n,"uint32"))      return CAST_UINT32;
    if(0==strcmp(n,"uint"))        return CAST_UINT;
    if(0==strcmp(n,"uint64"))      return CAST_UINT64;
    if(0==strcmp(n,"long"))        return CAST_LONG;
    if(0==strcmp(n,"long long"))   return CAST_LONGLONG;
    if(0==strcmp(n,"float"))       return CAST_FLOAT;
    if(0==strcmp(n,"ulong"))       return CAST_ULONG;
    if(0==strcmp(n,"uchar"))       return CAST_UCHAR;
    if(0==strcmp(n,"short"))       return CAST_SHORT;
    if(0==strcmp(n,"ushort"))      return CAST_USHORT;
    if(0==strcmp(n,"size_t"))      return CAST_SIZE_T;
    if(0==strcmp(n,"ssize_t"))     return CAST_SSIZE_T;
    if(0==strcmp(n,"void"))        return CAST_VOID;
    if(0==strcmp(n,"long double")) return CAST_LONG_DOUBLE;
    if(0==strcmp(n,"ptr"))         return CAST_PTR;
    if(0==strcmp(n,"bigint"))      return CAST_BIGINT;
    if(0==strcmp(n,"decimal"))     return CAST_DECIMAL;
    if(0==strcmp(n,"bitdecimal"))  return CAST_BITDECIMAL;
    /* 自定义类型名：struct → 结构体指针；class/type → class 指针 */
    if(struct_lookup(n)) return CAST_STRUCT_PTR;
    if(class_lookup(n) || type_lookup(n)) return CAST_CLASS_PTR;
    return CAST_NONE;
}

/* 把形参注册进 Ctx 变量表（下标必须与 bf_sym 槽位一致，保证函数体 STORE 索引正确） */
static void ctx_register_param(Ctx* c, const char* name, ExprType t) {
    if(c->var_cnt >= c->var_cap) {
        c->var_cap = c->var_cap ? c->var_cap * 2 : 16;
        c->var_names = realloc(c->var_names, c->var_cap * sizeof(char*));
        c->var_types = realloc(c->var_types, c->var_cap * sizeof(ExprType));
    }
    c->var_names[c->var_cnt] = strdup(name);
    c->var_types[c->var_cnt] = t;
    c->var_cnt++;
}

/* 释放编译上下文动态表（函数编译在 parse 期多次发生，避免 strdup 泄漏） */
static void ctx_cleanup(Ctx* c) {
    for(int i = 0; i < c->var_cnt; i++) free(c->var_names[i]);
    free(c->var_names);
    free(c->var_types);
    c->var_names = NULL; c->var_types = NULL;
    c->var_cnt = c->var_cap = 0;
}

/* 编译函数 / Compile a lumin function into bytecode and register it */
BytecodeFunc* ir_compile_function(const char* name, AstNode* params, AstNode* body, int is_generator, const char* class_name, const char* ret_type_name) {
    BytecodeFunc* fn = bytecode_func_new(name, 0);
    fn->is_generator = is_generator ? 1 : 0;
    fn->class_name = class_name ? strdup(class_name) : NULL;
    fn->ret_type_name = ret_type_name ? strdup(ret_type_name) : NULL;

    /* 编译上下文：形参按声明序注册（Ctx idx == bf 槽位 == frame 槽位） */
    Ctx c;
    memset(&c, 0, sizeof(Ctx));
    c.fn = fn;

    int total = 0, pcap = 0;
    for(AstNode* p = params; p; p = p->u.param.next) {
        /* fn->params / param_is_ref 动态数组 */
        if(total >= pcap) {
            pcap = pcap ? pcap * 2 : 8;
            fn->params = realloc(fn->params, pcap * sizeof(char*));
            fn->param_is_ref = realloc(fn->param_is_ref, pcap * sizeof(int));
        }
        /* 形参名预注册为函数符号（槽位下标即声明顺序） */
        int slot = bf_sym(fn, p->u.param.name);
        fn->params[total] = strdup(p->u.param.name);
        fn->param_is_ref[total] = p->u.param.is_ref;
        /* 形参类型标注：写 var_type_tags，并据此确定 Ctx 参数类型 */
        CastKind pck = ir_type_name_to_castkind(p->u.param.constraint);
        if(pck != CAST_NONE) fn->var_type_tags[slot] = (int)pck;
        ExprType et = castkind_to_exprtype(pck);   /* 无标注 → NONE（动态） */
        ctx_register_param(&c, p->u.param.name, et);
        if(p->u.param.is_ellipsis) fn->has_variadic = 1;
        else fn->param_cnt++;
        total++;
    }

    /* 先注册再编译函数体：使函数体内的自引用递归调用能查到自身。
       形参类型与返回标注此时已就绪；递归 CALL 真正运行时函数体已编译完整。 */
    ir_func_table_register(fn);

    /* lambda：注册捕获的外层变量为本地槽位（type NONE，VALUE 栈访问） */
    if(name && strncmp(name, "_lambda_", 8) == 0) {
        int ncap = lambda_capture_count(name);
        for(int i = 0; i < ncap; i++) {
            const char* cname = lambda_capture_name(name, i);
            if(cname) c_add_var(&c, cname, EXPR_TYPE_NONE);
        }
    }

    /* 编译函数体；末尾隐式返回 null（显式 return 时该指令不可达，无害） */
    if(body) c_stmt(&c, body);
    emit(&c, OPC_RETURN_NIL, 0, 0);

    ctx_cleanup(&c);
    return fn;
}

/* ============================================================
 * 全局函数表（红黑树）  Global function table (red-black tree)
 * 键：NS_FUNCTION 普通函数 / NS_METHOD class 方法
 * ============================================================ */
static RBTree* g_func_table = NULL;

static RBTree* func_table_tree(void) {
    if(!g_func_table) g_func_table = rbtree_create();
    return g_func_table;
}

/* 注册/替换普通函数（同名旧函数被释放） / Register or replace a function */
void ir_func_table_register(BytecodeFunc* fn) {
    if(!fn || !fn->name) return;
    RBTree* t = func_table_tree();
    void* old = rbtree_set_data(t, NS_FUNCTION, NULL, fn->name, fn);
    if(old) {
        if(old != fn) bytecode_func_free((BytecodeFunc*)old);
    } else {
        rbtree_insert(t, NS_FUNCTION, NULL, fn->name, fn);
    }
}

/* 函数表查找 / Lookup a function by name */
BytecodeFunc* ir_func_table_lookup(const char* name) {
    if(!g_func_table || !name) return NULL;
    return (BytecodeFunc*)rbtree_find(g_func_table, NS_FUNCTION, NULL, name);
}

BytecodeFunc* ir_func_table_lookup_class(const char* class_name, const char* method_name) {
    if(!g_func_table || !class_name || !method_name) return NULL;
    return (BytecodeFunc*)rbtree_find(g_func_table, NS_METHOD, class_name, method_name);
}

/* lookup_any 遍历状态 */
typedef struct {
    const char* name;
    BytecodeFunc* found;
} LookupAnyCtx;

static void lookup_any_cb(RBTNamespace ns, const char* class_name, const char* name, void* data, void* user_data) {
    (void)ns; (void)class_name;
    LookupAnyCtx* ctx = (LookupAnyCtx*)user_data;
    if(!ctx->found && name && strcmp(name, ctx->name) == 0)
        ctx->found = (BytecodeFunc*)data;
}

BytecodeFunc* ir_func_table_lookup_any(const char* name) {
    BytecodeFunc* f = ir_func_table_lookup(name);
    if(f) return f;
    if(!g_func_table || !name) return NULL;
    LookupAnyCtx ctx = {name, NULL};
    rbtree_foreach_ns(g_func_table, NS_METHOD, lookup_any_cb, &ctx);
    return ctx.found;
}

static void reset_free_cb(RBTNamespace ns, const char* class_name, const char* name, void* data, void* user_data) {
    (void)ns; (void)class_name; (void)name; (void)user_data;
    bytecode_func_free((BytecodeFunc*)data);
}

void ir_func_table_reset(void) {
    if(!g_func_table) return;
    rbtree_foreach(g_func_table, reset_free_cb, NULL);
    rbtree_destroy(g_func_table);
    g_func_table = NULL;
}

/* rbtree 遍历回调（5 参数）→ 对外回调（4 参数）适配 */
static void (*g_ir_foreach_cb)(const char*, const char*, void*, void*);

static void foreach_adapter(RBTNamespace ns, const char* class_name, const char* name, void* data, void* user_data) {
    (void)ns;
    g_ir_foreach_cb(class_name, name, data, user_data);
}

void ir_func_table_foreach(void (*callback)(const char*, const char*, void*, void*), void* user_data) {
    if(!g_func_table || !callback) return;
    g_ir_foreach_cb = callback;
    rbtree_foreach(g_func_table, foreach_adapter, user_data);
}

BytecodeFunc* ir_func_table_recompile(const char* name, AstNode* params, AstNode* body, int is_generator, const char* class_name, const char* ret_type_name) {
    return ir_compile_function(name, params, body, is_generator, class_name, ret_type_name);
}

void string_cache_reset(void) {
}
