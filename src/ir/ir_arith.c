/*
 * 运算优化模块实现
 *
 * 本模块负责各种数据类型的算术运算和比较运算的零开销优化。
 * 支持的数据类型包括：
 * - 有符号整数：int、int8、int16、int32、int64、long long、long、ssize_t
 * - 无符号整数：uint、uint8、uint16、uint32、uint64、unsigned long、size_t、byte
 * - 浮点数：float、double、long double
 * - 其他：bool、char
 *
 * 设计原则：
 * 1. 每个数据类型有自己的专属指令和通道，不混用通用指令
 * 2. 算术运算结果保持在专用栈中，后续操作通过上下文感知选择专用指令
 * 3. 不允许硬编码数字，使用枚举判断
 * 4. 不允许回滚代码，修改前必须询问用户
 */

#include "ir_arith.h"
#include "ir_compile.h"
#include <string.h>

/* ========== 辅助函数：获取变量的类型标记 ========== */

static CastKind get_var_cast_type(Ctx* c, const char* name) {
    if(!c || !c->fn || !name) return (CastKind)-1;
    int idx = bf_sym(c->fn, name);
    if(idx < 0) return (CastKind)-1;
    if(!c->fn->var_type_tags) return (CastKind)-1;
    return c->fn->var_type_tags[idx];
}

/* ========== 类型判断函数实现 ========== */

int arith_is_int_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_INT;
}

int arith_is_uint_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_UINT32;
}

int arith_is_double_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_DOUBLE;
}

int arith_is_float_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_FLOAT;
}

int arith_is_long_long_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_LONGLONG;
}

int arith_is_long_double_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_LONG_DOUBLE;
}

int arith_is_bool_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_BOOL;
}

int arith_is_char_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_CHAR;
}

int arith_is_byte_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_BYTE;
}

int arith_is_int8_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_INT8;
}

int arith_is_int16_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_INT16;
}

int arith_is_short_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_SHORT;
}

int arith_is_int32_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_INT32;
}

int arith_is_int64_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_INT64;
}

int arith_is_uint8_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_UINT8;
}

int arith_is_uint16_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_UINT16;
}

int arith_is_uchar_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_UCHAR;
}

int arith_is_ushort_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_USHORT;
}

int arith_is_uint32_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_UINT32;
}

int arith_is_uint64_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_UINT64;
}

int arith_is_long_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_LONG;
}

int arith_is_ulong_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_ULONG;
}

int arith_is_size_t_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_SIZE_T;
}

int arith_is_ssize_t_var(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_VAR) return 0;
    CastKind ct = get_var_cast_type(c, node->u.varname);
    return ct == CAST_SSIZE_T;
}

/* ========== 表达式类型判断 ========== */

ExprType arith_get_expr_type(Ctx* c, AstNode* node) {
    if(!node) return EXPR_TYPE_NONE;

    /* 变量引用：根据变量类型标记判断实际专用栈。
       配套 c_expr(AST_VAR) 发 LOAD_*_VAR（专用栈）、赋值发 STORE_*_VAR（专用栈）。 */
    if(node->type == AST_VAR) {
        CastKind ct = get_var_cast_type(c, node->u.varname);
        switch(ct) {
            case CAST_BOOL: return EXPR_TYPE_BOOL;
            case CAST_CHAR: return EXPR_TYPE_CHAR;
            case CAST_INT8: return EXPR_TYPE_INT8;
            case CAST_INT16: return EXPR_TYPE_INT16;
            case CAST_SHORT: return EXPR_TYPE_SHORT;
            case CAST_INT: return EXPR_TYPE_INT;
            case CAST_INT32: return EXPR_TYPE_INT32;
            case CAST_INT64: return EXPR_TYPE_INT64;
            case CAST_LONGLONG: return EXPR_TYPE_LONG_LONG;
            case CAST_LONG: return EXPR_TYPE_LONG;
            case CAST_BYTE: return EXPR_TYPE_BYTE;
            case CAST_UINT8: return EXPR_TYPE_UINT8;
            case CAST_UINT16: return EXPR_TYPE_UINT16;
            case CAST_UINT32: return EXPR_TYPE_UINT;
            case CAST_UINT64: return EXPR_TYPE_UINT64;
            case CAST_ULONG: return EXPR_TYPE_ULONG;
            case CAST_SIZE_T: return EXPR_TYPE_SIZE_T;
            case CAST_SSIZE_T: return EXPR_TYPE_SSIZE_T;
            case CAST_FLOAT: return EXPR_TYPE_FLOAT;
            case CAST_DOUBLE: return EXPR_TYPE_DOUBLE;
            case CAST_LONG_DOUBLE: return EXPR_TYPE_LONG_DOUBLE;
            default: return EXPR_TYPE_NONE;
        }
    }

    /* 类型化数组下标 a[i]：还原元素的 ExprType（变量 tag 为 VAR_TYPE_*_ARRAY）。
       这样多参数 print 等通用 Value 栈场景才能在取值后插入 *_TO_VALUE，避免跨栈错取。 */
    if(node->type == AST_INDEX) {
        AstNode* arr = node->u.index.arr;
        if(arr && arr->type == AST_VAR) {
            CastKind ct = get_var_cast_type(c, arr->u.varname);
            switch(ct) {
                case VAR_TYPE_INT_ARRAY:    return EXPR_TYPE_INT;
                case VAR_TYPE_DOUBLE_ARRAY: return EXPR_TYPE_DOUBLE;
                case VAR_TYPE_FLOAT_ARRAY:  return EXPR_TYPE_FLOAT;
                case VAR_TYPE_UINT_ARRAY:   return EXPR_TYPE_UINT;
                case VAR_TYPE_BOOL_ARRAY:   return EXPR_TYPE_BOOL;
                case VAR_TYPE_CHAR_ARRAY:   return EXPR_TYPE_CHAR;
                case VAR_TYPE_BYTE_ARRAY:   return EXPR_TYPE_BYTE;
                case VAR_TYPE_INT8_ARRAY:   return EXPR_TYPE_INT8;
                default:                    return EXPR_TYPE_NONE;
            }
        }
        return EXPR_TYPE_NONE;
    }

    /* 二元运算：递归判断左右操作数类型
       规则：
       - 两个都是已知类型 → 类型提升
       - 一个已知一个字面量 → 字面量提升为已知类型
       - 一个已知一个 Value 变量 → 整体走通用路径（NONE），因为 Value 变量不能直接转专用栈 */
    if(node->type == AST_BINOP) {
        ExprType left_type = arith_get_expr_type(c, node->u.bin.left);
        ExprType right_type = arith_get_expr_type(c, node->u.bin.right);
        /* 左操作数未知：检查是不是字面量（可以提升）还是 Value 变量（不能提升） */
        if(left_type == EXPR_TYPE_NONE) {
            /* 左操作数是字面量（AST_INT/AST_NUM 等），可以提升为右操作数类型 */
            if(node->u.bin.left->type == AST_INT || node->u.bin.left->type == AST_NUM) {
                return right_type;
            }
            /* 左操作数是 Value 变量或其他复杂表达式，不能直接转专用栈 */
            return EXPR_TYPE_NONE;
        }
        /* 右操作数未知：检查是不是字面量（可以提升）还是 Value 变量（不能提升） */
        if(right_type == EXPR_TYPE_NONE) {
            /* 右操作数是字面量，可以提升为左操作数类型 */
            if(node->u.bin.right->type == AST_INT || node->u.bin.right->type == AST_NUM) {
                return left_type;
            }
            /* 右操作数是 Value 变量或其他复杂表达式，不能直接转专用栈 */
            return EXPR_TYPE_NONE;
        }
        /* 两个都是已知类型，取较高优先级 */
        return (left_type > right_type) ? left_type : right_type;
    }

    /* 字面量：不自动推导类型，保持动态 Value 类型
       只有显式声明 <int>10 或变量有类型标记时才是专用类型 */
    if(node->type == AST_INT) return EXPR_TYPE_NONE;
    if(node->type == AST_NUM) return EXPR_TYPE_NONE;
    if(node->type == AST_BOOL) return EXPR_TYPE_NONE;
    if(node->type == AST_CHAR) return EXPR_TYPE_NONE;

    /* 类型转换：根据转换类型判断 */
    if(node->type == AST_CAST) {
        /* 强转 (int8)x：OPC_CAST_xxx 在 Value 栈上弹1压1，结果仍在 Value 栈，
           不进专用栈。返回 NONE 让 print/赋值走通用 Value 路径。 */
        return EXPR_TYPE_NONE;
    }
    if(node->type == AST_TYPE_ANNOTATION) {
        /* 类型标注 <int8>1000：折叠走 LOAD_CONST（Value 栈），非折叠走 OPC_CAST_xxx（Value 栈弹1压1）。
           结果基本都在 Value 栈，返回 NONE 让 print 走通用 PRINT（除 long double 保持专用栈）。 */
        if(node->u.type_annotation.cast_type == CAST_LONG_DOUBLE)
            return EXPR_TYPE_LONG_DOUBLE;
        return EXPR_TYPE_NONE;
    }

    return EXPR_TYPE_NONE;
}

/* ========== 算术运算和比较运算优化 ========== */

/*
 * 通用的算术运算优化函数
 *
 * 参数：
 *   c - 编译上下文
 *   left - 左操作数变量名
 *   right - 右操作数变量名
 *   load_op - 加载指令（如 OPC_LOAD_INT64_VAR）
 *   arith_map - 算术运算指令映射表
 *   cmp_map - 比较运算指令映射表
 *   bop - 运算符
 *   is_arith - 是否为算术运算
 *   is_cmp - 是否为比较运算
 */
static void emit_typed_arith(Ctx* c, const char* left, const char* right,
                               OpCode load_op,
                               const OpCode* arith_map,
                               const OpCode* cmp_map,
                               int bop, int is_arith, int is_cmp) {
    int left_idx = bf_sym(c->fn, left);
    int right_idx = bf_sym(c->fn, right);
    emit(c, load_op, left_idx, 0);
    emit(c, load_op, right_idx, 0);
    if(is_arith) {
        emit(c, arith_map[bop], 0, 0);
    } else if(is_cmp) {
        emit(c, cmp_map[bop], 0, 0);
    }
}

int arith_try_optimize_binop(Ctx* c, AstNode* node, int bop) {
    if(!c || !node || node->type != AST_BINOP) return 0;

    AstNode* left = node->u.bin.left;
    AstNode* right = node->u.bin.right;

    /* 只优化变量之间的运算 */
    if(!left || left->type != AST_VAR || !right || right->type != AST_VAR) return 0;

    int is_arith = (bop == OP_ADD || bop == OP_SUB || bop == OP_MUL || bop == OP_DIV || bop == OP_MOD);
    int is_cmp = (bop == OP_GT || bop == OP_LT || bop == OP_GE || bop == OP_LE || bop == OP_EQ || bop == OP_NE);

    if(!is_arith && !is_cmp) return 0;

    /* ========== int 类型 ========== */
    if(arith_is_int_var(c, left) && arith_is_int_var(c, right)) {
        static const OpCode int_arith_map[] = {
            [OP_ADD] = OPC_INT64_ADD, [OP_SUB] = OPC_INT64_SUB, [OP_MUL] = OPC_INT64_MUL,
            [OP_DIV] = OPC_INT64_DIV, [OP_MOD] = OPC_INT64_MOD,
        };
        static const OpCode int_cmp_map[] = {
            [OP_GT] = OPC_INT64_GT, [OP_LT] = OPC_INT64_LT, [OP_GE] = OPC_INT64_GE,
            [OP_LE] = OPC_INT64_LE, [OP_EQ] = OPC_INT64_EQ, [OP_NE] = OPC_INT64_NE,
        };
        emit_typed_arith(c, left->u.varname, right->u.varname, OPC_LOAD_INT64_VAR,
                         int_arith_map, int_cmp_map, bop, is_arith, is_cmp);
        return 1;
    }

    /* ========== uint 类型 ========== */
    if(arith_is_uint_var(c, left) && arith_is_uint_var(c, right)) {
        static const OpCode uint_arith_map[] = {
            [OP_ADD] = OPC_INT64_ADD, [OP_SUB] = OPC_INT64_SUB, [OP_MUL] = OPC_INT64_MUL,
            [OP_DIV] = OPC_INT64_DIV, [OP_MOD] = OPC_INT64_MOD,
        };
        static const OpCode uint_cmp_map[] = {
            [OP_GT] = OPC_INT64_GT, [OP_LT] = OPC_INT64_LT, [OP_GE] = OPC_INT64_GE,
            [OP_LE] = OPC_INT64_LE, [OP_EQ] = OPC_INT64_EQ, [OP_NE] = OPC_INT64_NE,
        };
        emit_typed_arith(c, left->u.varname, right->u.varname, OPC_LOAD_INT64_VAR,
                         uint_arith_map, uint_cmp_map, bop, is_arith, is_cmp);
        return 1;
    }

    /* ========== double 类型 ========== */
    if(arith_is_double_var(c, left) && arith_is_double_var(c, right)) {
        static const OpCode double_arith_map[] = {
            [OP_ADD] = OPC_DOUBLE_ADD, [OP_SUB] = OPC_DOUBLE_SUB, [OP_MUL] = OPC_DOUBLE_MUL,
            [OP_DIV] = OPC_DOUBLE_DIV,
        };
        static const OpCode double_cmp_map[] = {
            [OP_GT] = OPC_DOUBLE_GT, [OP_LT] = OPC_DOUBLE_LT, [OP_GE] = OPC_DOUBLE_GE,
            [OP_LE] = OPC_DOUBLE_LE, [OP_EQ] = OPC_DOUBLE_EQ, [OP_NE] = OPC_DOUBLE_NE,
        };
        emit_typed_arith(c, left->u.varname, right->u.varname, OPC_LOAD_DOUBLE_VAR,
                         double_arith_map, double_cmp_map, bop, is_arith, is_cmp);
        return 1;
    }

    /* ========== float 类型 ========== */
    if(arith_is_float_var(c, left) && arith_is_float_var(c, right)) {
        static const OpCode float_arith_map[] = {
            [OP_ADD] = OPC_DOUBLE_ADD, [OP_SUB] = OPC_DOUBLE_SUB, [OP_MUL] = OPC_DOUBLE_MUL,
            [OP_DIV] = OPC_DOUBLE_DIV,
        };
        static const OpCode float_cmp_map[] = {
            [OP_GT] = OPC_DOUBLE_GT, [OP_LT] = OPC_DOUBLE_LT, [OP_GE] = OPC_DOUBLE_GE,
            [OP_LE] = OPC_DOUBLE_LE, [OP_EQ] = OPC_DOUBLE_EQ, [OP_NE] = OPC_DOUBLE_NE,
        };
        emit_typed_arith(c, left->u.varname, right->u.varname, OPC_LOAD_DOUBLE_VAR,
                         float_arith_map, float_cmp_map, bop, is_arith, is_cmp);
        return 1;
    }

    /* ========== long long 类型 ========== */
    if(arith_is_long_long_var(c, left) && arith_is_long_long_var(c, right)) {
        static const OpCode ll_arith_map[] = {
            [OP_ADD] = OPC_INT64_ADD, [OP_SUB] = OPC_INT64_SUB, [OP_MUL] = OPC_INT64_MUL,
            [OP_DIV] = OPC_INT64_DIV, [OP_MOD] = OPC_INT64_MOD,
        };
        static const OpCode ll_cmp_map[] = {
            [OP_GT] = OPC_INT64_GT, [OP_LT] = OPC_INT64_LT, [OP_GE] = OPC_INT64_GE,
            [OP_LE] = OPC_INT64_LE, [OP_EQ] = OPC_INT64_EQ, [OP_NE] = OPC_INT64_NE,
        };
        emit_typed_arith(c, left->u.varname, right->u.varname, OPC_LOAD_INT64_VAR,
                         ll_arith_map, ll_cmp_map, bop, is_arith, is_cmp);
        return 1;
    }

    /* ========== long double 类型 ========== */
    if(arith_is_long_double_var(c, left) && arith_is_long_double_var(c, right)) {
        static const OpCode ld_arith_map[] = {
            [OP_ADD] = OPC_DOUBLE_ADD, [OP_SUB] = OPC_DOUBLE_SUB, [OP_MUL] = OPC_DOUBLE_MUL,
            [OP_DIV] = OPC_DOUBLE_DIV,
        };
        static const OpCode ld_cmp_map[] = {
            [OP_GT] = OPC_DOUBLE_GT, [OP_LT] = OPC_DOUBLE_LT, [OP_GE] = OPC_DOUBLE_GE,
            [OP_LE] = OPC_DOUBLE_LE, [OP_EQ] = OPC_DOUBLE_EQ, [OP_NE] = OPC_DOUBLE_NE,
        };
        emit_typed_arith(c, left->u.varname, right->u.varname, OPC_LOAD_DOUBLE_VAR,
                         ld_arith_map, ld_cmp_map, bop, is_arith, is_cmp);
        return 1;
    }

    /* ========== int8 类型 ========== */
    if(arith_is_int8_var(c, left) && arith_is_int8_var(c, right)) {
        static const OpCode i8_arith_map[] = {
            [OP_ADD] = OPC_INT64_ADD, [OP_SUB] = OPC_INT64_SUB, [OP_MUL] = OPC_INT64_MUL,
            [OP_DIV] = OPC_INT64_DIV, [OP_MOD] = OPC_INT64_MOD,
        };
        static const OpCode i8_cmp_map[] = {
            [OP_GT] = OPC_INT64_GT, [OP_LT] = OPC_INT64_LT, [OP_GE] = OPC_INT64_GE,
            [OP_LE] = OPC_INT64_LE, [OP_EQ] = OPC_INT64_EQ, [OP_NE] = OPC_INT64_NE,
        };
        emit_typed_arith(c, left->u.varname, right->u.varname, OPC_LOAD_INT64_VAR,
                         i8_arith_map, i8_cmp_map, bop, is_arith, is_cmp);
        return 1;
    }

    /* ========== int16 类型 ========== */
    if(arith_is_int16_var(c, left) && arith_is_int16_var(c, right)) {
        static const OpCode i16_arith_map[] = {
            [OP_ADD] = OPC_INT64_ADD, [OP_SUB] = OPC_INT64_SUB, [OP_MUL] = OPC_INT64_MUL,
            [OP_DIV] = OPC_INT64_DIV, [OP_MOD] = OPC_INT64_MOD,
        };
        static const OpCode i16_cmp_map[] = {
            [OP_GT] = OPC_INT64_GT, [OP_LT] = OPC_INT64_LT, [OP_GE] = OPC_INT64_GE,
            [OP_LE] = OPC_INT64_LE, [OP_EQ] = OPC_INT64_EQ, [OP_NE] = OPC_INT64_NE,
        };
        emit_typed_arith(c, left->u.varname, right->u.varname, OPC_LOAD_INT64_VAR,
                         i16_arith_map, i16_cmp_map, bop, is_arith, is_cmp);
        return 1;
    }

    /* short 类型不优化：short 栈与 int16 栈分离，专用算术指令从 int16 栈弹出，
       强行复用会导致栈类型不匹配。short 走通用路径（Value 栈），性能损失可忽略。 */

    /* ========== short 类型专用优化 ========== */
    if(arith_is_short_var(c, left) && arith_is_short_var(c, right)) {
        static const OpCode short_arith_map[] = {
            [OP_ADD] = OPC_INT64_ADD, [OP_SUB] = OPC_INT64_SUB, [OP_MUL] = OPC_INT64_MUL,
            [OP_DIV] = OPC_INT64_DIV, [OP_MOD] = OPC_INT64_MOD,
        };
        static const OpCode short_cmp_map[] = {
            [OP_GT] = OPC_INT64_GT, [OP_LT] = OPC_INT64_LT, [OP_GE] = OPC_INT64_GE,
            [OP_LE] = OPC_INT64_LE, [OP_EQ] = OPC_INT64_EQ, [OP_NE] = OPC_INT64_NE,
        };
        emit_typed_arith(c, left->u.varname, right->u.varname, OPC_LOAD_INT64_VAR,
                         short_arith_map, short_cmp_map, bop, is_arith, is_cmp);
        /* 算术运算结果在 short 栈，需要转换回 Value 栈 */
        if(is_arith) {
            emit(c, OPC_INT64_TO_VALUE, 0, 0);
        }
        return 1;
    }

    /* ========== int32 类型 ========== */
    if(arith_is_int32_var(c, left) && arith_is_int32_var(c, right)) {
        static const OpCode i32_arith_map[] = {
            [OP_ADD] = OPC_INT64_ADD, [OP_SUB] = OPC_INT64_SUB, [OP_MUL] = OPC_INT64_MUL,
            [OP_DIV] = OPC_INT64_DIV, [OP_MOD] = OPC_INT64_MOD,
        };
        static const OpCode i32_cmp_map[] = {
            [OP_GT] = OPC_INT64_GT, [OP_LT] = OPC_INT64_LT, [OP_GE] = OPC_INT64_GE,
            [OP_LE] = OPC_INT64_LE, [OP_EQ] = OPC_INT64_EQ, [OP_NE] = OPC_INT64_NE,
        };
        emit_typed_arith(c, left->u.varname, right->u.varname, OPC_LOAD_INT64_VAR,
                         i32_arith_map, i32_cmp_map, bop, is_arith, is_cmp);
        return 1;
    }

    /* ========== int64 类型 ========== */
    if(arith_is_int64_var(c, left) && arith_is_int64_var(c, right)) {
        static const OpCode i64_arith_map[] = {
            [OP_ADD] = OPC_INT64_ADD, [OP_SUB] = OPC_INT64_SUB, [OP_MUL] = OPC_INT64_MUL,
            [OP_DIV] = OPC_INT64_DIV, [OP_MOD] = OPC_INT64_MOD,
        };
        static const OpCode i64_cmp_map[] = {
            [OP_GT] = OPC_INT64_GT, [OP_LT] = OPC_INT64_LT, [OP_GE] = OPC_INT64_GE,
            [OP_LE] = OPC_INT64_LE, [OP_EQ] = OPC_INT64_EQ, [OP_NE] = OPC_INT64_NE,
        };
        emit_typed_arith(c, left->u.varname, right->u.varname, OPC_LOAD_INT64_VAR,
                         i64_arith_map, i64_cmp_map, bop, is_arith, is_cmp);
        return 1;
    }

    /* 未匹配到任何类型，返回 0 表示需要走通用路径 */
    return 0;
}

/* ========== 运算结果处理 ========== */

void arith_handle_assign_result(Ctx* c, AstNode* binop, int var_idx, ExprType result_type) {
    if(!c || !binop) return;

    /* 根据运算结果类型选择对应的专用存储指令 */
    switch(result_type) {
        /* 整数类型 */
        case EXPR_TYPE_BOOL:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_BOOL;
            break;
        case EXPR_TYPE_CHAR:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_CHAR;
            break;
        case EXPR_TYPE_INT8:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_INT8;
            break;
        case EXPR_TYPE_INT16:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_INT16;
            break;
        case EXPR_TYPE_SHORT:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_SHORT;
            break;
        case EXPR_TYPE_INT:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_INT;
            break;
        case EXPR_TYPE_INT64:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_INT64;
            break;
        case EXPR_TYPE_LONG_LONG:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_LONGLONG;
            break;
        case EXPR_TYPE_LONG:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_LONG;
            break;
        /* 无符号整数类型 */
        case EXPR_TYPE_BYTE:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_BYTE;
            break;
        case EXPR_TYPE_UINT8:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_UINT8;
            break;
        case EXPR_TYPE_UINT16:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_UINT16;
            break;
        case EXPR_TYPE_UINT:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_UINT32;
            break;
        case EXPR_TYPE_UINT64:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_UINT64;
            break;
        case EXPR_TYPE_ULONG:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_ULONG;
            break;
        case EXPR_TYPE_SIZE_T:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_SIZE_T;
            break;
        case EXPR_TYPE_SSIZE_T:
            c_expr(c, binop);
            emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_SSIZE_T;
            break;
        /* 浮点类型 */
        case EXPR_TYPE_FLOAT:
            c_expr(c, binop);
            emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_FLOAT;
            break;
        case EXPR_TYPE_DOUBLE:
            c_expr(c, binop);
            emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_DOUBLE;
            break;
        case EXPR_TYPE_LONG_DOUBLE:
            c_expr(c, binop);
            emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
            c->fn->var_type_tags[var_idx] = CAST_LONG_DOUBLE;
            break;
        default:
            /* EXPR_TYPE_NONE 或未知类型，不处理 */
            break;
    }
}

int arith_handle_print_result(Ctx* c, AstNode* single_arg, ExprType result_type) {
    if(!c || !single_arg) return 0;

    /* 根据运算结果类型选择对应的专用打印指令 */
    switch(result_type) {
        /* 整数类型 */
        case EXPR_TYPE_BOOL:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_CHAR:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_INT8:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_INT16:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_SHORT:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_INT:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_INT64:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_LONG_LONG:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_LONG:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        /* 无符号整数类型 */
        case EXPR_TYPE_BYTE:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_UINT8:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_UINT16:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_UINT:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_UINT64:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_ULONG:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_SIZE_T:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        case EXPR_TYPE_SSIZE_T:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64, 0, 0);
            return 1;
        /* 浮点类型 */
        case EXPR_TYPE_FLOAT:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_DOUBLE, 0, 0);
            return 1;
        case EXPR_TYPE_DOUBLE:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_DOUBLE, 0, 0);
            return 1;
        case EXPR_TYPE_LONG_DOUBLE:
            c_expr(c, single_arg);
            emit(c, OPC_PRINT_INT64_DOUBLE, 0, 0);
            return 1;
        default:
            /* EXPR_TYPE_NONE 或未知类型，返回 0 表示需要走通用路径 */
            return 0;
    }
}
