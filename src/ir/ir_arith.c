// lumyr-lang 算术运算类型分析
// 4 核心栈设计：STACK_VALUE / INT64 / DOUBLE / PTR
#include "ir_arith.h"
#include "ir_compile.h"

/* 获取变量的类型标记 */
CastKind get_var_cast_type(Ctx* c, const char* name) {
    if(!c || !c->fn || !name) return -1;
    int var_idx = bf_sym(c->fn, name);
    if(var_idx < 0 || var_idx >= c->fn->sym_cnt) return -1;
    return (CastKind)c->fn->var_type_tags[var_idx];
}

/* ============================================================
 * arith_get_expr_type：获取表达式的精确类型
 * 4 核心栈设计：所有整数合并到 INT64，所有浮点合并到 DOUBLE
 * ============================================================ */
ExprType arith_get_expr_type(Ctx* c, AstNode* node) {
    if(!node) return EXPR_TYPE_NONE;

    /* 变量引用：根据变量类型标记判断 */
    if(node->type == AST_VAR) {
        CastKind ct = get_var_cast_type(c, node->u.varname);
        switch(ct) {
            case CAST_INT:
            case CAST_INT8:
            case CAST_INT16:
            case CAST_INT32:
            case CAST_INT64:
            case CAST_LONGLONG:
            case CAST_LONG:
            case CAST_SHORT:
            case CAST_BOOL:
            case CAST_CHAR:
            case CAST_BYTE:
            case CAST_UINT8:
            case CAST_UINT16:
            case CAST_UINT32:
            case CAST_UINT64:
            case CAST_ULONG:
            case CAST_SIZE_T:
            case CAST_SSIZE_T:
                return EXPR_TYPE_INT;  /* 所有整数 → INT64 栈 */

            case CAST_FLOAT:
            case CAST_DOUBLE:
            case CAST_LONG_DOUBLE:
                return EXPR_TYPE_DOUBLE;  /* 所有浮点 → DOUBLE 栈 */

            case CAST_STRING:
            case CAST_ASCII:
                return EXPR_TYPE_PTR;  /* 字符串 → PTR 栈 */

            default:
                return EXPR_TYPE_NONE;  /* 动态类型 → Value 栈 */
        }
    }

    /* 字面量自动推导类型 */
    if(node->type == AST_INT) return EXPR_TYPE_INT;      /* 整数字面量 → int */
    if(node->type == AST_NUM) return EXPR_TYPE_DOUBLE;   /* 浮点字面量 → double */
    if(node->type == AST_BOOL) return EXPR_TYPE_INT;     /* 布尔字面量 → int (0/1) */
    if(node->type == AST_CHAR) return EXPR_TYPE_INT;     /* 字符字面量 → int (ASCII) */
    if(node->type == AST_STRING) return EXPR_TYPE_PTR; /* 字符串字面量 → string */

    /* 二元运算：递归判断左右操作数类型 */
    if(node->type == AST_BINOP) {
        ExprType left_type = arith_get_expr_type(c, node->u.bin.left);
        ExprType right_type = arith_get_expr_type(c, node->u.bin.right);

        /* 两个都是已知类型，取较高优先级 */
        if(left_type != EXPR_TYPE_NONE && right_type != EXPR_TYPE_NONE) {
            /* string 优先级最高（字符串拼接） */
            if(left_type == EXPR_TYPE_PTR || right_type == EXPR_TYPE_PTR) {
                return EXPR_TYPE_PTR;
            }
            /* double 优先级高于 int */
            if(left_type == EXPR_TYPE_DOUBLE || right_type == EXPR_TYPE_DOUBLE) {
                return EXPR_TYPE_DOUBLE;
            }
            /* 都是 int */
            return EXPR_TYPE_INT;
        }

        /* 一个已知，一个未知：保持已知类型（字面量提升） */
        if(left_type != EXPR_TYPE_NONE) return left_type;
        if(right_type != EXPR_TYPE_NONE) return right_type;

        /* 都是未知 → 动态类型 */
        return EXPR_TYPE_NONE;
    }

    return EXPR_TYPE_NONE;
}

/* ============================================================
 * arith_handle_assign_result：处理赋值结果
 * 根据表达式类型选择对应的专用存储指令
 * ============================================================ */
void arith_handle_assign_result(Ctx* c, AstNode* binop, int var_idx, ExprType result_type) {
    if(!c || !binop) return;

    switch(result_type) {
        case EXPR_TYPE_INT:
            /* 整数 → INT64 栈 */
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_INT;
            break;

        case EXPR_TYPE_DOUBLE:
            /* 浮点 → DOUBLE 栈 */
            c_expr(c, binop);
            emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_DOUBLE;
            break;

        case EXPR_TYPE_PTR:
            /* 字符串 → PTR 栈 */
            c_expr(c, binop);
            emit(c, OPC_STORE_PTR_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_STRING;
            break;

        default:
            /* 动态类型 → Value 栈 */
            c_expr(c, binop);
            emit(c, OPC_STORE_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = -1;
            break;
    }
}

/* ============================================================
 * arith_handle_print_result：处理打印结果
 * 根据表达式类型选择对应的打印指令
 * ============================================================ */
int arith_handle_print_result(Ctx* c, AstNode* single_arg, ExprType result_type) {
    if(!c || !single_arg) return 0;

    switch(result_type) {
        case EXPR_TYPE_INT:
            /* 整数 → 从 INT64 栈弹出打印 */
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;

        case EXPR_TYPE_DOUBLE:
            /* 浮点 → 从 DOUBLE 栈弹出打印 */
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_DOUBLE, 0, 0);
            return 1;

        default:
            /* 动态类型 → 走通用 Value 栈打印 */
            return 0;  /* 返回 0 表示走默认路径 */
    }
}
