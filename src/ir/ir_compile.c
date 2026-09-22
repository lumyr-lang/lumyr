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
#include "lm_type.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* IR 编译致命错误标志：调用确定不存在的方法等情况设置。
 * 全部编译完成后由顶层（ir_compile_main 调用方）检查：有错则不运行 VM。
 * 此前仅打印错误继续生成，已压栈实参残留会导致运行时栈错位连锁污染。 */
static int g_ir_compile_error = 0;
int ir_compile_had_error(void) { return g_ir_compile_error; }

/* 前向声明 */
void c_stmt(Ctx* c, AstNode* node);
ExprType c_expr(Ctx* c, AstNode* node);
static const char* c_expr_type_name(Ctx* c, AstNode* node);
static CastKind c_expr_cast_type(Ctx* c, AstNode* node);
static void emit_to_dynamic(Ctx* c, ExprType from, CastKind ck);
static void c_expr_to_value(Ctx* c, AstNode* node);
AstNode* func_ast_lookup(const char* name);   /* AST 函数表（func_compile.c） */
static ExprType compile_user_call(Ctx* c, BytecodeFunc* callee, AstNode* def_ast, AstNode* args, int keep_result, AstNode* method_recv);
static BytecodeFunc* resolve_self_recursive(Ctx* c, const char* func_name, AstNode* tmp_def);
static void collect_call_args(AstNode* n, AstNode*** argv, int* argc, int* acap);
/* 自由函数重载解析：成功写 *fn_out/*def_out 返回1，失败（无匹配/歧义）返回0 */
static int resolve_free_call(Ctx* c, const char* name, AstNode* args,
                             BytecodeFunc** fn_out, AstNode** def_out);
static int ol_group_exists(const char* name);
static int g_ol_seq = 0;   /* 重载唯一键全局序号（定义在后，此处前置） */
static void emit_value_cast(Ctx* c, ExprType from, ExprType to);  /* typed → typed 栈转换 */
char* c_expr_owner_type(Ctx* c, AstNode* node);  /* 表达式持有的自定义类型名 */
static void record_var_owner(Ctx* c, int bf_idx, AstNode* rhs);  /* 记录变量槽属主类型 */
static ExprType compile_method_call_expr(Ctx* c, AstNode* node, int keep_result);  /* 方法调用编译 */

/* ============================================================
 * builtin_id_by_name：方法名/函数名 → BuiltinId（编译期静态表）
 * 仅收录已有运行时实现的内置；驼峰/下划线别名多对一映射。
 * 无运行时字符串查表：编译期一次解析为枚举编码进指令。
 * 返回 -1 表示不是内置。
 * ============================================================ */
static int builtin_id_by_name(const char* name) {
    static const struct { const char* n; BuiltinId id; } TBL[] = {
        /* 通用 */
        {"len", BUILTIN_LEN}, {"size", BUILTIN_LEN},
        {"type", BUILTIN_TYPE},
        {"range", BUILTIN_RANGE},
        {"format", BUILTIN_FORMAT},
        /* 字符串组 */
        {"substr", BUILTIN_SUBSTR}, {"substring", BUILTIN_SUBSTR},
        {"toupper", BUILTIN_TOUPPER}, {"toUpperCase", BUILTIN_TOUPPER}, {"upper", BUILTIN_TOUPPER},
        {"tolower", BUILTIN_TOLOWER}, {"toLowerCase", BUILTIN_TOLOWER}, {"lower", BUILTIN_TOLOWER},
        {"strip", BUILTIN_STRIP}, {"trim", BUILTIN_STRIP},
        {"repeat", BUILTIN_REPEAT},
        {"split", BUILTIN_SPLIT},
        {"replace", BUILTIN_REPLACE},
        {"startswith", BUILTIN_STARTSWITH}, {"startsWith", BUILTIN_STARTSWITH},
        {"endswith", BUILTIN_ENDSWITH}, {"endsWith", BUILTIN_ENDSWITH},
        {"char_at", BUILTIN_CHAR_AT}, {"charAt", BUILTIN_CHAR_AT},
        /* 加密/编码/正则（kit/runtime 现成封装） */
        {"md5", BUILTIN_MD5},
        {"encodeBase64", BUILTIN_ENCODE_BASE64}, {"base64_encode", BUILTIN_ENCODE_BASE64},
        {"decodeBase64", BUILTIN_DECODE_BASE64}, {"base64_decode", BUILTIN_DECODE_BASE64},
        {"encodeURL", BUILTIN_ENCODE_URL}, {"url_encode", BUILTIN_ENCODE_URL},
        {"decodeURL", BUILTIN_DECODE_URL}, {"url_decode", BUILTIN_DECODE_URL},
        {"regex_match", BUILTIN_REGEX_MATCH},
        {"regex_search", BUILTIN_REGEX_SEARCH},
        {"regex_replace", BUILTIN_REGEX_REPLACE},
        /* contains / 查找 */
        {"contains", BUILTIN_CONTAINS},
        {"indexOf", BUILTIN_ARRAY_INDEXOF}, {"index_of", BUILTIN_ARRAY_INDEXOF},
        {"objectIndex", BUILTIN_OBJECT_INDEX},
        /* 数组/字典修改类（原地 + 返回 self） */
        {"add", BUILTIN_ARRAY_ADD},
        {"insert", BUILTIN_INSERT},
        {"remove", BUILTIN_ARRAY_REMOVE},
        {"clear", BUILTIN_ARRAY_CLEAR},
        {"addAll", BUILTIN_ARRAY_ADDALL},
        {"set", BUILTIN_SET},
        {"get", BUILTIN_GET},
        {"first", BUILTIN_ARRAY_FIRST}, {"last", BUILTIN_ARRAY_LAST},
        /* 排序/聚合 */
        {"sort", BUILTIN_SORT},
        {"reverse", BUILTIN_REVERSE},
        {"sum", BUILTIN_SUM},
        {"avg", BUILTIN_AVG}, {"mean", BUILTIN_MEAN},
        {"std", BUILTIN_STD}, {"variance", BUILTIN_VARIANCE}, {"var", BUILTIN_VARIANCE},
        {"min", BUILTIN_MIN}, {"max", BUILTIN_MAX},
        {"argmax", BUILTIN_ARGMAX}, {"argmin", BUILTIN_ARGMIN},
        {"norm", BUILTIN_NORM},
        {"normalize", BUILTIN_NORMALIZE},
        {"softmax", BUILTIN_SOFTMAX},
        /* enum 增强 */
        {"fromValue", BUILTIN_FROM_VALUE}, {"from_value", BUILTIN_FROM_VALUE},
        {"flat", BUILTIN_ARRAY_FLAT},
        {"join", BUILTIN_JOIN},
        /* 高阶函数 */
        {"map", BUILTIN_MAP},
        {"filter", BUILTIN_FILTER},
        {"reduce", BUILTIN_REDUCE},
        /* 字典组 */
        {"keys", BUILTIN_KEYS},
        {"values", BUILTIN_VALUES},
        {"has", BUILTIN_MAP_HAS},
        /* AI / 线性代数 */
        {"shape", BUILTIN_SHAPE},
        {"reshape", BUILTIN_RESHAPE},
        {"slice", BUILTIN_SLICE},
        {"concat", BUILTIN_CONCAT},
        {"dot", BUILTIN_DOT},
        {"matmul", BUILTIN_MATMUL},
        {"transpose", BUILTIN_TRANSPOSE},
        {"det", BUILTIN_DET},
        {"inv", BUILTIN_INV},
        /* 数学（全局形式） */
        {"floor", BUILTIN_FLOOR}, {"ceil", BUILTIN_CEIL},
        {"abs", BUILTIN_ABS}, {"sqrt", BUILTIN_SQRT},
        {"del", BUILTIN_DEL},
        /* 生成器 */
        {"next", BUILTIN_NEXT}, {"send", BUILTIN_SEND},
        {"receive", BUILTIN_RECEIVE}, {"close", BUILTIN_CLOSE},
        {"__assert", BUILTIN_ASSERT},
        /* date 族构造（全局形式 date(...) / datetime(...) / time(...) / timedelta(...)） */
        {"date", BUILTIN_DATE_MAKE},
        {"datetime", BUILTIN_DATETIME_MAKE},
        {"time", BUILTIN_TIME_MAKE},
        {"timedelta", BUILTIN_TIMEDELTA_MAKE},
        {"now", BUILTIN_NOW},     /* now()：当前 UTC 时间（VAL_DATETIME） */
        {"today", BUILTIN_TODAY}, /* today()：当前 UTC 日期（VAL_DATE） */
        /* date 族字段访问（全局形式 year(d) / month(d) ...） */
        {"year", BUILTIN_YEAR}, {"month", BUILTIN_MONTH}, {"day", BUILTIN_DAY},
        {"hour", BUILTIN_HOUR}, {"minute", BUILTIN_MINUTE}, {"second", BUILTIN_SECOND},
        {"weekday", BUILTIN_WEEKDAY}, {"yearday", BUILTIN_YEARDAY},
        {"days", BUILTIN_DAYS}, {"seconds", BUILTIN_SECONDS}, {"totalSeconds", BUILTIN_TOTAL_SECONDS},
        {"format_date", BUILTIN_FORMAT_DATE},
        {"diff", BUILTIN_DATE_DIFF},
        /* tuple/bytes/complex 构造（全局形式） */
        {"tuple", BUILTIN_TUPLE_MAKE},
        {"bytes", BUILTIN_BYTES_MAKE},
        {"complex", BUILTIN_COMPLEX_MAKE},
        /* set 方法（非冲突名） */
        {"union", BUILTIN_SET_UNION},
        {"intersect", BUILTIN_SET_INTERSECT},
        /* bytes 方法 */
        {"hex", BUILTIN_BYTES_HEX},
        {"to_str", BUILTIN_BYTES_TO_STR},
        {"from_hex", BUILTIN_BYTES_FROM_HEX},
        /* complex 方法 */
        {"conjugate", BUILTIN_COMPLEX_CONJUGATE},
        /* calendar 综合日历 */
        {"calendar", BUILTIN_CALENDAR_MAKE},
        {"firstDate", BUILTIN_CALENDAR_FIRST_DATE},
        {"lastDate", BUILTIN_CALENDAR_LAST_DATE},
        {NULL, (BuiltinId)-1}
    };
    for(int i = 0; TBL[i].n; i++) {
        if(strcmp(TBL[i].n, name) == 0) return (int)TBL[i].id;
    }
    return -1;
}


/* ============================================================
 * 表达式编译
 * ============================================================ */

/* 查找变量索引，返回 -1 表示未找到 */
static int c_find_var(Ctx* c, const char* name) {
    return symhash_lookup(&c->var_idx, c->var_names, name);
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
    /* 登记 Ctx 变量哈希（var_names[idx] 已就绪） */
    symhash_insert(&c->var_idx, c->var_names, c->var_cnt, idx);
    /* 同步到 BytecodeFunc 符号表（供 arith_get_expr_type 使用） */
    int bf_idx = bf_sym(c->fn, name);
    c->fn->var_type_tags[bf_idx] = (int)type;
    return idx;
}

/* ============================================================
 * 三元表达式辅助
 * ============================================================ */

/* CastKind → 4 栈架构规范类型名。
 * 关键：所有整型（含 bool/char/byte/ascii/int64...）归一 "int"（INT64 栈），
 * 所有浮点归一 "double"（DOUBLE 栈）；不能用 castkind_to_name 的精确名
 * （"int64"/"bool"/"long long" ternary 无法识别，会误判为 PTR 导致跨栈错位）。
 * 返回：int/double/string/bigint/decimal/bitdecimal/ptr/unknown */
static const char* castkind_canonical_name(CastKind ck) {
    switch(ck) {
    case CAST_INT: case CAST_INT8: case CAST_INT16: case CAST_INT32: case CAST_INT64:
    case CAST_LONGLONG: case CAST_LONG: case CAST_SHORT: case CAST_USHORT:
    case CAST_BOOL: case CAST_CHAR: case CAST_UCHAR: case CAST_BYTE: case CAST_ASCII:
    case CAST_UINT8: case CAST_UINT16: case CAST_UINT32: case CAST_UINT:
    case CAST_UINT64: case CAST_ULONG: case CAST_SIZE_T: case CAST_SSIZE_T:
        return "int";
    case CAST_FLOAT: case CAST_DOUBLE: case CAST_LONG_DOUBLE:
        return "double";
    case CAST_STRING:     return "string";
    case CAST_BIGINT:     return "bigint";
    case CAST_BITDECIMAL: return "bitdecimal";
    case CAST_DECIMAL:    return "decimal";
    case CAST_PTR: case CAST_STRUCT_PTR: case CAST_CLASS_PTR:
        return "ptr";
    default:              return "unknown";
    }
}

/* 在两个分支类型名之间选统一目标，优先级与类型吸收规则一致：
 * string > bigint > bitdecimal > decimal > double > int（ptr 最低） */
static const char* ternary_target_name(const char* a, const char* b) {
    /* 任一分支为动态（unknown/null）→ 整体走 VALUE 栈（NONE），另一分支 box */
    if(strcmp(a, "unknown") == 0 || strcmp(a, "null") == 0 ||
       strcmp(b, "unknown") == 0 || strcmp(b, "null") == 0) return "unknown";
    static const char* order[] = {"string", "bigint", "bitdecimal", "decimal", "double", "int", "ptr"};
    for(int i = 0; i < 7; i++) {
        if(strcmp(a, order[i]) == 0 || strcmp(b, order[i]) == 0) return order[i];
    }
    return a;
}

/* 把栈顶值从 from 类型转换为 to 类型（仅 emit 本编译器已支持的转换指令） */
static void ternary_cast_to(Ctx* c, const char* from, const char* to) {
    if(strcmp(from, to) == 0) return;

    /* 目标动态 VALUE：typed → box（int/double/ptr 各有 BOX 指令） */
    if(strcmp(to, "unknown") == 0) {
        ExprType et = strcmp(from, "int") == 0 ? EXPR_TYPE_INT :
                      strcmp(from, "double") == 0 ? EXPR_TYPE_DOUBLE : EXPR_TYPE_PTR;
        CastKind ck = strcmp(from, "int") == 0 ? CAST_INT :
                      strcmp(from, "double") == 0 ? CAST_DOUBLE : CAST_NONE;
        emit_to_dynamic(c, et, ck);
        return;
    }

    if(strcmp(to, "double") == 0) {
        if(strcmp(from, "int") == 0) emit(c, OPC_INT64_TO_DOUBLE, 0, 0);
        /* 高精度 → 字符串 → double */
        else if(strcmp(from, "bigint") == 0) {
            emit(c, OPC_BIGINT_TO_STRING, 0, 0); emit(c, OPC_STR_TO_DOUBLE, 0, 0);
        }
        else if(strcmp(from, "decimal") == 0) {
            emit(c, OPC_DECIMAL_TO_STRING, 0, 0); emit(c, OPC_STR_TO_DOUBLE, 0, 0);
        }
        else if(strcmp(from, "bitdecimal") == 0) {
            emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0); emit(c, OPC_STR_TO_DOUBLE, 0, 0);
        }
    } else if(strcmp(to, "int") == 0) {
        if(strcmp(from, "double") == 0) emit(c, OPC_DOUBLE_TO_INT64, 0, 0);
        else if(strcmp(from, "ptr") == 0) emit(c, OPC_PTR_TO_INT64, 0, 0);
        else if(strcmp(from, "bigint") == 0) {
            emit(c, OPC_BIGINT_TO_STRING, 0, 0); emit(c, OPC_STR_TO_INT64, 0, 0);
        }
        else if(strcmp(from, "decimal") == 0) {
            emit(c, OPC_DECIMAL_TO_STRING, 0, 0); emit(c, OPC_STR_TO_INT64, 0, 0);
        }
        else if(strcmp(from, "bitdecimal") == 0) {
            emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0); emit(c, OPC_STR_TO_INT64, 0, 0);
        }
    } else if(strcmp(to, "string") == 0) {
        if(strcmp(from, "int") == 0) emit(c, OPC_INT64_TO_STRING, 0, 0);
        else if(strcmp(from, "double") == 0) emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
        else if(strcmp(from, "bigint") == 0) emit(c, OPC_BIGINT_TO_STRING, 0, 0);
        else if(strcmp(from, "decimal") == 0) emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
        else if(strcmp(from, "bitdecimal") == 0) emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0);
    } else if(strcmp(to, "bigint") == 0 || strcmp(to, "decimal") == 0 ||
              strcmp(to, "bitdecimal") == 0) {
        /* 统一经字符串（PTR 栈）走 *_FROM_STRING */
        if(strcmp(from, "int") == 0) emit(c, OPC_INT64_TO_STRING, 0, 0);
        else if(strcmp(from, "double") == 0) emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
        else if(strcmp(from, "bigint") == 0) emit(c, OPC_BIGINT_TO_STRING, 0, 0);
        else if(strcmp(from, "decimal") == 0) emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
        else if(strcmp(from, "bitdecimal") == 0) emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0);
        if(strcmp(to, "bigint") == 0) emit(c, OPC_BIGINT_FROM_STRING, 0, 0);
        else if(strcmp(to, "decimal") == 0) emit(c, OPC_DECIMAL_FROM_STRING, 0, 0);
        else emit(c, OPC_BITDECIMAL_FROM_STRING, 0, 0);
    } else if(strcmp(to, "ptr") == 0) {
        if(strcmp(from, "int") == 0) emit(c, OPC_INT64_TO_PTR, 0, 0);
    }
}

/* 高精度 PTR 栈对象（bigint/decimal/bitdecimal）归一为字符串形式（PTR→PTR）；
   string 已具字符串形式、裸 ptr 无数字语义，二者零操作。
   用于字符串解析指令（STR_TO_INT64/STR_TO_DOUBLE）之前。 */
static void emit_hiptr_to_string(Ctx* c, CastKind child_ct) {
    if(child_ct == CAST_BIGINT)
        emit(c, OPC_BIGINT_TO_STRING, 0, 0);
    else if(child_ct == CAST_DECIMAL)
        emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
    else if(child_ct == CAST_BITDECIMAL)
        emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0);
}

/* 类型名 → ExprType */
static ExprType ternary_name_to_exprtype(const char* n) {
    if(strcmp(n, "double") == 0) return EXPR_TYPE_DOUBLE;
    if(strcmp(n, "int") == 0) return EXPR_TYPE_INT;
    if(strcmp(n, "unknown") == 0) return EXPR_TYPE_NONE;  /* 动态 → VALUE 栈 */
    return EXPR_TYPE_PTR;  /* string/bigint/decimal/bitdecimal/ptr */
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
        /* super：等价于 self（同一实例指针，slot 0），返回 EXPR_TYPE_PTR */
        if(strcmp(name, "super") == 0 && c->fn->method_self_struct) {
            emit(c, OPC_LOAD_PTR_VAR, 0, 0);
            return EXPR_TYPE_PTR;
        }
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
        /* 匿名函数表达式（lambda/arrow）：有捕获则 MKCLOSURE，否则普通函数值 */
        const char* lname = node->u.func_def.name;
        int sym = c_add_var(c, lname, EXPR_TYPE_NONE);
        /* struct/class 方法体内的嵌套 lambda/arrow 在 parse 期未单独编译
         * （g_current_class/struct 非空时 yacc 动作跳过 compile_func_from_ast），
         * 此处补编译并注册，否则下方 GETFUNC/MKCLOSURE 引用的字节码在函数表中不存在。
         * 首版本按无捕获编译，typecheck 分析出捕获后会重编译覆盖。 */
        if(!ir_func_table_lookup(lname)) {
            RuntimeFunc* nrf = compile_func_from_ast(node);
            Value nfv;
            memset(&nfv, 0, sizeof(nfv));
            nfv.type = VAL_FUNC;
            nfv.v.func.func_obj = nrf;
            nfv.v.func.ffi_func = NULL;
            nfv.v.func.is_ffi = 0;
            sym_set(lname, nfv);
        }
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
        /* cast 目标是数组字面量（(T)[e1,e2]）：逐元素强转构造类型化数组。
         * 例外：CAST_ARRAY（及容器类 cast）语义为透传，不应构造 typed_array，
         * 否则 type 形状的 array 字段会被错误转为 typed_array */
        if(node->u.cast.child && node->u.cast.child->type == AST_ARRAY_LIT &&
           ct != CAST_ARRAY && ct != CAST_MAP && ct != CAST_TYPED_ARRAY &&
           ct != CAST_STRUCT_PTR && ct != CAST_CLASS_PTR) {
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
            } else if(child_type == EXPR_TYPE_NONE) {
                /* 动态 Value（VALUE 栈）→ 字符串（PTR 栈）出箱 */
                emit(c, OPC_CAST_STRING, 0, 0);
            } else if(child_type == EXPR_TYPE_PTR && child_ct == CAST_BIGINT) {
                emit(c, OPC_BIGINT_TO_STRING, 0, 0);
            } else if(child_type == EXPR_TYPE_PTR && child_ct == CAST_DECIMAL) {
                emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
            } else if(child_type == EXPR_TYPE_PTR && child_ct == CAST_BITDECIMAL) {
                emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0);
            }
            /* PTR(string) → string：透传，零转换 */
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
            /* ptr → int：真裸指针取地址；string/bigint/decimal/bitdecimal 经字符串解析 */
            else if(child_type == EXPR_TYPE_PTR) {
                if(child_ct == CAST_PTR) {
                    emit(c, OPC_PTR_TO_INT64, 0, 0);
                } else {
                    emit_hiptr_to_string(c, child_ct);
                    emit(c, OPC_STR_TO_INT64, 0, 0);
                }
            }
            /* 动态 Value（VALUE 栈）→ 出箱到 INT64 栈（UNBOX 内含字符串/动态转换兜底） */
            else if(child_type == EXPR_TYPE_NONE) {
                emit(c, OPC_UNBOX_INT64, 0, 0);
            }
            return EXPR_TYPE_INT;
        }
        /* 转成 double：根据源类型 emit 转换指令 */
        if(ct == CAST_DOUBLE || ct == CAST_FLOAT || ct == CAST_LONG_DOUBLE) {
            /* int → double */
            if(child_type == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_TO_DOUBLE, 0, 0);
            }
            /* ptr → double：真裸指针经 int64 中转；string/bigint/decimal/bitdecimal
               经字符串解析（此前缺此分支，值滞留 PTR 栈造成错位） */
            else if(child_type == EXPR_TYPE_PTR) {
                if(child_ct == CAST_PTR) {
                    emit(c, OPC_PTR_TO_INT64, 0, 0);
                    emit(c, OPC_INT64_TO_DOUBLE, 0, 0);
                } else {
                    emit_hiptr_to_string(c, child_ct);
                    emit(c, OPC_STR_TO_DOUBLE, 0, 0);
                }
            }
            /* 动态 Value（VALUE 栈）→ 出箱到 DOUBLE 栈 */
            else if(child_type == EXPR_TYPE_NONE) {
                emit(c, OPC_UNBOX_DOUBLE, 0, 0);
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
            } else if(child_type == EXPR_TYPE_NONE) {
                /* 动态 Value → 取裸指针出箱（对象/地址语义） */
                emit(c, OPC_UNBOX_PTR, 0, 0);
            }
            return EXPR_TYPE_PTR;
        }
        /* 转成 bigint/decimal/bitdecimal：统一经字符串（PTR 栈）走 *_FROM_STRING，
           与 AST_TYPE_ANNOTATION 分支语义一致；已是目标类型则透传 */
        if(ct == CAST_BIGINT || ct == CAST_DECIMAL || ct == CAST_BITDECIMAL) {
            if(child_type == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_TO_STRING, 0, 0);
            } else if(child_type == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
            } else if(child_type == EXPR_TYPE_NONE) {
                /* 动态 Value → 字符串出箱 */
                emit(c, OPC_CAST_STRING, 0, 0);
            } else if(child_type == EXPR_TYPE_PTR) {
                if(child_ct == ct) {
                    /* 已是目标类型：透传，零转换 */
                    return EXPR_TYPE_PTR;
                }
                if(child_ct == CAST_BIGINT) {
                    emit(c, OPC_BIGINT_TO_STRING, 0, 0);
                } else if(child_ct == CAST_DECIMAL) {
                    emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
                } else if(child_ct == CAST_BITDECIMAL) {
                    emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0);
                }
                /* PTR(string)：保持字符串形式，直接 FROM_STRING */
            }
            if(ct == CAST_BIGINT) emit(c, OPC_BIGINT_FROM_STRING, 0, 0);
            else if(ct == CAST_DECIMAL) emit(c, OPC_DECIMAL_FROM_STRING, 0, 0);
            else emit(c, OPC_BITDECIMAL_FROM_STRING, 0, 0);
            return EXPR_TYPE_PTR;
        }
        return child_type;
    }

    case AST_TYPE_ANNOTATION: {
        /* 类型标注 <type>expr：编译子表达式，标记类型 */
        CastKind ct = node->u.type_annotation.cast_type;
        AstNode* ann_child = node->u.type_annotation.expr;
        /* 标注目标是数组字面量（<T>[e1,e2]）：逐元素转型构造类型化数组。
         * CAST_ARRAY/容器类标注语义为透传，不应构造 typed_array */
        if(ann_child && ann_child->type == AST_ARRAY_LIT &&
           ct != CAST_ARRAY && ct != CAST_MAP && ct != CAST_TYPED_ARRAY &&
           ct != CAST_STRUCT_PTR && ct != CAST_CLASS_PTR) {
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
            /* ptr → int：真裸指针取地址；string/bigint/decimal/bitdecimal 经字符串解析 */
            else if(child_type == EXPR_TYPE_PTR) {
                if(c_expr_cast_type(c, ann_child) == CAST_PTR) {
                    emit(c, OPC_PTR_TO_INT64, 0, 0);
                } else {
                    emit_hiptr_to_string(c, c_expr_cast_type(c, ann_child));
                    emit(c, OPC_STR_TO_INT64, 0, 0);
                }
            }
            /* 动态 Value（VALUE 栈）→ 出箱到 INT64 栈，防止 STORE 弹空栈 */
            else if(child_type == EXPR_TYPE_NONE) {
                emit(c, OPC_UNBOX_INT64, 0, 0);
            }
            return EXPR_TYPE_INT;
        } else if(ct == CAST_PTR) {
            /* <ptr>expr：整数地址 → 裸指针，压 PTR 栈 */
            if(child_type == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_TO_PTR, 0, 0);
            } else if(child_type == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_TO_INT64, 0, 0);
                emit(c, OPC_INT64_TO_PTR, 0, 0);
            } else if(child_type == EXPR_TYPE_NONE) {
                /* 动态 Value → 取裸指针出箱 */
                emit(c, OPC_UNBOX_PTR, 0, 0);
            }
            return EXPR_TYPE_PTR;
        } else if(ct == CAST_DOUBLE || ct == CAST_FLOAT || ct == CAST_LONG_DOUBLE) {
            /* 如果子表达式是 INT，需要转成 DOUBLE */
            if(child_type == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_TO_DOUBLE, 0, 0);
            }
            /* ptr → double：真裸指针经 int64 中转；string/高精度经字符串解析 */
            else if(child_type == EXPR_TYPE_PTR) {
                CastKind cct = c_expr_cast_type(c, ann_child);
                if(cct == CAST_PTR) {
                    emit(c, OPC_PTR_TO_INT64, 0, 0);
                    emit(c, OPC_INT64_TO_DOUBLE, 0, 0);
                } else {
                    emit_hiptr_to_string(c, cct);
                    emit(c, OPC_STR_TO_DOUBLE, 0, 0);
                }
            }
            /* 动态 Value（VALUE 栈）→ 出箱到 DOUBLE 栈 */
            else if(child_type == EXPR_TYPE_NONE) {
                emit(c, OPC_UNBOX_DOUBLE, 0, 0);
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
                CastKind child_ct = c_expr_cast_type(c, child);
                if(child_type == EXPR_TYPE_PTR && child_ct == CAST_BIGINT) {
                    /* 已是 bigint：透传，零转换（跳过 FROM_STRING） */
                    return EXPR_TYPE_PTR;
                }
                if(child_type == EXPR_TYPE_INT) {
                    emit(c, OPC_INT64_TO_STRING, 0, 0);
                } else if(child_type == EXPR_TYPE_DOUBLE) {
                    emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
                } else if(child_type == EXPR_TYPE_NONE) {
                    /* 动态 Value（VALUE 栈）→ 字符串（PTR 栈）出箱，防止 FROM_STRING 弹空栈 */
                    emit(c, OPC_CAST_STRING, 0, 0);
                } else if(child_type == EXPR_TYPE_PTR && child_ct == CAST_DECIMAL) {
                    emit(c, OPC_DECIMAL_TO_STRING, 0, 0);
                } else if(child_type == EXPR_TYPE_PTR && child_ct == CAST_BITDECIMAL) {
                    emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0);
                }
                /* PTR(string)：保持字符串形式，直接 FROM_STRING */
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
                CastKind child_ct = c_expr_cast_type(c, child);
                if(child_type == EXPR_TYPE_PTR && child_ct == CAST_DECIMAL) {
                    /* 已是 decimal：透传，零转换（跳过 FROM_STRING） */
                    return EXPR_TYPE_PTR;
                }
                if(child_type == EXPR_TYPE_DOUBLE) {
                    emit(c, OPC_DOUBLE_TO_STRING, 0, 0);
                } else if(child_type == EXPR_TYPE_INT) {
                    emit(c, OPC_INT64_TO_STRING, 0, 0);
                } else if(child_type == EXPR_TYPE_NONE) {
                    /* 动态 Value（VALUE 栈）→ 字符串（PTR 栈）出箱，防止 FROM_STRING 弹空栈 */
                    emit(c, OPC_CAST_STRING, 0, 0);
                } else if(child_type == EXPR_TYPE_PTR && child_ct == CAST_BIGINT) {
                    emit(c, OPC_BIGINT_TO_STRING, 0, 0);
                } else if(child_type == EXPR_TYPE_PTR && child_ct == CAST_BITDECIMAL) {
                    emit(c, OPC_BITDECIMAL_TO_STRING, 0, 0);
                }
                /* PTR(string)：保持字符串形式，直接 FROM_STRING */
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
                } else if(child_type == EXPR_TYPE_NONE) {
                    /* 动态 Value（VALUE 栈）→ 字符串（PTR 栈）出箱，防止 FROM_STRING 弹空栈 */
                    emit(c, OPC_CAST_STRING, 0, 0);
                } else if(child_type == EXPR_TYPE_PTR && child_ct == CAST_BITDECIMAL) {
                    /* 已是 bitdecimal：透传，零转换 */
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
            } else if(child_type == EXPR_TYPE_NONE) {
                /* 动态 Value（VALUE 栈）→ 字符串（PTR 栈）出箱，防止 STORE 弹空栈 */
                emit(c, OPC_CAST_STRING, 0, 0);
            }
            return EXPR_TYPE_PTR;
        }
        return child_type;
    }

    case AST_CALL: {
        /* 函数调用：用户函数优先（允许同名覆盖内置），未命中走内置静态表，
         * 最后兜底函数值变量的动态调用 */
        const char* func_name = node->u.call.name;
        AstNode* args = node->u.call.args;

        /* 重载名：存在重载组 → 按实参解析唯一版本（失败/歧义致命） */
        if(ol_group_exists(func_name)) {
            BytecodeFunc* callee = NULL; AstNode* def_ast = NULL;
            if(!resolve_free_call(c, func_name, args, &callee, &def_ast))
                exit(EXIT_FAILURE);
            return compile_user_call(c, callee, def_ast, args, 1, NULL);
        }

        /* 用户自定义函数：查函数表 + AST 表 */
        {
            BytecodeFunc* callee = ir_func_table_lookup(func_name);
            AstNode* def_ast = func_ast_lookup(func_name);
            /* 方法内裸名自递归：扁平名查不到时匹配当前方法自身 */
            AstNode self_tmp;
            BytecodeFunc* self_fn = resolve_self_recursive(c, func_name, &self_tmp);
            if(!callee && self_fn) {
                return compile_user_call(c, self_fn, &self_tmp, args, 1, NULL);
            }
            if(callee && def_ast) {
                return compile_user_call(c, callee, def_ast, args, 1, NULL);
            }
        }

        /* 内置函数：编译期静态表解析为 BuiltinId（无运行时字符串查表），
         * 实参全部转 VALUE，OPC_BUILTIN 弹 argc 个（栈顶为最后一个）压返回值。
         * 注意：若 func_name 同时是当前作用域内的局部变量（例如 add = (a,b)=>...;
         *       用户覆盖了内置名 add），必须优先走动态调用，否则会被误派发到
         *       BUILTIN_ARRAY_ADD 等方法式内置上，导致运行时报错 */
        int bid = builtin_id_by_name(func_name);
        int is_local_var = (c_find_var(c, func_name) >= 0);
        if(bid >= 0 && !is_local_var) {
            int argc = 0, acap = 0;
            AstNode** argv = NULL;
            collect_call_args(args, &argv, &argc, &acap);
            for(int i = 0; i < argc; i++) {
                c_expr_to_value(c, argv[i]);
            }
            free(argv);
            emit(c, OPC_BUILTIN, bid, argc);
            return EXPR_TYPE_NONE;
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

    case AST_INDEX: {
        /* self.field 快路径：arr 是 AST_VAR "self"，且当前 fn 是 struct/class 方法，
         * idx 是字符串字面量且能在 method_self_struct 类型中找到字段 → OPC_LOAD_FIELD
         * 走 typed 栈路由（INT64/DOUBLE/PTR），不做 Value 装箱 */
        if(node->u.index.arr->type == AST_VAR &&
           (strcmp(node->u.index.arr->u.varname, "self") == 0 ||
            strcmp(node->u.index.arr->u.varname, "super") == 0) &&
           c->fn->method_self_struct &&
           node->u.index.idx->type == AST_STRING) {
            const char* field_name = node->u.index.idx->u.sval;
            /* method_self_struct 可能带 "class:" 前缀（class_register 设置） */
            const char* tname = c->fn->method_self_struct;
            if(strncmp(tname, "class:", 6) == 0) tname += 6;
            TypeDef* td = struct_lookup(tname);
            if(!td) td = class_lookup(tname);
            if(td && td->runtime_info) {
                FieldInfo* fi = lumyr_type_find_field(td->runtime_info, field_name);
                if(fi) {
                    int fi_idx = (int)(fi - td->runtime_info->fields);
                    int cls = lumyr_etype_stackcls(fi->valtype);
                    /* 编译 self 到 PTR 栈（self 是指针，LOAD_PTR_VAR 直接加载） */
                    ExprType arr_et = c_expr(c, node->u.index.arr);
                    if(arr_et != EXPR_TYPE_PTR) {
                        /* 兜底：非 PTR 则装箱到 VALUE，再 BOX_PTR 到 PTR 栈 */
                        emit_to_dynamic(c, arr_et, c_expr_cast_type(c, node->u.index.arr));
                        emit(c, OPC_BOX_PTR, (int)CAST_PTR, 0);
                    }
                    emit(c, OPC_LOAD_FIELD, cls, fi_idx);
                    return (cls == 1) ? EXPR_TYPE_INT :
                           (cls == 2) ? EXPR_TYPE_DOUBLE : EXPR_TYPE_PTR;
                }
            }
        }
        /* 属性形式 x.len：idx 是字符串字面量 "len" 且 receiver 非用户自定义类型
         * → 内置 BUILTIN_LEN（数组/字典/类型化数组/字符串通用，运行时零转换）。
         * 用户类型的 len 字段访问仍走下方动态 INDEX_GET */
        if(node->u.index.idx->type == AST_STRING &&
           strcmp(node->u.index.idx->u.sval, "len") == 0) {
            char* ot = c_expr_owner_type(c, node->u.index.arr);
            int is_user = ot && type_lookup(ot);
            free(ot);
            if(!is_user) {
                c_expr_to_value(c, node->u.index.arr);
                emit(c, OPC_CALL_BUILTIN_METHOD, BUILTIN_LEN, 0);
                return EXPR_TYPE_NONE;
            }
        }
        /* 默认：动态 INDEX_GET（VAL_STRUCT_PTR 在 vm_exec_index_get 中走 lumyr_field_get） */
        c_expr_to_value(c, node->u.index.arr);
        c_expr_to_value(c, node->u.index.idx);
        emit(c, OPC_INDEX_GET, 0, 0);
        return EXPR_TYPE_NONE;
    }

    case AST_CLASS_NEW: {
        /* class/struct 实例构造：TypeName(args)
         * struct 无 __init__：实参压 VALUE 栈 → CLASS_NEW 弹参直接初始化字段
         * class 有 __init__：CLASS_NEW(argc=0) → BOX_PTR(self 入 VALUE) → 编译实参 → CALL __init__ */
        const char* tname = node->u.class_new.class_name;
        TypeDef* td = struct_lookup(tname);
        if(!td) td = class_lookup(tname);
        if(!td || !td->runtime_info) {
            /* 未注册类型：回退到普通函数调用（兼容旧语义） */
            AstNode** argv = NULL;
            int argc = 0, acap = 0;
            collect_call_args(node->u.class_new.args, &argv, &argc, &acap);
            for(int i = 0; i < argc; i++) c_expr_to_value(c, argv[i]);
            free(argv);
            AstNode callee_var;
            memset(&callee_var, 0, sizeof(callee_var));
            callee_var.type = AST_VAR;
            callee_var.u.varname = (char*)tname;
            c_expr(c, &callee_var);
            emit(c, OPC_CALLV, 0, argc);
            return EXPR_TYPE_NONE;
        }
        /* 计算实参个数 */
        AstNode** argv = NULL;
        int argc = 0, acap = 0;
        collect_call_args(node->u.class_new.args, &argv, &argc, &acap);
        free(argv);

        /* 判断是否有 __init__ 构造函数（class 有 constructor 节点；沿继承链查找父类构造函数） */
        const char* ctor_owner = NULL;
        if(td->is_class) {
            TypeDef* td2 = td;
            while(td2) {
                if(td2->constructor) { ctor_owner = td2->name; break; }
                if(!td2->parent) break;
                td2 = class_lookup(td2->parent);
            }
        }
        int has_init = (ctor_owner != NULL);

        /* RuntimeTypeInfo* 存入常量池 */
        int cp_idx = bf_add_u64_const(c->fn, (uint64_t)(uintptr_t)td->runtime_info);

        if(has_init) {
            /* class 有 __init__：CLASS_NEW 创建实例 → 存临时变量 → 编译实参 + 加载 self → CALL → POP ret → 加载临时变量
             * CALL 会消耗 self 和实参，所以先存实例到临时变量，调用后恢复 */
            emit(c, OPC_CLASS_NEW, cp_idx, 0);
            /* 存实例到临时变量（编译期生成唯一名） */
            static int ctor_tmp_seq = 0;
            char tmp_name[64];
            snprintf(tmp_name, sizeof(tmp_name), "__ctor_tmp_%d", ctor_tmp_seq++);
            int tmp_idx = c_add_var(c, tmp_name, EXPR_TYPE_PTR);
            int tmp_bf = bf_sym(c->fn, tmp_name);
            c->fn->var_type_tags[tmp_bf] = (int)CAST_CLASS_PTR;
            emit(c, OPC_STORE_PTR_VAR, tmp_idx, 0);
            /* 构造函数名：<ctor_owner>___init__（ctor_owner 可能是本类或父类） */
            char ctor_name[256];
            snprintf(ctor_name, sizeof(ctor_name), "%s___init__", ctor_owner);
            BytecodeFunc* ctor_fn = ir_func_table_lookup(ctor_name);
            AstNode* ctor_ast = func_ast_lookup(ctor_name);
            if(ctor_fn && ctor_ast) {
                /* 加载 self（临时变量）到 PTR 栈 */
                emit(c, OPC_LOAD_PTR_VAR, tmp_idx, 0);
                /* 按形参类型编译实参（slot 0 = self，已加载；slot 1+ = 用户实参） */
                AstNode** argv2 = NULL;
                int argc2 = 0, acap2 = 0;
                collect_call_args(node->u.class_new.args, &argv2, &argc2, &acap2);
                int slot = 1;
                AstNode* p = ctor_ast->u.func_def.params;
                if(p && p->u.param.name && strcmp(p->u.param.name, "self") == 0) {
                    p = p->u.param.next;
                }
                int ai = 0;
                while(p && ai < argc2) {
                    if(p->u.param.is_ellipsis) break;
                    CastKind pck = (slot < ctor_fn->sym_cnt) ? (CastKind)ctor_fn->var_type_tags[slot] : CAST_NONE;
                    ExprType param_et = castkind_to_exprtype(pck);
                    if(param_et != EXPR_TYPE_NONE) {
                        ExprType at = c_expr(c, argv2[ai]);
                        emit_value_cast(c, at, param_et);
                    } else {
                        c_expr_to_value(c, argv2[ai]);
                    }
                    slot++;
                    ai++;
                    p = p->u.param.next;
                }
                free(argv2);
                /* emit CALL（ctor 为 void：keep_result=0，CALL 不压返回值） */
                int total = 1 + argc2;
                int cs = bf_add_callsite(c->fn, ctor_name, total, 0, (int)EXPR_TYPE_NONE);
                emit(c, OPC_CALL, cs, total);
            } else {
                fprintf(stderr, "IR: 警告 - class %s 的 __init__ 未注册\n", tname);
            }
            /* 加载临时变量（实例）到 PTR 栈作为表达式结果 */
            emit(c, OPC_LOAD_PTR_VAR, tmp_idx, 0);
            return EXPR_TYPE_PTR;
        } else {
            /* struct 或 class 无 __init__：实参压 VALUE 栈，CLASS_NEW 弹参直接初始化 */
            AstNode** argv2 = NULL;
            int argc2 = 0, acap2 = 0;
            collect_call_args(node->u.class_new.args, &argv2, &argc2, &acap2);
            for(int i = 0; i < argc2; i++) c_expr_to_value(c, argv2[i]);
            free(argv2);
            emit(c, OPC_CLASS_NEW, cp_idx, argc2);
            return EXPR_TYPE_PTR;
        }
    }

    case AST_METHOD_CALL:
        return compile_method_call_expr(c, node, 1);

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

    /* 列表推导式 [expr for x in iter (if cond)]
     * 编译为：创建空数组 → 遍历 iter → 条件过滤 → 元素入数组 → 返回数组 */
    case AST_COMP_LIST: {
        /* 1. 创建空数组并存到临时变量 */
        emit(c, OPC_ARRAY_LIT, 0, 0);
        static int comp_seq = 0;
        char res_name[64], idx_name[64], len_name[64], obj_name[64];
        snprintf(res_name, sizeof(res_name), "__comp_res_%d", comp_seq);
        snprintf(idx_name, sizeof(idx_name), "__comp_i_%d", comp_seq);
        snprintf(len_name, sizeof(len_name), "__comp_n_%d", comp_seq);
        snprintf(obj_name, sizeof(obj_name), "__comp_o_%d", comp_seq);
        comp_seq++;
        int res_slot = c_add_var(c, res_name, EXPR_TYPE_NONE);
        int res_bf = bf_sym(c->fn, res_name);
        emit(c, OPC_STORE_VAR, res_slot, 0);

        /* 2. 编译 iter 并取 len */
        c_expr_to_value(c, node->u.comp.iter);
        int obj_slot = c_add_var(c, obj_name, EXPR_TYPE_NONE);
        int obj_bf = bf_sym(c->fn, obj_name);
        emit(c, OPC_STORE_VAR, obj_slot, 0);
        /* len(obj) */
        emit(c, OPC_LOAD_VAR, obj_slot, 0);
        emit(c, OPC_BUILTIN, BUILTIN_LEN, 1);
        emit(c, OPC_UNBOX_INT64, 0, 0);  /* VALUE → INT64 */
        int len_slot = c_add_var(c, len_name, EXPR_TYPE_INT);
        int len_bf = bf_sym(c->fn, len_name);
        c->fn->var_type_tags[len_bf] = (int)CAST_INT;
        emit(c, OPC_STORE_INT64_VAR, len_slot, 0);

        /* 3. i = 0 */
        int idx_slot = c_add_var(c, idx_name, EXPR_TYPE_INT);
        int idx_bf = bf_sym(c->fn, idx_name);
        c->fn->var_type_tags[idx_bf] = (int)CAST_INT;
        emit(c, OPC_PUSH_INT64_CONST, 0, 0);
        emit(c, OPC_STORE_INT64_VAR, idx_slot, 0);

        /* 4. 循环 */
        int loop_start = c->fn->code_len;
        /* if i >= len: break */
        emit(c, OPC_LOAD_INT64_VAR, idx_slot, 0);
        emit(c, OPC_LOAD_INT64_VAR, len_slot, 0);
        emit(c, OPC_INT64_GE, 0, 0);
        int jmp_end = c->fn->code_len;
        emit(c, OPC_JMP_IF_TRUE, 0, 0);  /* i>=len → 跳到 end */

        /* 5. x = obj[i] */
        emit(c, OPC_LOAD_VAR, obj_slot, 0);
        emit(c, OPC_LOAD_INT64_VAR, idx_slot, 0);
        emit(c, OPC_BOX_INT64, (int)CAST_NONE, 0);  /* INT64 → VALUE */
        emit(c, OPC_BUILTIN, BUILTIN_GET, 2);  /* get(obj, idx) → VALUE */
        /* 绑定循环变量 */
        const char* varname = node->u.comp.var->u.varname;
        int var_slot = c_add_var(c, (char*)varname, EXPR_TYPE_NONE);
        int var_bf = bf_sym(c->fn, (char*)varname);
        emit(c, OPC_STORE_VAR, var_slot, 0);

        /* 6. 条件过滤 */
        int cont_target;
        if(node->u.comp.cond) {
            c_expr_to_value(c, node->u.comp.cond);
            int jmp_skip = c->fn->code_len;
            emit(c, OPC_JMP_IF_FALSE_V, 0, 0);  /* false → 跳过 add */
            cont_target = c->fn->code_len;
            c->fn->code[jmp_skip].a = -1;  /* patch later to continue */
        } else {
            cont_target = c->fn->code_len;
        }

        /* 7. res.add(expr) */
        emit(c, OPC_LOAD_VAR, res_slot, 0);
        c_expr_to_value(c, node->u.comp.expr);
        emit(c, OPC_CALL_BUILTIN_METHOD, BUILTIN_ARRAY_ADD, 1);
        emit(c, OPC_POP, 0, 0);  /* discard add() return */

        /* 8. i++ */
        int continue_target = c->fn->code_len;
        if(node->u.comp.cond) {
            /* patch skip target to here */
            for(int pi = cont_target - 1; pi >= 0; pi--) {
                if(c->fn->code[pi].op == OPC_JMP_IF_FALSE_V && c->fn->code[pi].a == -1) {
                    c->fn->code[pi].a = continue_target;
                    break;
                }
            }
        }
        emit(c, OPC_LOAD_INT64_VAR, idx_slot, 0);
        emit(c, OPC_PUSH_INT64_CONST, 1, 0);
        emit(c, OPC_INT64_ADD, 0, 0);
        emit(c, OPC_STORE_INT64_VAR, idx_slot, 0);
        emit(c, OPC_JMP, loop_start, 0);

        /* 9. end: load result */
        int end_target = c->fn->code_len;
        c->fn->code[jmp_end].a = end_target;
        emit(c, OPC_LOAD_VAR, res_slot, 0);
        return EXPR_TYPE_NONE;
    }

    /* 字典推导式 {k:v for x in iter (if cond)} */
    case AST_COMP_MAP: {
        /* 1. 创建空 map */
        emit(c, OPC_MAP_LIT, 0, 0);
        static int mcomp_seq = 0;
        char res_name[64], idx_name[64], len_name[64], obj_name[64];
        snprintf(res_name, sizeof(res_name), "__mcomp_r_%d", mcomp_seq);
        snprintf(idx_name, sizeof(idx_name), "__mcomp_i_%d", mcomp_seq);
        snprintf(len_name, sizeof(len_name), "__mcomp_n_%d", mcomp_seq);
        snprintf(obj_name, sizeof(obj_name), "__mcomp_o_%d", mcomp_seq);
        mcomp_seq++;
        int res_slot = c_add_var(c, res_name, EXPR_TYPE_NONE);
        emit(c, OPC_STORE_VAR, res_slot, 0);

        /* 2. 编译 iter 并取 len */
        c_expr_to_value(c, node->u.comp.iter);
        int obj_slot = c_add_var(c, obj_name, EXPR_TYPE_NONE);
        emit(c, OPC_STORE_VAR, obj_slot, 0);
        emit(c, OPC_LOAD_VAR, obj_slot, 0);
        emit(c, OPC_BUILTIN, BUILTIN_LEN, 1);
        emit(c, OPC_UNBOX_INT64, 0, 0);  /* VALUE → INT64 */
        int len_slot = c_add_var(c, len_name, EXPR_TYPE_INT);
        int len_bf = bf_sym(c->fn, len_name);
        c->fn->var_type_tags[len_bf] = (int)CAST_INT;
        emit(c, OPC_STORE_INT64_VAR, len_slot, 0);

        /* 3. i = 0 */
        int idx_slot = c_add_var(c, idx_name, EXPR_TYPE_INT);
        int idx_bf = bf_sym(c->fn, idx_name);
        c->fn->var_type_tags[idx_bf] = (int)CAST_INT;
        emit(c, OPC_PUSH_INT64_CONST, 0, 0);
        emit(c, OPC_STORE_INT64_VAR, idx_slot, 0);

        /* 4. 循环 */
        int loop_start = c->fn->code_len;
        emit(c, OPC_LOAD_INT64_VAR, idx_slot, 0);
        emit(c, OPC_LOAD_INT64_VAR, len_slot, 0);
        emit(c, OPC_INT64_GE, 0, 0);
        int jmp_end = c->fn->code_len;
        emit(c, OPC_JMP_IF_TRUE, 0, 0);

        /* 5. x = obj[i] */
        emit(c, OPC_LOAD_VAR, obj_slot, 0);
        emit(c, OPC_LOAD_INT64_VAR, idx_slot, 0);
        emit(c, OPC_BOX_INT64, (int)CAST_NONE, 0);
        emit(c, OPC_BUILTIN, BUILTIN_GET, 2);
        const char* varname = node->u.comp.var->u.varname;
        int var_slot = c_add_var(c, (char*)varname, EXPR_TYPE_NONE);
        emit(c, OPC_STORE_VAR, var_slot, 0);

        /* 6. 条件过滤 */
        int cont_target;
        if(node->u.comp.cond) {
            c_expr_to_value(c, node->u.comp.cond);
            int jmp_skip = c->fn->code_len;
            emit(c, OPC_JMP_IF_FALSE_V, 0, 0);
            cont_target = c->fn->code_len;
            c->fn->code[jmp_skip].a = -1;
        } else {
            cont_target = c->fn->code_len;
        }

        /* 7. res[k] = v → set(res, k, v) */
        emit(c, OPC_LOAD_VAR, res_slot, 0);
        c_expr_to_value(c, node->u.comp.expr);   /* key */
        c_expr_to_value(c, node->u.comp.value);   /* value */
        emit(c, OPC_CALL_BUILTIN_METHOD, BUILTIN_SET, 2);
        emit(c, OPC_POP, 0, 0);

        /* 8. i++ */
        int continue_target = c->fn->code_len;
        if(node->u.comp.cond) {
            for(int pi = cont_target - 1; pi >= 0; pi--) {
                if(c->fn->code[pi].op == OPC_JMP_IF_FALSE_V && c->fn->code[pi].a == -1) {
                    c->fn->code[pi].a = continue_target;
                    break;
                }
            }
        }
        emit(c, OPC_LOAD_INT64_VAR, idx_slot, 0);
        emit(c, OPC_PUSH_INT64_CONST, 1, 0);
        emit(c, OPC_INT64_ADD, 0, 0);
        emit(c, OPC_STORE_INT64_VAR, idx_slot, 0);
        emit(c, OPC_JMP, loop_start, 0);

        /* 9. end */
        int end_target = c->fn->code_len;
        c->fn->code[jmp_end].a = end_target;
        emit(c, OPC_LOAD_VAR, res_slot, 0);
        return EXPR_TYPE_NONE;
    }

    case AST_INDEX_ASSIGN: {
        /* self.field = val 快路径：arr 是 self，idx 是字符串字面量，且当前 fn 是方法
         * → OPC_STORE_FIELD（typed 栈路由） */
        if(node->u.index_assign.arr->type == AST_VAR &&
           (strcmp(node->u.index_assign.arr->u.varname, "self") == 0 ||
            strcmp(node->u.index_assign.arr->u.varname, "super") == 0) &&
           c->fn->method_self_struct &&
           node->u.index_assign.idx->type == AST_STRING) {
            const char* field_name = node->u.index_assign.idx->u.sval;
            /* method_self_struct 可能带 "class:" 前缀（class_register 设置） */
            const char* tname = c->fn->method_self_struct;
            if(strncmp(tname, "class:", 6) == 0) tname += 6;
            TypeDef* td = struct_lookup(tname);
            if(!td) td = class_lookup(tname);
            if(td && td->runtime_info) {
                FieldInfo* fi = lumyr_type_find_field(td->runtime_info, field_name);
                if(fi) {
                    int fi_idx = (int)(fi - td->runtime_info->fields);
                    int cls = lumyr_etype_stackcls(fi->valtype);
                    /* 编译 val 到对应 typed 栈，不匹配则走动态路径 */
                    ExprType val_et = c_expr(c, node->u.index_assign.value);
                    int matched = 0;
                    if(cls == 1) {
                        if(val_et == EXPR_TYPE_INT) matched = 1;
                        else if(val_et == EXPR_TYPE_DOUBLE) { emit(c, OPC_DOUBLE_TO_INT64, 0, 0); matched = 1; }
                        else if(val_et == EXPR_TYPE_PTR) { emit(c, OPC_PTR_TO_INT64, 0, 0); matched = 1; }
                        else if(val_et == EXPR_TYPE_NONE) { emit(c, OPC_UNBOX_INT64, 0, 0); matched = 1; }
                    } else if(cls == 2) {
                        if(val_et == EXPR_TYPE_DOUBLE) matched = 1;
                        else if(val_et == EXPR_TYPE_INT) { emit(c, OPC_INT64_TO_DOUBLE, 0, 0); matched = 1; }
                        else if(val_et == EXPR_TYPE_NONE) { emit(c, OPC_UNBOX_DOUBLE, 0, 0); matched = 1; }
                    } else {
                        if(val_et == EXPR_TYPE_PTR) matched = 1;
                        else if(val_et == EXPR_TYPE_NONE) { emit(c, OPC_UNBOX_PTR, 0, 0); matched = 1; }
                        else if(val_et == EXPR_TYPE_INT) { emit(c, OPC_INT64_TO_PTR, 0, 0); matched = 1; }
                    }
                    if(matched) {
                        /* 编译 self 到 PTR 栈 */
                        ExprType arr_et = c_expr(c, node->u.index_assign.arr);
                        if(arr_et != EXPR_TYPE_PTR) {
                            emit_to_dynamic(c, arr_et, c_expr_cast_type(c, node->u.index_assign.arr));
                            emit(c, OPC_BOX_PTR, (int)CAST_PTR, 0);
                        }
                        emit(c, OPC_STORE_FIELD, cls, fi_idx);
                        return (cls == 1) ? EXPR_TYPE_INT :
                               (cls == 2) ? EXPR_TYPE_DOUBLE : EXPR_TYPE_PTR;
                    }
                    /* 类型不匹配：已编译到 typed 栈但无法走 STORE_FIELD
                     * Phase D1 暂不支持不匹配 case（typed field 应配 typed val） */
                    fprintf(stderr, "IR: self.%s = val 类型不匹配 (field cls=%d, val et=%d)\n",
                            field_name, cls, (int)val_et);
                    /* 兜底：继续走动态路径会有栈不平衡，直接报错退出 */
                    return EXPR_TYPE_NONE;
                }
            }
        }
        /* 默认：动态 INDEX_SET（VAL_STRUCT_PTR 走 lumyr_field_set） */
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
        BinOp op = node->u.uny.op;
        /* 非空断言：子表达式落 VALUE，发 ASSERT_NONNULL，结果仍为 VALUE（动态） */
        if(op == OP_NONNULL_ASSERT) {
            c_expr_to_value(c, node->u.uny.child);
            emit(c, OPC_ASSERT_NONNULL, 0, 0);
            return EXPR_TYPE_NONE;
        }
        /* ++ / --：var++ / ++var / var-- / --var
         * 简化语义：作为语句时不区分前置/后置（结果丢弃）；作为表达式时后置返回原值，前置返回新值（暂未实现精确语义，统一按前置处理） */
        if(op == OP_POST_INC || op == OP_PRE_INC || op == OP_POST_DEC || op == OP_PRE_DEC) {
            AstNode* child = node->u.uny.child;
            int is_inc = (op == OP_POST_INC || op == OP_PRE_INC);
            if(child->type == AST_VAR) {
                const char* name = child->u.varname;
                int idx = c_find_var(c, name);
                if(idx < 0) idx = c_add_var(c, name, EXPR_TYPE_NONE);
                ExprType vt = c->var_types[idx];
                if(vt == EXPR_TYPE_INT) {
                    emit(c, OPC_LOAD_INT64_VAR, idx, 0);
                    emit(c, OPC_PUSH_INT64_CONST, 1, 0);
                    emit(c, is_inc ? OPC_INT64_ADD : OPC_INT64_SUB, 0, 0);
                    emit(c, OPC_STORE_INT64_VAR, idx, 0);
                } else if(vt == EXPR_TYPE_DOUBLE) {
                    /* 1.0 走常量池 → PUSH_CONST_IDX 压入 DOUBLE 栈 */
                    int kc = bf_add_double_const(c->fn, 1.0);
                    emit(c, OPC_LOAD_DOUBLE_VAR, idx, 0);
                    emit(c, OPC_PUSH_CONST_IDX, kc, 0);
                    emit(c, is_inc ? OPC_DOUBLE_ADD : OPC_DOUBLE_SUB, 0, 0);
                    emit(c, OPC_STORE_DOUBLE_VAR, idx, 0);
                } else {
                    /* VALUE 栈：动态 1 + add/sub */
                    emit(c, OPC_LOAD_VAR, idx, 0);
                    emit(c, OPC_PUSH_INT_VAL, 1, 0);
                    emit(c, is_inc ? OPC_ADD : OPC_SUB, 0, 0);
                    emit(c, OPC_STORE_VAR, idx, 0);
                }
                return vt;
            }
            /* TODO: 字段/下标 ++/--（暂不支持） */
            fprintf(stderr, "IR: ++/-- only supports simple variable\n");
            return EXPR_TYPE_NONE;
        }
        /* 逻辑非 !a：a 转 VALUE 栈，假则跳到 true 分支，结果 0/1 压 VALUE 栈 */
        if(op == OP_LOGIC_NOT) {
            ExprType ct = c_expr(c, node->u.uny.child);
            if(ct != EXPR_TYPE_NONE) emit_to_dynamic(c, ct, c_expr_cast_type(c, node->u.uny.child));
            int jtrue = emit_here(c, OPC_JMP_IF_FALSE_V, 0, 0);   /* a 假 → !a = true */
            emit(c, OPC_PUSH_INT_VAL, 0, 0);                       /* a 真 → !a = false */
            int jend = emit_here(c, OPC_JMP, 0, 0);
            patch_to(c, jtrue);
            emit(c, OPC_PUSH_INT_VAL, 1, 0);                       /* a 假 → !a = true */
            patch_to(c, jend);
            return EXPR_TYPE_NONE;
        }
        /* 一元运算：负号 */
        ExprType child_type = c_expr(c, node->u.uny.child);
        if(op == OP_UNARY_MINUS) {
            if(child_type == EXPR_TYPE_NONE) {
                emit(c, OPC_VNEG, 0, 0);   /* 动态 Value 一元负 */
            } else {
                emit(c, OPC_NEG, (int)child_type, 0);
            }
        }
        /* 按位取反 ~：整数走 INT64 栈；否则转动态，运行时校验 */
        if(op == OP_BIT_NOT) {
            if(child_type == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_BNOT, 0, 0);
                return EXPR_TYPE_INT;
            }
            if(child_type != EXPR_TYPE_NONE)
                emit_to_dynamic(c, child_type, c_expr_cast_type(c, node->u.uny.child));
            emit(c, OPC_VBNOT, 0, 0);
            return EXPR_TYPE_NONE;
        }
        return child_type;
    }

    case AST_BINOP: {
        /* 逻辑运算 && || 短路求值：left/right 转 VALUE 栈，结果（0/1 或原值）压 VALUE 栈 */
        if(node->u.bin.op == OP_LOGIC_AND || node->u.bin.op == OP_LOGIC_OR) {
            int is_and = (node->u.bin.op == OP_LOGIC_AND);
            /* left 压 VALUE 栈 */
            ExprType lt = c_expr(c, node->u.bin.left);
            if(lt != EXPR_TYPE_NONE) emit_to_dynamic(c, lt, c_expr_cast_type(c, node->u.bin.left));
            /* 短路：AND 假则跳 push_short；OR 真则跳 push_short */
            int jshort = emit_here(c, is_and ? OPC_JMP_IF_FALSE_V : OPC_JMP_IF_TRUE_V, 0, 0);
            /* right 压 VALUE 栈（作为正常路径结果） */
            ExprType rt = c_expr(c, node->u.bin.right);
            if(rt != EXPR_TYPE_NONE) emit_to_dynamic(c, rt, c_expr_cast_type(c, node->u.bin.right));
            int jend = emit_here(c, OPC_JMP, 0, 0);
            /* short 分支：AND push 0；OR push 1 */
            patch_to(c, jshort);
            emit(c, OPC_PUSH_INT_VAL, is_and ? 0 : 1, 0);
            patch_to(c, jend);
            return EXPR_TYPE_NONE;
        }
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
            } else if(lt == EXPR_TYPE_NONE) {
                /* 动态类型（如 next(g) 返回值）：从 VALUE 栈转字符串到 PTR 栈 */
                emit(c, OPC_CAST_STRING, 0, 0);
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
            } else if(rt == EXPR_TYPE_NONE) {
                /* 动态类型（如 next(g) 返回值）：从 VALUE 栈转字符串到 PTR 栈 */
                emit(c, OPC_CAST_STRING, 0, 0);
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
        /* 幂 **：按静态结果栈选择指令（运行时各自完成幂运算） */
        case OP_POW:
            if(result == EXPR_TYPE_INT) {
                emit(c, OPC_INT64_POW, 0, 0);
            } else if(result == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_DOUBLE_POW, 0, 0);
            } else {
                emit(c, OPC_VPOW, 0, 0);
            }
            break;
        /* 位运算：整数走 INT64 栈；动态走 VALUE 栈（运行时校验整数） */
        case OP_BIT_AND: case OP_BIT_OR: case OP_BIT_XOR:
        case OP_SHL: case OP_SHR: {
            if(result == EXPR_TYPE_INT) {
                switch(node->u.bin.op) {
                case OP_BIT_AND: emit(c, OPC_INT64_BAND, 0, 0); break;
                case OP_BIT_OR:  emit(c, OPC_INT64_BOR, 0, 0); break;
                case OP_BIT_XOR: emit(c, OPC_INT64_BXOR, 0, 0); break;
                case OP_SHL:     emit(c, OPC_INT64_SHL, 0, 0); break;
                case OP_SHR:     emit(c, OPC_INT64_SHR, 0, 0); break;
                default: break;
                }
            } else {
                switch(node->u.bin.op) {
                case OP_BIT_AND: emit(c, OPC_VBAND, 0, 0); break;
                case OP_BIT_OR:  emit(c, OPC_VBOR, 0, 0); break;
                case OP_BIT_XOR: emit(c, OPC_VBXOR, 0, 0); break;
                case OP_SHL:     emit(c, OPC_VSHL, 0, 0); break;
                case OP_SHR:     emit(c, OPC_VSHR, 0, 0); break;
                default: break;
                }
            }
            break;
        }
        case OP_GT: case OP_LT: case OP_GE:
        case OP_LE: case OP_EQ: case OP_NE: {
            /* 字符串（PTR 栈）相等比较：两操作数此时已在 PTR 栈（bottom=left/top=right）。
               先各自 BOX_PTR 到 VALUE，再走 VEQ/VNE。此前误落到下面的 VEQ 直接弹
               VALUE 栈（字符串却在 PTR 栈），导致 "a"=="a" 恒 false。
               注意 bigint/decimal 比较在前面专用分支已提前 return，不会到达此处。 */
            if(result == EXPR_TYPE_PTR) {
                if(node->u.bin.op == OP_EQ || node->u.bin.op == OP_NE) {
                    emit(c, OPC_BOX_PTR, (int)CAST_STRING, 0);   /* right */
                    emit(c, OPC_BOX_PTR, (int)CAST_STRING, 0);   /* left */
                    emit(c, node->u.bin.op == OP_EQ ? OPC_VEQ : OPC_VNE, 0, 0);
                    return EXPR_TYPE_NONE;
                }
                /* 字符串的大小比较无明确语义，保持原路径不处理 */
            }
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
        /* 记录变量持有的自定义类型名，供后续 recv.method() 推断接收者类型 */
        record_var_owner(c, bf_idx, node->u.assign.expr);
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

    /* null 合并 a ?? b：a truthy 用 a，否则用 b */
    case AST_NULL_COALESCE: {
        ExprType lt = c_expr(c, node->u.null_coalesce.left);
        CastKind lk = c_expr_cast_type(c, node->u.null_coalesce.left);
        if(lt != EXPR_TYPE_NONE) emit_to_dynamic(c, lt, lk);
        emit(c, OPC_DUP, 0, 0);                                /* 栈顶：left, left */
        int jfalse = emit_here(c, OPC_JMP_IF_FALSE_V, 0, 0);    /* 弹一个 left，falsy 跳 push_right */
        int jend = emit_here(c, OPC_JMP, 0, 0);                 /* 栈顶剩 left，跳 end */
        patch_to(c, jfalse);
        emit(c, OPC_POP, 0, 0);                                /* 弹掉 left 副本 */
        ExprType rt = c_expr(c, node->u.null_coalesce.right);
        if(rt != EXPR_TYPE_NONE) emit_to_dynamic(c, rt, c_expr_cast_type(c, node->u.null_coalesce.right));
        patch_to(c, jend);
        return EXPR_TYPE_NONE;
    }

    /* 安全调用 obj?.method(args) / obj?.field
     * 实现：obj 编译存临时变量，DUP+null 检查跳到 push_null，
     * truthy 时调用 method（用 ast_method_call(ast_var(tmp), method, args)），
     * null 时压 PUSH_NONE */
    case AST_SAFE_CALL: {
        AstNode* obj = node->u.safe_call.obj;
        const char* method = node->u.safe_call.method;
        AstNode* args = node->u.safe_call.args;
        static int safe_call_counter = 0;
        char tmp_name[64];
        snprintf(tmp_name, sizeof(tmp_name), "__safe_call_recv_%d", safe_call_counter++);
        /* 编译 obj 到 VALUE 栈，存临时变量（避免 obj 副作用重复执行） */
        ExprType ot = c_expr(c, obj);
        if(ot != EXPR_TYPE_NONE) emit_to_dynamic(c, ot, c_expr_cast_type(c, obj));
        int recv_idx = c_add_var(c, tmp_name, EXPR_TYPE_NONE);
        int recv_bf = bf_sym(c->fn, tmp_name);
        if(recv_bf >= 0 && recv_bf < c->fn->sym_cnt) c->fn->var_type_tags[recv_bf] = -1;
        emit(c, OPC_STORE_VAR, recv_idx, 0);
        /* DUP+null 检查 */
        emit(c, OPC_LOAD_VAR, recv_idx, 0);
        int jfalse = emit_here(c, OPC_JMP_IF_FALSE_V, 0, 0);   /* null 跳 push_null */
        /* truthy：args 非空=安全方法调用 recv.method(args)；args 为空=安全属性访问 recv["method"] */
        ExprType mt;
        CastKind mt_cast;
        AstNode* member_expr;
        if(args) {
            member_expr = ast_method_call(ast_var(strdup(tmp_name)), strdup(method), args);
        } else {
            member_expr = ast_index(ast_var(strdup(tmp_name)), ast_string(strdup(method)));
        }
        mt = c_expr(c, member_expr);
        mt_cast = c_expr_cast_type(c, member_expr);
        if(mt != EXPR_TYPE_NONE) emit_to_dynamic(c, mt, mt_cast);
        int jend = emit_here(c, OPC_JMP, 0, 0);
        /* push_null: PUSH_NONE */
        patch_to(c, jfalse);
        emit(c, OPC_PUSH_NONE, 0, 0);
        patch_to(c, jend);
        return EXPR_TYPE_NONE;
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

    /* 字面量（4 栈归一：bool/char 走 INT64 栈 → int） */
    if(node->type == AST_INT) return "int";
    if(node->type == AST_NUM) return "double";
    if(node->type == AST_BOOL) return "int";
    if(node->type == AST_CHAR) return "int";
    if(node->type == AST_STRING) return "string";
    if(node->type == AST_NONE) return "null";

    /* 类型标注 <type>expr → 标注类型的规范名 */
    if(node->type == AST_TYPE_ANNOTATION) {
        return castkind_canonical_name(node->u.type_annotation.cast_type);
    }

    /* 强转 (type)expr → 强转类型的规范名 */
    if(node->type == AST_CAST) {
        return castkind_canonical_name(node->u.cast.cast_type);
    }

    /* 变量：查符号表，返回变量类型的规范名 */
    if(node->type == AST_VAR) {
        int bf_idx = bf_sym(c->fn, node->u.varname);
        if(bf_idx >= 0 && bf_idx < c->fn->sym_cnt) {
            CastKind ck = (CastKind)c->fn->var_type_tags[bf_idx];
            return castkind_canonical_name(ck);
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

/* self_field_lookup：检测 node 是否是 self.<字段名> 访问
 * 返回 FieldInfo*（含偏移/精确类型/嵌套类型名）；不匹配返回 NULL
 * lumyr_self_field_castkind / c_expr_owner_type 共用，单点封装检测逻辑 */
static FieldInfo* self_field_lookup(Ctx* c, AstNode* node) {
    if(!node || node->type != AST_INDEX) return NULL;
    if(!node->u.index.arr || node->u.index.arr->type != AST_VAR) return NULL;
    const char* vn = node->u.index.arr->u.varname;
    /* self 或 super：super 用父类字段表 */
    if(strcmp(vn, "self") != 0 && strcmp(vn, "super") != 0) return NULL;
    if(!c->fn->method_self_struct) return NULL;
    if(!node->u.index.idx || node->u.index.idx->type != AST_STRING) return NULL;

    /* method_self_struct 可能带 "class:" 前缀（class 方法设置） */
    const char* tname = c->fn->method_self_struct;
    if(strncmp(tname, "class:", 6) == 0) tname += 6;
    TypeDef* td = struct_lookup(tname);
    if(!td) td = class_lookup(tname);
    /* super：跳到父类字段表 */
    if(strcmp(vn, "super") == 0 && td && td->parent) {
        td = class_lookup(td->parent);
        if(!td) td = struct_lookup(tname);
    }
    if(!td || !td->runtime_info) return NULL;

    return lumyr_type_find_field(td->runtime_info, node->u.index.idx->u.sval);
}

/* lumyr_self_field_castkind：检测 node 是否是 self.field 访问
 * 若是，返回字段的 CastKind（INT/DOUBLE/STRING 等）；否则返回 CAST_NONE
 * 用于 arith_get_expr_type 和 c_expr_cast_type 统一识别 self.field 类型
 * 使得 self.x + 1 等 BINOP 能走 typed 栈路径，而非全部压 VALUE 栈 */
CastKind lumyr_self_field_castkind(Ctx* c, AstNode* node) {
    FieldInfo* fi = self_field_lookup(c, node);
    if(!fi) return CAST_NONE;

    /* 仅当字段类型支持 typed 栈路由（INT64/DOUBLE/PTR）时返回 CastKind
     * 否则返回 CAST_NONE 让表达式走 VALUE 栈动态路径 */
    int cls = lumyr_etype_stackcls(fi->valtype);
    if(cls == 0) return CAST_NONE;

    return (CastKind)valuetype_to_castkind((int)fi->valtype);
}

/* c_expr_owner_type：推断表达式持有的自定义类型名（struct/class 实例）
 * 覆盖形态：构造（AST_CLASS_NEW）、self、普通变量（var_struct_names）、self.field 嵌套字段
 * 返回 strdup 的类型名（调用方释放）；非实例或类型未知返回 NULL
 * 用于 AST_ASSIGN 记录变量属主类型、AST_METHOD_CALL 推断接收者类型 */
char* c_expr_owner_type(Ctx* c, AstNode* node) {
    if(!node) return NULL;

    /* 构造表达式：类型名直接可知 */
    if(node->type == AST_CLASS_NEW) return strdup(node->u.class_new.class_name);

    if(node->type == AST_VAR) {
        const char* vn = node->u.varname;
        /* self：方法属主类型 */
        if(strcmp(vn, "self") == 0 && c->fn->method_self_struct) {
            const char* t = c->fn->method_self_struct;
            if(strncmp(t, "class:", 6) == 0) t += 6;
            return strdup(t);
        }
        /* super：返回父类名，让方法分派查父类方法表 */
        if(strcmp(vn, "super") == 0 && c->fn->method_self_struct) {
            const char* t = c->fn->method_self_struct;
            if(strncmp(t, "class:", 6) == 0) t += 6;
            TypeDef* td = class_lookup(t);
            if(td && td->parent) return strdup(td->parent);
            return NULL;
        }
        /* 普通变量：查平行数组 var_struct_names（由 AST_ASSIGN 记录） */
        for(int i = 0; i < c->fn->sym_cnt; i++) {
            if(c->fn->syms[i] && strcmp(c->fn->syms[i], vn) == 0) {
                if(c->fn->var_struct_names && c->fn->var_struct_names[i])
                    return strdup(c->fn->var_struct_names[i]);
                return NULL;
            }
        }
        return NULL;
    }

    /* self.field 嵌套字段：FieldInfo.type_name 记录字段的自定义类型名 */
    if(node->type == AST_INDEX) {
        FieldInfo* fi = self_field_lookup(c, node);
        if(fi && fi->type_name) return strdup(fi->type_name);
    }

    return NULL;
}

/* record_var_owner：为变量槽 bf_idx 记录 RHS 持有的自定义类型名（struct/class）
 * RHS 可推断（构造/变量传递/self.field）→ 替换；无法推断（动态返回值）→ 保留旧记录不清空。
 * c_expr 与 c_stmt 的 AST_ASSIGN 共用，保证属主类型单点维护。 */
static void record_var_owner(Ctx* c, int bf_idx, AstNode* rhs) {
    char* owner = c_expr_owner_type(c, rhs);
    if(!owner) return;
    if(!c->fn->var_struct_names) {
        int cap = c->fn->sym_cap > 0 ? c->fn->sym_cap : 16;
        c->fn->var_struct_names = (char**)calloc((size_t)cap, sizeof(char*));
    }
    if(c->fn->var_struct_names[bf_idx]) free(c->fn->var_struct_names[bf_idx]);
    c->fn->var_struct_names[bf_idx] = owner;
}

/* 获取表达式的精确类型（CastKind），用于打印格式化 */
static CastKind c_expr_cast_type(Ctx* c, AstNode* node) {
    if(!node) return CAST_NONE;

    /* 字面量：整数按实际宽度返回——超过 int32 的大字面量为 CAST_INT64，
     * 否则在"提升装箱到动态"时会被 CAST_INT 截断（如 dyn == 1099511627776 误判）。
     * 与 c_expr_to_value 中 AST_INT 走 PUSH_CONST_VAL(int64) 的处理保持一致。 */
    if(node->type == AST_INT) {
        int64_t lv = node->u.inum;
        return (lv >= INT32_MIN && lv <= INT32_MAX) ? CAST_INT : CAST_INT64;
    }
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

    /* 方法调用 recv.method(args)：查属主类型方法签名 → ret_type_name → CastKind
     * 否则 print 等场景下 cast_type=CAST_NONE，string/bigint/decimal 返回值被当裸指针打印 */
    if(node->type == AST_METHOD_CALL) {
        AstNode* recv = node->u.method_call.recv;
        const char* mname = node->u.method_call.method;
        char* owner = c_expr_owner_type(c, recv);
        TypeDef* td = owner ? type_lookup(owner) : NULL;
        CastKind ck = CAST_NONE;
        if(td && td->runtime_info) {
            RuntimeFunc* rf = lumyr_type_find_method(td->runtime_info, mname);
            if(rf && interp_func_is_payload(rf)) {
                InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
                BytecodeFunc* def_fn = pl->bytecode;
                if(def_fn && def_fn->ret_type_name)
                    ck = ir_type_name_to_castkind(def_fn->ret_type_name);
            }
        }
        free(owner);
        return ck;
    }

    /* struct/class 构造：返回对应的 CAST_STRUCT_PTR/CAST_CLASS_PTR */
    if(node->type == AST_CLASS_NEW) {
        const char* tname = node->u.class_new.class_name;
        if(struct_lookup(tname)) return CAST_STRUCT_PTR;
        if(class_lookup(tname)) return CAST_CLASS_PTR;
        return CAST_PTR;
    }

    /* self.field 访问（AST_INDEX）：委托 lumyr_self_field_castkind
     * 让 self.x + 1 等 BINOP 的 c_expr_cast_type(self.x) 返回字段 CastKind
     * 而非 CAST_NONE（导致 c_value_fallback 误压 VALUE 栈） */
    if(node->type == AST_INDEX) {
        CastKind ck = lumyr_self_field_castkind(c, node);
        if(ck != CAST_NONE) return ck;
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

    /* 三元 cond ? a : b：复用分支类型预判 + 统一规则，把目标类型名转 CastKind。
     * 否则字符串分支结果被当 CAST_NONE，PTR 变量 LOAD 时读成裸 pointer 而非 string。 */
    if(node->type == AST_TERNARY) {
        const char* nt = c_expr_type_name(c, node->u.ternary.true_expr);
        const char* nf = c_expr_type_name(c, node->u.ternary.false_expr);
        return ir_type_name_to_castkind(ternary_target_name(nt, nf));
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
        else if(to == EXPR_TYPE_PTR)
            /* VALUE -> PTR：string 等裸指针拆箱（UNBOX_PTR 对 string 做独立拷贝） */
            emit(c, OPC_UNBOX_PTR, 0, 0);
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
        emit(c, OPC_BOX_INT64, (int)ck, 0);   /* 携带整型子类型 uint/char/bool/... */
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
        /* 按位取反：静态整数走 typed INT64（保留整数性），以 int64 装箱不截断；
         * 动态子树 c_expr 已发 VBNOT，结果已在 VALUE，无需处理 */
        if(node->u.uny.op == OP_BIT_NOT) {
            ExprType t = c_expr(c, node);
            if(t == EXPR_TYPE_INT) emit(c, OPC_BOX_INT64, CAST_NONE, 0);
            return;
        }
        c_value_fallback(c, node);
        return;
    }

    /* 二元运算：左右子树均目标 VALUE，发通用 Value 指令 */
    case AST_BINOP: {
        int bop = node->u.bin.op;
        /* 幂与位运算：不走通用 value 算术（lumyr_add 对 int64 会提升为 double，
         * 位运算又严格要求整数）。改为自然编译——静态整数走 typed INT64（保留整数性），
         * 浮点走 DOUBLE，动态 c_expr 已发 V 指令落 VALUE；typed 结果按 int64/double 装箱，
         * int64 装箱不做 CAST_INT 截断。 */
        if(bop == OP_POW || bop == OP_BIT_AND || bop == OP_BIT_OR ||
           bop == OP_BIT_XOR || bop == OP_SHL || bop == OP_SHR) {
            ExprType t = c_expr(c, node);
            if(t == EXPR_TYPE_INT)
                emit(c, OPC_BOX_INT64, CAST_NONE, 0);
            else if(t == EXPR_TYPE_DOUBLE)
                emit(c, OPC_BOX_DOUBLE, 0, 0);
            return;
        }
        int vop = binop_value_opcode(bop);
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

static void layer_push(Ctx* c, int kind, const char* label) {
    if(c->layer_depth >= c->layers_cap) {
        c->layers_cap = c->layers_cap ? c->layers_cap * 2 : 8;
        c->layers = (Layer*)realloc(c->layers, sizeof(Layer) * c->layers_cap);
        if(!c->layers) { perror("layer_push"); exit(EXIT_FAILURE); }
    }
    Layer* L = &c->layers[c->layer_depth++];
    L->kind = kind;
    L->label = label ? strdup(label) : NULL;
    L->brk = NULL; L->brk_cnt = L->brk_cap = 0;
    L->cont = NULL; L->cont_cnt = L->cont_cap = 0;
    L->brk_fin = NULL; L->brk_fin_cnt = L->brk_fin_cap = 0;
    L->cont_fin = NULL; L->cont_fin_cnt = L->cont_fin_cap = 0;
    L->cont_target = -1;
}

/* 按 label 向上搜索 layer 栈，找到首个匹配的循环层；NULL=用最近循环层 */
static Layer* layer_find_labeled(Ctx* c, const char* label) {
    if(!label) return layer_top(c);
    for(int i = c->layer_depth - 1; i >= 0; i--) {
        Layer* L = &c->layers[i];
        if(L->kind != 0) continue;  /* 只匹配循环层（跳过 switch 层） */
        if(L->label && strcmp(L->label, label) == 0) return L;
    }
    return NULL;
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
    free(L->label);
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

/* resolve_self_recursive：方法体内以裸名调用自身（自递归）时，
 * 函数表注册名是内部唯一名、扁平名查不到；此处匹配当前方法并构造仅含形参链的
 * 临时 def 壳（compile_user_call 仅遍历 def 的形参签名），返回当前 fn；否则 NULL */
static BytecodeFunc* resolve_self_recursive(Ctx* c, const char* func_name, AstNode* tmp_def) {
    if(!c->fn->class_name || !c->fn->name) return NULL;
    const char* msep = strstr(c->fn->name, "__m__");
    if(!msep || strcmp(msep + 5, func_name) != 0) return NULL;
    memset(tmp_def, 0, sizeof(AstNode));
    tmp_def->type = AST_FUNC_DEF;
    tmp_def->u.func_def.params = c->cur_params;
    return c->fn;
}

/* 编译用户自定义函数调用。
 * 实参按形参声明左至右绑定，按形参类型转换；缺实参用默认值；个数校验。
 * 发 OPC_CALL(a=callsite 下标, b=绑定参数个数)，返回 callee 返回类型。
 * 注意：可变形参 ...args 由 Task 9 处理，本函数遇到即停（多余实参 Task 9 再组装）。 */
static ExprType compile_user_call(Ctx* c, BytecodeFunc* callee, AstNode* def_ast, AstNode* args, int keep_result, AstNode* method_recv)
{
    /* 1. 收集实参到数组（左至右，中序遍历左倾 SEQ 树） */
    int argc = 0, acap = 0;
    AstNode** argv = NULL;
    collect_call_args(args, &argv, &argc, &acap);

    /* ---- 命名实参拆分：形如 id = expr 的实参（AST_ASSIGN）按形参名绑定，其余为位置实参 ----
     * 1) 建立用户形参名表（方法跳过 self，遇可变停止）；
     * 2) 位置实参入 posv；AST_ASSIGN 实参按名定位形参槽，校验 未知/重复/与位置冲突；
     * 3) src_for_slot[相对形参下标] = 该槽实参节点（位置或命名）。 */
    int nnormal = 0, pcap = 0;
    char** pnames = NULL;
    /* 建立用户形参名表：方法跳过 self，遇可变形参停止 */
    {
        int pi = 0;
        for(AstNode* p = def_ast->u.func_def.params; p; p = p->u.param.next, pi++) {
            if(method_recv && pi == 0) continue;      /* self */
            if(p->u.param.is_ellipsis) break;
            if(nnormal >= pcap) {
                pcap = pcap ? pcap * 2 : 8;
                pnames = (char**)realloc(pnames, sizeof(char*) * (size_t)pcap);
            }
            pnames[nnormal++] = p->u.param.name;
        }
    }
    AstNode** src_for_slot = (AstNode**)calloc(nnormal > 0 ? nnormal : 1, sizeof(AstNode*));
    int posc = 0, poscap = 0;
    AstNode** posv = NULL;
    int* named_slot_used = (int*)calloc(nnormal > 0 ? nnormal : 1, sizeof(int));
    int named_err = 0;
    for(int i = 0; i < argc; i++) {
        AstNode* a = argv[i];
        if(a->type == AST_ASSIGN) {
            const char* nm = a->u.assign.varname;
            int hit = -1;
            for(int k = 0; k < nnormal; k++)
                if(pnames[k] && strcmp(pnames[k], nm) == 0) { hit = k; break; }
            if(hit < 0) {
                fprintf(stderr, "IR: 调用 %s 没有名为 %s 的形参\n",
                        callee->name ? callee->name : "?", nm);
                named_err = 1;
            } else if(named_slot_used[hit]) {
                fprintf(stderr, "IR: 调用 %s 的命名实参 %s 重复\n",
                        callee->name ? callee->name : "?", nm);
                named_err = 1;
            } else {
                named_slot_used[hit] = 1;
                src_for_slot[hit] = a->u.assign.expr;
            }
        } else {
            if(posc >= poscap) {
                poscap = poscap ? poscap * 2 : 8;
                posv = (AstNode**)realloc(posv, sizeof(AstNode*) * (size_t)poscap);
            }
            posv[posc] = a;
            if(posc < nnormal) src_for_slot[posc] = a;
            posc++;
        }
    }
    /* 命名槽若已被位置实参占据（hit < posc）则冲突 */
    for(int k = 0; k < nnormal; k++) {
        if(named_slot_used[k] && k < posc) {
            fprintf(stderr, "IR: 调用 %s 的形参 %s 已由位置实参占据，不能再用命名实参\n",
                    callee->name ? callee->name : "?", pnames[k]);
            named_err = 1;
        }
    }
    if(named_err) {
        free(argv); free(pnames); free(src_for_slot);
        free(named_slot_used); free(posv);
        exit(EXIT_FAILURE);
    }

    /* 方法调用：method_recv 非空。栈布局 slot 0=receiver（self），slot 1+=用户实参。
     * 先编译 receiver 占底（VM 弹栈时实参在顶先弹，receiver 最后弹） */
    int is_method = (method_recv != NULL);
    int base_slot = is_method ? 1 : 0;
    int total = 0;
    if(is_method) {
        ExprType rt0 = c_expr(c, method_recv);
        if(rt0 != EXPR_TYPE_PTR) {
            fprintf(stderr, "IR: %s 方法调用的接收者不是实例引用（receiver 类型=%d）\n",
                    callee->name ? callee->name : "?", (int)rt0);
        }
        total = 1;
    }

    /* 2. 逐形参绑定（方法时第一个形参 self 已由 receiver 占据，跳过；可变形参前停止）
     * 注意：bound 是形参数（≥用户实参 argc，默认参数场景 argc < bound），
     * total = base_slot + bound + (可变?1:0) 可能 > nslots = base_slot + argc，
     * 故 ref_flags/ref_slots 容量取 max(nslots, sym_cnt) 防止 ASAN heap-buffer-overflow */
    int bound = 0;
    int nslots = base_slot + argc;
    int alloc_n = nslots > 0 ? nslots : 1;
    if(callee->sym_cnt > alloc_n) alloc_n = callee->sym_cnt;
    int* ref_flags = (int*)calloc(alloc_n, sizeof(int));
    int* ref_slots = (int*)malloc(alloc_n * sizeof(int));
    for(int i = 0; i < alloc_n; i++) ref_slots[i] = -1;
    int pindex = 0;
    for(AstNode* p = def_ast->u.func_def.params; p; p = p->u.param.next, pindex++) {
        if(is_method && pindex == 0) continue;       /* self 槽已由 receiver 填 */
        if(p->u.param.is_ellipsis) break;      /* 可变槽 Task 9 */
        int slot = base_slot + bound;
        /* 形参类型（slot==形参符号下标；var_type_tags 初值 -1） */
        CastKind pck = (slot < callee->sym_cnt) ? (CastKind)callee->var_type_tags[slot] : CAST_NONE;
        ExprType param_et = castkind_to_exprtype(pck);

        AstNode* arg_node = src_for_slot[bound];   /* 该槽实参（位置或命名），无则 NULL */

        if(p->u.param.is_ref && arg_node) {
            /* ref 形参：实参必须是左值（当前支持变量）；命名写法 f(x=var) 亦同 */
            if(arg_node->type != AST_VAR) {
                fprintf(stderr, "IR: 调用 %s 的 ref 形参 %s 需要左值（变量）\n",
                        callee->name ? callee->name : "?", p->u.param.name);
            } else {
                int cslot = c_find_var(c, arg_node->u.varname);
                if(cslot >= 0) {
                    ref_flags[slot] = 1;
                    ref_slots[slot] = cslot;
                }
            }
        }

        if(arg_node) {
            if(param_et != EXPR_TYPE_NONE) {
                ExprType at = c_expr(c, arg_node);
                /* 非空 T：动态(VALUE)实参（含显式 null，c_expr 归为 NONE）在拆箱前断言；
                 * 静态 typed 实参不可能为 null，无需断言（零开销）。可空 T? 跳过。 */
                if(at == EXPR_TYPE_NONE && !p->u.param.is_nullable)
                    emit(c, OPC_ASSERT_NONNULL, 0, 0);
                emit_value_cast(c, at, param_et);   /* typed -> typed */
            } else {
                c_expr_to_value(c, arg_node);       /* 上下文目标 VALUE，免 BOX */
            }
        } else if(p->u.param.default_val) {
            if(param_et != EXPR_TYPE_NONE) {
                ExprType at = c_expr(c, p->u.param.default_val);
                if(at == EXPR_TYPE_NONE && !p->u.param.is_nullable)
                    emit(c, OPC_ASSERT_NONNULL, 0, 0);
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
    total += bound;

    /* 3. 可变参数：仅位置实参可溢出（命名实参不能进入可变槽）。
     *    超出普通形参的位置实参组装为数组，绑定到可变槽。 */
    if(callee->has_variadic) {
        int extra = posc - nnormal;
        if(extra < 0) extra = 0;
        for(int i = 0; i < extra; i++) {
            c_expr_to_value(c, posv[nnormal + i]);
        }
        emit(c, OPC_ARRAY_LIT, 0, extra);   /* 弹 extra 个 VALUE，压数组 */
        total += 1;                         /* 数组作为可变槽 */
    } else if(posc > nnormal) {
        fprintf(stderr, "IR: 调用 %s 实参过多：%d 个，最多 %d 个\n",
                callee->name ? callee->name : "?", posc, nnormal);
    }

    /* 4. 按 callee 返回标注确定结果类型（无标注→动态 NONE），供 callsite 记录压栈目标。
     *    生成器函数例外：调用返回的是生成器对象（VALUE 栈），声明的返回类型描述
     *    的是 yield 产出的值类型，不应影响调用结果压栈路由。 */
    ExprType ret_et = EXPR_TYPE_NONE;
    if(callee->ret_type_name && !callee->is_generator) {
        CastKind rck = ir_type_name_to_castkind(callee->ret_type_name);
        ret_et = castkind_to_exprtype(rck);
    }

    /* 5. 登记调用点并发 CALL / CALL_METHOD。
     * callee 键记录运行时可定位的注册键（重载版本 table_key，否则内部名/源名）；
     * CALL_METHOD 运行时按 receiver 实际类型重新解析 */
    const char* callee_key = callee->table_key ? callee->table_key :
                             (callee->name ? callee->name : "?");
    int cs = bf_add_callsite(c->fn, callee_key,
                             total, keep_result, (int)ret_et);
    CallSite* csp = &c->fn->callsites[cs];
    for(int i = 0; i < total; i++) {
        csp->arg_is_ref[i] = ref_flags[i];
        csp->arg_ref_slots[i] = ref_slots[i];
    }
    free(ref_flags);
    free(ref_slots);
    emit(c, is_method ? OPC_CALL_METHOD : OPC_CALL, cs, total);
    free(argv);
    free(pnames);
    free(src_for_slot);
    free(named_slot_used);
    free(posv);

    return ret_et;
}

/* 编译方法调用 recv.method(args)。
 * 按 receiver 静态类型解析方法签名 → 参数 typed 路由 → OPC_CALL_METHOD；
 * VM 运行时按 receiver 实际类型（方法表含继承槽位，重写覆盖在原位置）分派 → 多态。
 * keep_result=1 表达式语境压返回值；0 语句语境丢弃。c_expr/c_stmt 共用。 */
static ExprType compile_method_call_expr(Ctx* c, AstNode* node, int keep_result) {
    AstNode* recv = node->u.method_call.recv;
    const char* mname = node->u.method_call.method;
    AstNode* margs = node->u.method_call.args;

    /* 1. 推断 receiver 类型名 + 定位 TypeDef */
    char* owner = c_expr_owner_type(c, recv);
    TypeDef* td = owner ? type_lookup(owner) : NULL;

    /* 2. 用户类型方法（同名覆盖内置：用户方法优先，沿继承链解析 → 多态） */
    if(td) {
        AstNode* mdef_ast = type_find_method_ast(owner, mname);
        BytecodeFunc* def_fn = NULL;
        if(td->runtime_info) {
            RuntimeFunc* rf = lumyr_type_find_method(td->runtime_info, mname);
            if(rf && interp_func_is_payload(rf)) {
                InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
                def_fn = pl->bytecode;
            }
        }
        if(mdef_ast && def_fn) {
            /* super.method()：静态分派到父类方法（不走多态，避免重写方法无限递归）
             * 栈布局同普通方法：slot0=self（super 加载 self 指针），slot1+=实参；
             * 但 emit OPC_CALL（callee=父类方法内部名如 S1__m__who）直接查表调用 */
            if(recv->type == AST_VAR && strcmp(recv->u.varname, "super") == 0) {
                int argc = 0, acap = 0;
                AstNode** argv = NULL;
                collect_call_args(margs, &argv, &argc, &acap);
                /* slot 0: self pointer (super = 同一实例) */
                ExprType rt0 = c_expr(c, recv);
                if(rt0 != EXPR_TYPE_PTR) {
                    fprintf(stderr, "IR: super.%s 调用接收者非指针（et=%d）\n", mname, (int)rt0);
                }
                int total = 1;
                /* slot 1+: 用户实参，按 def_fn 的 slot 1+ 形参类型 cast */
                int pindex = 0, bound = 0;
                for(AstNode* p = mdef_ast->u.func_def.params; p; p = p->u.param.next, pindex++) {
                    if(pindex == 0) continue;  /* 跳过 self */
                    if(p->u.param.is_ellipsis) break;
                    int slot = 1 + bound;
                    CastKind pck = (slot < def_fn->sym_cnt) ? (CastKind)def_fn->var_type_tags[slot] : CAST_NONE;
                    ExprType param_et = castkind_to_exprtype(pck);
                    if(bound < argc) {
                        if(param_et != EXPR_TYPE_NONE) {
                            ExprType at = c_expr(c, argv[bound]);
                            emit_value_cast(c, at, param_et);
                        } else {
                            c_expr_to_value(c, argv[bound]);
                        }
                    }
                    bound++;
                }
                total += bound;
                /* 可变参数 */
                if(def_fn->has_variadic) {
                    int extra = argc - bound;
                    if(extra < 0) extra = 0;
                    for(int i = 0; i < extra; i++) c_expr_to_value(c, argv[bound + i]);
                    emit(c, OPC_ARRAY_LIT, 0, extra);
                    total += 1;
                }
                /* 返回类型 */
                ExprType ret_et = EXPR_TYPE_NONE;
                if(def_fn->ret_type_name) {
                    CastKind rck = ir_type_name_to_castkind(def_fn->ret_type_name);
                    ret_et = castkind_to_exprtype(rck);
                }
                /* 静态调用：callee=父类方法内部名（如 S1__m__who），VM 直接查表 */
                int cs = bf_add_callsite(c->fn, def_fn->name ? def_fn->name : "?",
                                         total, keep_result, (int)ret_et);
                emit(c, OPC_CALL, cs, total);
                free(argv);
                free(owner);
                return ret_et;
            }
            /* 普通方法调用：复用统一参数绑定路径（method_recv=recv → OPC_CALL_METHOD，self 槽自动占位） */
            ExprType ret = compile_user_call(c, def_fn, mdef_ast, margs, keep_result, recv);
            free(owner);
            return ret;
        }
    }

    /* 3. 内置方法（字符串/数组/字典/高阶/AI 线代/加密编码等）：
     * 编译期静态表解析 BuiltinId（无运行时字符串查表），receiver 与实参全部转 VALUE，
     * OPC_CALL_BUILTIN_METHOD 弹 argc 个实参再弹 receiver，按运行时类型分派 */
    int bid = builtin_id_by_name(mname);
    if(bid >= 0) {
        c_expr_to_value(c, recv);
        AstNode** av = NULL; int ac = 0, acp = 0;
        collect_call_args(margs, &av, &ac, &acp);
        for(int i = 0; i < ac; i++) c_expr_to_value(c, av[i]);
        free(av);
        free(owner);
        emit(c, OPC_CALL_BUILTIN_METHOD, bid, ac);
        if(!keep_result) emit(c, OPC_POP, 0, 0);
        return EXPR_TYPE_NONE;
    }

    /* 4. 无法确定类型：先动态取属性 recv[mname]（runtime 解析 map 值/实例字段/bound method），
     * 再 CALLV。不能直接把 recv 当函数——方法名会丢失。
     * 正常全类型标注路径不会进入；覆盖动态 map 函数值、动态取出实例的方法调用 */
    if(!owner || !td) {
        AstNode* prop = ast_index(recv, ast_string((char*)mname));
        c_expr_to_value(c, prop);
        AstNode** av = NULL; int ac = 0, acp = 0;
        collect_call_args(margs, &av, &ac, &acp);
        for(int i = 0; i < ac; i++) c_expr_to_value(c, av[i]);
        free(av);
        free(owner);
        emit(c, OPC_CALLV, 0, ac);
        return EXPR_TYPE_NONE;
    }

    /* 5. 用户类型既无该方法也非内置 */
    fprintf(stderr, "IR: 类型 \"%s\" 没有方法 \"%s\"\n", owner, mname);
    g_ir_compile_error = 1;
    free(owner);
    return EXPR_TYPE_NONE;
}

/* emit_deferred：LIFO 逆序编译所有已注册的 defer body。
 * 用于 AST_RETURN、AST_DEFER 之后的 fallthrough 路径与异常 handler 路径。 */
static void emit_deferred(Ctx* c) {
    if(!c || c->deferred_cnt <= 0) return;
    for(int i = c->deferred_cnt - 1; i >= 0; i--) {
        if(c->deferred[i]) c_stmt(c, c->deferred[i]);
    }
}

/* push_defer：把 defer body 压入函数级 LIFO 栈（按需扩容） */
static void push_defer(Ctx* c, AstNode* body) {
    if(!c) return;
    if(c->deferred_cnt >= c->deferred_cap) {
        c->deferred_cap = c->deferred_cap ? c->deferred_cap * 2 : 4;
        c->deferred = (AstNode**)realloc(c->deferred, sizeof(AstNode*) * c->deferred_cap);
        if(!c->deferred) { perror("push_defer"); exit(EXIT_FAILURE); }
    }
    c->deferred[c->deferred_cnt++] = body;
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
                    } else if(cast_type == CAST_STRING) {
                        emit(c, OPC_PRINT_PTR, (int)cast_type, 0);
                    } else {
                        /* 容器/实例引用（map/array/typed_array/struct_ptr/class_ptr）：
                         * 装箱后走 VALUE 打印路径，PRINT_PTR 只能打裸字符串 */
                        emit(c, OPC_BOX_PTR, (int)cast_type, 0);
                        emit(c, OPC_PRINT, 0, 0);
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
        /* 记录变量持有的自定义类型名（顶层语句赋值），供后续 recv.method() 推断接收者类型 */
        record_var_owner(c, bf_idx, node->u.assign.expr);
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

    case AST_METHOD_CALL:
        /* 语句形式方法调用：keep_result=0，callsite 不压返回值，无需 POP */
        compile_method_call_expr(c, node, 0);
        break;

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
        /* 表达式语句调用：编译但丢弃返回值（keep_result=0）。
         * 用户函数优先；非用户函数（局部变量持有函数值 / 内置）走 c_expr
         * 兜底，避免误报 "unknown function" 导致语句不执行 */
        const char* call_name = node->u.call.name;
        /* 重载名：按实参解析版本（keep_result=0） */
        if(ol_group_exists(call_name)) {
            BytecodeFunc* oc = NULL; AstNode* od = NULL;
            if(!resolve_free_call(c, call_name, node->u.call.args, &oc, &od))
                exit(EXIT_FAILURE);
            compile_user_call(c, oc, od, node->u.call.args, 0, NULL);
            break;
        }
        BytecodeFunc* callee = ir_func_table_lookup(call_name);
        AstNode* def_ast = func_ast_lookup(call_name);
        /* 方法内裸名自递归 fallback */
        AstNode self_tmp;
        BytecodeFunc* self_fn = resolve_self_recursive(c, call_name, &self_tmp);
        if(!callee && self_fn) {
            compile_user_call(c, self_fn, &self_tmp, node->u.call.args, 0, NULL);
        } else if(callee && def_ast) {
            compile_user_call(c, callee, def_ast, node->u.call.args, 0, NULL);
        } else {
            /* 非用户函数：走 c_expr 处理内置 / 动态调用，并丢弃返回值 */
            ExprType et = c_expr(c, node);
            int pop_sel = 0;
            if(et == EXPR_TYPE_INT) pop_sel = 1;
            else if(et == EXPR_TYPE_DOUBLE) pop_sel = 2;
            else if(et == EXPR_TYPE_PTR) pop_sel = 3;
            emit(c, OPC_POP, pop_sel, 0);
        }
        break;
    }

    case AST_INDEX_ASSIGN: {
        /* 下标/属性写语句：丢弃压回的结果。结果所在栈按编译返回类型选择，
         * 否则 self.field=typed 值（INT64/DOUBLE/PTR）会误弹 VALUE 偷调用方数据 */
        ExprType wet = c_expr(c, node);
        int pop_sel = 0;
        if(wet == EXPR_TYPE_INT) pop_sel = 1;
        else if(wet == EXPR_TYPE_DOUBLE) pop_sel = 2;
        else if(wet == EXPR_TYPE_PTR) pop_sel = 3;
        emit(c, OPC_POP, pop_sel, 0);
        break;
    }

    case AST_DESTRUCT: {
        /* 解构赋值：a, b, c = [1, 2, 3]
         * 实现：把 rhs 求值后存到临时变量 __destruct_tmp_N，然后对每个 i
         * 生成 names[i] = __destruct_tmp_N[i]，避免 rhs 重复执行
         * 注意：dst 变量可能已 typed（INT64/DOUBLE/PTR），需按其 var_type_tags
         * 选择对应 STORE 指令，避免 VALUE 槽位与 typed 槽位错位 */
        int n = node->u.destruct.count;
        char** names = node->u.destruct.names;
        static int destruct_counter = 0;
        char tmp_name[64];
        snprintf(tmp_name, sizeof(tmp_name), "__destruct_tmp_%d", destruct_counter++);
        /* 编译 rhs（数组/可索引容器），结果压 VALUE 栈 */
        ExprType rt = c_expr(c, node->u.destruct.rhs);
        if(rt != EXPR_TYPE_NONE) emit_to_dynamic(c, rt, c_expr_cast_type(c, node->u.destruct.rhs));
        /* 存到临时变量（VALUE 槽位） */
        int var_idx = c_add_var(c, tmp_name, EXPR_TYPE_NONE);
        int bf_idx = bf_sym(c->fn, tmp_name);
        if(bf_idx >= 0 && bf_idx < c->fn->sym_cnt) c->fn->var_type_tags[bf_idx] = -1;
        emit(c, OPC_STORE_VAR, var_idx, 0);
        /* 对每个 i：names[i] = __destruct_tmp_N[i] */
        for(int i = 0; i < n; i++) {
            /* 加载临时数组到 VALUE 栈 */
            emit(c, OPC_LOAD_VAR, var_idx, 0);
            /* 压索引 i 到 VALUE 栈 */
            emit(c, OPC_PUSH_INT_VAL, i, 0);
            /* arr[i] -> VALUE 栈（动态 Value） */
            emit(c, OPC_INDEX_GET, 0, 0);
            /* 按 dst 变量现有类型选择 STORE 指令 */
            int dst_idx = c_find_var(c, names[i]);
            if(dst_idx < 0) dst_idx = c_add_var(c, names[i], EXPR_TYPE_NONE);
            int dst_bf = bf_sym(c->fn, names[i]);
            CastKind dst_ck = (dst_bf >= 0 && dst_bf < c->fn->sym_cnt)
                              ? (CastKind)c->fn->var_type_tags[dst_bf] : CAST_NONE;
            ExprType dst_et = castkind_to_exprtype(dst_ck);
            if(dst_et == EXPR_TYPE_INT) {
                emit(c, OPC_UNBOX_INT64, (int)dst_ck, 0);   /* VALUE -> INT64 栈 */
                emit(c, OPC_STORE_INT64_VAR, dst_idx, 0);
            } else if(dst_et == EXPR_TYPE_DOUBLE) {
                emit(c, OPC_UNBOX_DOUBLE, 0, 0);            /* VALUE -> DOUBLE 栈 */
                emit(c, OPC_STORE_DOUBLE_VAR, dst_idx, 0);
            } else if(dst_et == EXPR_TYPE_PTR) {
                emit(c, OPC_UNBOX_PTR, (int)dst_ck, 0);     /* VALUE -> PTR 栈 */
                emit(c, OPC_STORE_PTR_VAR, dst_idx, 0);
            } else {
                /* 动态：VALUE 栈直接存 */
                if(dst_bf >= 0 && dst_bf < c->fn->sym_cnt) c->fn->var_type_tags[dst_bf] = -1;
                emit(c, OPC_STORE_VAR, dst_idx, 0);
            }
        }
        break;
    }

    case AST_UNARY:
    case AST_BINOP:
    case AST_TERNARY:
    case AST_CAST: {
        /* 表达式语句：编译并丢弃结果（POP 按返回栈路由） */
        ExprType et = c_expr(c, node);
        int pop_sel = 0;
        if(et == EXPR_TYPE_INT) pop_sel = 1;
        else if(et == EXPR_TYPE_DOUBLE) pop_sel = 2;
        else if(et == EXPR_TYPE_PTR) pop_sel = 3;
        emit(c, OPC_POP, pop_sel, 0);
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
                    int vi = c_add_var(c, cl[i].var, EXPR_TYPE_NONE);
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

    case AST_DEFER: {
        /* defer { body }：不立即编译，把 body 压入函数级 LIFO 栈。
         * 在 AST_RETURN / fallthrough / 异常 handler 处由 emit_deferred 统一执行。 */
        push_defer(c, node->u.defer.body);
        c->has_defer = 1;
        break;
    }

    case AST_RETURN: {
        /* 返回语句：无值 RETURN_NIL；有值 c_expr 后 RETURN（a=返回 ExprType）。
           在 try-finally 内：编译为 PEND_RETURN，先执行 finally 再真正返回。
           defer 处理：函数级 has_defer=1 时，return 前先 LIFO 执行所有 deferred。 */
        AstNode* rv = node->u.ret.ret_val;

        if(c->fin_depth > 0) {
            int pr;
            if(!rv) {
                /* 无值返回：先 emit_deferred 再 PEND_RETURN，让外层 finally 与 defer 都执行 */
                emit_deferred(c);
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
                /* 值已 box 到 VALUE 栈，再 LIFO 执行 defer（可能修改 self.field/全局；
                   不影响已压栈的返回值），最后 PEND_RETURN */
                emit_deferred(c);
                pr = emit_here(c, OPC_PEND_RETURN, 0, 0);
            }
            fin_add_pend(c, pr);
            break;
        }

        if(!rv) {
            /* 无值返回：先 LIFO 执行 defer，再 RETURN_NIL */
            emit_deferred(c);
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
                /* 标量类型：按标注类型 cast，发 typed RETURN（INT64/DOUBLE/PTR 栈） */
                emit_value_cast(c, vt, declared);
                target = declared;
                ret_ck = tck;
            } else {
                /* array/map/typed_array 等容器类型：callsite ret_stack=NONE（VALUE 栈），
                 * 需把 typed 栈值装箱到 VALUE 栈，使 RETURN 弹栈与 push_call_result 一致 */
                emit_to_dynamic(c, target, ret_ck);
                target = EXPR_TYPE_NONE;
                ret_ck = CAST_NONE;
            }
        } else {
            /* 无返回标注：返回值统一 box 到 VALUE 栈，使 callsite（ret_stack=NONE）一致 */
            emit_to_dynamic(c, target, ret_ck);
            target = EXPR_TYPE_NONE;
            ret_ck = CAST_NONE;
        }
        /* 返回值已 box 到 VALUE 栈，再 LIFO 执行 defer，最后 RETURN */
        emit_deferred(c);
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
        /* break：直接 JMP 出口；在 try-finally 内则 FIN_PUSH(BREAK) 先执行 finally 再跳。
         * 若 break 带 label，向上搜 layer 栈找匹配标签的循环层。 */
        const char* lbl = node->u.jump.label;
        Layer* L = layer_find_labeled(c, lbl);
        if(!L) {
            if(lbl) fprintf(stderr, "IR: break label '%s' not found\n", lbl);
            else    fprintf(stderr, "IR: break outside loop\n");
            break;
        }
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
        /* continue：跳 cond/更新头；try-finally 内 FIN_PUSH(CONT) 先执行 finally 再跳。
         * 若 continue 带 label，向上搜 layer 栈找匹配标签的循环层。 */
        const char* lbl = node->u.jump.label;
        Layer* L = layer_find_labeled(c, lbl);
        if(!L) {
            if(lbl) fprintf(stderr, "IR: continue label '%s' not found\n", lbl);
            else    fprintf(stderr, "IR: continue outside loop\n");
            break;
        }
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
        layer_push(c, 0, node->u.while_node.label);
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
        layer_push(c, 0, node->u.while_node.label);
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
        layer_push(c, 0, node->u.for_node.label);
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

    case AST_SWITCH: {
        /* switch (cond) { case v: stmts; break; ... default: stmts; }
         * 实现：把 cond 存到临时变量，每个 case 用 ast_binop(OP_EQ, sw_var, const_val) 作 cond，
         * 复用 emit_cond_jump_if_false 生成 if-elseif 链；default 放在末尾；break 走 layer */
        AstNode* cond = node->u.sw.cond;
        AstNode* cases = node->u.sw.cases;

        static int sw_counter = 0;
        char sw_var[64];
        snprintf(sw_var, sizeof(sw_var), "__sw_val_%d", sw_counter++);

        ExprType ct = c_expr(c, cond);
        CastKind var_ck = c_expr_cast_type(c, cond);
        int var_idx = c_add_var(c, sw_var, ct);
        int bf_idx = bf_sym(c->fn, sw_var);
        c->fn->var_type_tags[bf_idx] = (int)var_ck;
        if(ct == EXPR_TYPE_INT) emit(c, OPC_STORE_INT64_VAR, var_idx, 0);
        else if(ct == EXPR_TYPE_DOUBLE) emit(c, OPC_STORE_DOUBLE_VAR, var_idx, 0);
        else if(ct == EXPR_TYPE_PTR) emit(c, OPC_STORE_PTR_VAR, var_idx, 0);
        else emit(c, OPC_STORE_VAR, var_idx, 0);

        /* 收集 case 节点（链表）和 default */
        AstNode** case_arr = (AstNode**)malloc(sizeof(AstNode*) * 64);
        int ncases = 0;
        AstNode* default_case = NULL;
        for(AstNode* p = cases; p; p = p->u.cs.next) {
            if(p->u.cs.is_default) default_case = p;
            else if(ncases < 64) case_arr[ncases++] = p;
        }

        layer_push(c, 1, NULL);   /* switch 层（break 跳出） */
        Layer* L = layer_top(c);

        int* end_jmps = (int*)malloc(sizeof(int) * (ncases + 1));
        int end_cnt = 0;

        for(int i = 0; i < ncases; i++) {
            AstNode* cs = case_arr[i];
            /* 条件：__sw_val <op> const_val
             * 用 ast_binop 构造等价表达式，让 emit_cond_jump_if_false 自动按栈类型选择 EQ/VEQ */
            AstNode* var_node = ast_var(strdup(sw_var));
            AstNode* cond_eq;
            if(cs->u.cs.is_type_match) {
                /* 类型匹配 case int: / case string: / case bool: / case char: / case double:
                 * type() 返回精确类型名（int64/int8/string/bool/double 等）
                 * case int: 应匹配所有整数族；用 strncmp 前缀比较实现
                 *   case int:    type 字符串以 "int" 或 "uint" 或 "long" 或 "short" 或 "byte" 或 "size" 或 "ssize" 或 "char" 或 "bool" 或 "uchar" 开头
                 *   case double: type == "double" 或 "float" 或 "long double"
                 *   case string: type == "string"
                 *   case bool:   type == "bool"
                 *   case char:   type == "char"
                 * 简化实现：直接用精确 == 比较（要求 case 关键字匹配 type() 实际返回名） */
                AstNode* type_call = ast_call(strdup("type"), ast_seq(ast_var(strdup(sw_var)), NULL));
                /* 精确匹配 type() 返回的字符串。
                 * 字面量整数 (42) 实际压 INT64 栈 → type() 返回 "int64"
                 * 字面量浮点 (3.14) 实际压 DOUBLE 栈 → type() 返回 "double"
                 * 字面量 true/false → INT64 栈 → type() 返回 "int64"（不是 "bool"！）
                 * 字面量 'A' → INT64 栈 → type() 返回 "int64"（不是 "char"！）
                 * 字面量 "abc" → PTR 栈 → type() 返回 "string"
                 * case int:    → type() == "int64"
                 * case double: → type() == "double"
                 * case string: → type() == "string"
                 * case bool:   → type() == "bool"（仅当变量已声明为 bool 类型时）
                 * case char:   → type() == "char"（仅当变量已声明为 char 类型时）
                 */
                const char* tn = "int64";
                switch(cs->u.cs.match_type) {
                    case VAL_INT:    tn = "int64"; break;   /* 字面量整数实际压 INT64 栈 */
                    case VAL_DOUBLE: tn = "double"; break;
                    case VAL_BOOL:   tn = "bool"; break;
                    case VAL_CHAR:   tn = "char"; break;
                    case VAL_STRING: tn = "string"; break;
                    default: tn = "__unknown__"; break;
                }
                cond_eq = ast_binop(OP_EQ, type_call, ast_string(strdup(tn)));
            } else if(cs->u.cs.bind_var) {
                /* 模式绑定 case x: 暂不支持（编译为不匹配） */
                cond_eq = ast_bool(0);
            } else {
                cond_eq = ast_binop(OP_EQ, var_node, ast_clone_node(cs->u.cs.const_val));
            }
            int jf = emit_cond_jump_if_false(c, cond_eq);
            c_stmt(c, cs->u.cs.body);
            /* fall-through（无 break）跳到 switch 末尾 */
            end_jmps[end_cnt++] = emit_here(c, OPC_JMP, 0, 0);
            patch_to(c, jf);
        }

        /* default：放在所有 case 测试之后 */
        if(default_case) {
            c_stmt(c, default_case->u.cs.body);
        }

        /* end: patch 所有 fall-through 与 break */
        for(int i = 0; i < end_cnt; i++) {
            patch_to(c, end_jmps[i]);
        }
        patch_list_here(c, L->brk, L->brk_cnt);
        patch_fin_list_here(c, L->brk_fin, L->brk_fin_cnt);
        layer_pop(c);

        free(case_arr);
        free(end_jmps);
        break;
    }

    /* 函数声明节点：顶层定义已由 func_compile 流程独立编译，main 顺序流中跳过。
     * 嵌套函数定义将在闭包任务（Task 12）中在此真正处理。 */
    case AST_FUNC_DEF:
    case AST_EXTERN_FUNC:
    case AST_PARAM:
    case AST_NONE:   /* no-op 语句（struct/class 声明注册后返回 ast_none()） */
        break;

    /* yield 表达式（生成器函数体内）：编译 yield 值到 VALUE 栈，emit OPC_YIELD。
     * 无值 yield;（仅恢复控制流）：emit OPC_YIELD（值压入 NONE）。 */
    case AST_YIELD: {
        AstNode* v = node->u.yieldnode.value;
        if(v) {
            ExprType vt = c_expr(c, v);
            /* yield 值统一 box 到 VALUE 栈（动态返回给 next() 调用方） */
            CastKind ck = c_expr_cast_type(c, v);
            emit_to_dynamic(c, vt, ck);
        } else {
            /* 无值 yield：压入 NONE */
            Value none; none.type = VAL_NONE; none.v.i = 0;
            (void)none;  /* 编译期不需要常量，OPC_YIELD 自己处理空 yield */
        }
        emit(c, OPC_YIELD, v ? 1 : 0, 0);  /* a=是否有 yield 值 */
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
    if(0==strcmp(n,"map"))         return CAST_MAP;
    if(0==strcmp(n,"array"))       return CAST_ARRAY;
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
    int idx = c->var_cnt++;
    c->var_names[idx] = strdup(name);
    c->var_types[idx] = t;
    /* 形参直接写 var_names（不经 c_add_var），须同步哈希 */
    symhash_insert(&c->var_idx, c->var_names, c->var_cnt, idx);
}

/* 释放编译上下文动态表（函数编译在 parse 期多次发生，避免 strdup 泄漏） */
static void ctx_cleanup(Ctx* c) {
    for(int i = 0; i < c->var_cnt; i++) free(c->var_names[i]);
    free(c->var_names);
    free(c->var_types);
    symhash_reset(&c->var_idx);
    c->var_names = NULL; c->var_types = NULL;
    c->var_cnt = c->var_cap = 0;
    /* defer 栈：只释放指针数组（body 节点由 AST 全局释放，不重复 free） */
    free(c->deferred);
    c->deferred = NULL;
    c->deferred_cnt = c->deferred_cap = 0;
}

/* 前置声明（重载组定义在后文） */
static void ol_add(BytecodeFunc* fn, AstNode* params, int* is_first_out);
static void ol_insert_bare_alias(BytecodeFunc* fn);

/* 编译函数 / Compile a lumin function into bytecode and register it */
BytecodeFunc* ir_compile_function(const char* name, AstNode* params, AstNode* body, int is_generator, const char* class_name, const char* ret_type_name) {
    /* 方法唯一内部名：<属主>__m__<方法>，避免不同 struct/class 的同名方法
     * 在全局函数表互相覆盖（分派必须通过接收者类型）。
     * 构造函数名 <Class>___init__ 已唯一保持；普通函数 class_name==NULL 保持原名 */
    const char* reg_name = name;
    char* internal_name = NULL;
    if(class_name && name) {
        size_t nl = strlen(name);
        int is_ctor = (nl >= 9 && strcmp(name + nl - 9, "___init__") == 0);
        if(!is_ctor) {
            size_t need = strlen(class_name) + nl + 6;  /* "__m__"(4) + \0 */
            internal_name = (char*)malloc(need);
            if(internal_name) {
                snprintf(internal_name, need, "%s__m__%s", class_name, name);
                reg_name = internal_name;
            }
        }
    }
    BytecodeFunc* fn = bytecode_func_new(reg_name, 0);
    free(internal_name);  /* bytecode_func_new 已 strdup */
    fn->is_generator = is_generator ? 1 : 0;
    fn->class_name = class_name ? strdup(class_name) : NULL;
    fn->ret_type_name = ret_type_name ? strdup(ret_type_name) : NULL;

    /* 自由函数（非 lambda/arrow/ctor）：分配唯一注册键以支持重载同名多版本；
     * fn->name 保留源名用于显示，实际入表键为 __ol__<name>__<seq> */
    int is_free_for_overload = 0;
    if(!class_name && name) {
        if(strncmp(name, "_lambda_", 8) != 0 &&
           strncmp(name, "_arrow_", 7) != 0 &&
           !(strlen(name) >= 9 && strcmp(name + strlen(name) - 9, "___init__") == 0)) {
            is_free_for_overload = 1;
            size_t need = strlen(name) + 16;
            char* key = (char*)malloc(need);
            snprintf(key, need, "__ol__%s__%d", name, g_ol_seq++);
            fn->table_key = key;
        }
    }

    /* 编译上下文：形参按声明序注册（Ctx idx == bf 槽位 == frame 槽位） */
    Ctx c;
    memset(&c, 0, sizeof(Ctx));
    c.fn = fn;
    c.cur_params = params;

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
        /* 特例：self 参数约束为自定义类型名时，struct_register 可能尚未调用
         * （struct 方法在 struct 定义体内即编译），此时 pck=CAST_NONE。
         * 但约束名实际就是 struct/class 名 → 视为 STRUCT_PTR/CLASS_PTR。
         * 这里仅设置 method_self_struct 和 var_type_tags，让后续 LOAD_FIELD 走 fast path */
        int is_self_struct = (strcmp(p->u.param.name, "self") == 0 &&
                              p->u.param.constraint &&
                              pck == CAST_NONE);
        if(is_self_struct) {
            /* 约束名是自定义类型（struct/class），无法此时确定是 struct 还是 class，
             * 统一标记为 CAST_STRUCT_PTR（IR 端 STRUCT_PTR 和 CLASS_PTR 行为一致） */
            pck = CAST_STRUCT_PTR;
            fn->var_type_tags[slot] = (int)pck;
            if(!fn->method_self_struct) fn->method_self_struct = strdup(p->u.param.constraint);
        } else {
            if(pck != CAST_NONE) fn->var_type_tags[slot] = (int)pck;
            /* 若是 self 参数且已识别为 struct/class：也设 method_self_struct */
            if(strcmp(p->u.param.name, "self") == 0 && p->u.param.constraint &&
               (pck == CAST_STRUCT_PTR || pck == CAST_CLASS_PTR)) {
                if(!fn->method_self_struct) fn->method_self_struct = strdup(p->u.param.constraint);
            }
        }
        ExprType et = castkind_to_exprtype(pck);   /* 无标注 → NONE（动态） */
        ctx_register_param(&c, p->u.param.name, et);
        if(p->u.param.is_ellipsis) fn->has_variadic = 1;
        else fn->param_cnt++;
        total++;
    }

    /* 先注册再编译函数体：使函数体内的自引用递归调用能查到自身。
       形参类型与返回标注此时已就绪；递归 CALL 真正运行时函数体已编译完整。 */
    ir_func_table_register(fn);

    /* 登记进重载组；首版本额外以裸名建别名（兼容函数值/裸名路径） */
    if(is_free_for_overload) {
        int first = 0;
        ol_add(fn, params, &first);
        if(first) ol_insert_bare_alias(fn);
    }

    /* lambda / arrow：注册捕获的外层变量为本地槽位（type NONE，VALUE 栈访问）。
     * 箭头函数 _arrow_N 与匿名函数 _lambda_N 共用同一闭包捕获机制 */
    if(name && (strncmp(name, "_lambda_", 8) == 0 ||
                strncmp(name, "_arrow_", 7) == 0)) {
        int ncap = lambda_capture_count(name);
        for(int i = 0; i < ncap; i++) {
            const char* cname = lambda_capture_name(name, i);
            if(cname) c_add_var(&c, cname, EXPR_TYPE_NONE);
        }
    }

    /* 预扫描函数体是否含 AST_DEFER；若有，发函数级 OPC_TRY 包裹整个 body，
     * 让 throw 路径也能触发 defer（catch handler 跑完 emit_deferred 后重抛） */
    int has_defer = body ? ast_has_defer(body) : 0;
    int try_pos = -1;
    if(has_defer) {
        try_pos = emit_here(&c, OPC_TRY, 0, 0);   /* a=catch_pc（后回填），b=0（无 finally） */
        c.has_defer = 1;
    }

    /* 编译函数体；末尾隐式返回 null（显式 return 时该指令不可达，无害）。
     * 若 has_defer，fallthrough 路径需先 LIFO 执行所有 deferred 再 ENDTRY+RETURN_NIL。 */
    if(body) c_stmt(&c, body);
    if(has_defer) {
        /* fallthrough 路径：先 emit_deferred（LIFO）然后弹 try 后隐式 return nil */
        emit_deferred(&c);
        emit(&c, OPC_ENDTRY, 0, 0);
    }
    emit(&c, OPC_RETURN_NIL, 0, 0);

    /* 异常 handler：emit_deferred + GET_ERR + THROW 重抛到外层 */
    if(has_defer) {
        int handler_pc = c.fn->code_len;
        /* 重新 emit_deferred（编译期重复一次，对应异常运行时路径） */
        emit_deferred(&c);
        emit(&c, OPC_GET_ERR, 0, 0);
        emit(&c, OPC_THROW, 0, 0);
        /* 回填 OPC_TRY 的 catch_pc */
        c.fn->code[try_pos].a = handler_pc;
    }

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

/* 注册/替换函数（key 取 fn->table_key 否则 name）；同键旧函数被释放 */
void ir_func_table_register(BytecodeFunc* fn) {
    if(!fn || !fn->name) return;
    const char* key = fn->table_key ? fn->table_key : fn->name;
    RBTree* t = func_table_tree();
    void* old = rbtree_set_data(t, NS_FUNCTION, NULL, key, fn);
    if(old) {
        if(old != fn) bytecode_func_free((BytecodeFunc*)old);
    } else {
        rbtree_insert(t, NS_FUNCTION, NULL, key, fn);
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

/* ============================================================
 * 自由函数重载组（按 arity + 形参声明类型；仅自由函数）
 * 每个版本以唯一键 __ol__<name>__<seq> 注册进全局函数表，避免同名覆盖；
 * 调用点 ol_resolve 评分选最佳版本，再走 compile_user_call。
 * ============================================================ */
typedef struct {
    BytecodeFunc* fn;
    AstNode def_shell;     /* AST_FUNC_DEF，供 compile_user_call 取 params */
} OlCand;
typedef struct {
    char* name;
    int cnt, cap;
    OlCand* cands;
} OlGroup;

static OlGroup* g_ol = NULL;
static int g_ol_n = 0, g_ol_cap = 0;
/* g_ol_seq 已在文件前部定义 */

static OlGroup* ol_find_group(const char* name) {
    for(int i = 0; i < g_ol_n; i++)
        if(strcmp(g_ol[i].name, name) == 0) return &g_ol[i];
    return NULL;
}

static int ol_group_exists(const char* name) { return ol_find_group(name) != NULL; }

/* 分配/登记一个重载版本；first=该组首版本（额外以裸名建别名，兼容函数值/其它裸名查找） */
static void ol_add(BytecodeFunc* fn, AstNode* params, int* is_first_out) {
    OlGroup* g = ol_find_group(fn->name);
    int is_first = 0;
    if(!g) {
        if(g_ol_n >= g_ol_cap) {
            g_ol_cap = g_ol_cap ? g_ol_cap * 2 : 16;
            g_ol = (OlGroup*)realloc(g_ol, sizeof(OlGroup) * (size_t)g_ol_cap);
        }
        g = &g_ol[g_ol_n++];
        g->name = strdup(fn->name);
        g->cnt = 0; g->cap = 0; g->cands = NULL;
        is_first = 1;
    }
    /* 去重：重编译（func_compile_recompile）会产生同一函数的新 BytecodeFunc，
     * 替换旧候选而非追加，避免重复计为歧义 */
    for(int i = 0; i < g->cnt; i++) {
        if(g->cands[i].fn->param_cnt == fn->param_cnt &&
           g->cands[i].fn->has_variadic == fn->has_variadic) {
            /* 同 arity → 替换（重编译更新） */
            g->cands[i].fn = fn;
            memset(&g->cands[i].def_shell, 0, sizeof(AstNode));
            g->cands[i].def_shell.type = AST_FUNC_DEF;
            g->cands[i].def_shell.u.func_def.params = params;
            if(is_first_out) *is_first_out = is_first;
            return;
        }
    }
    if(g->cnt >= g->cap) {
        g->cap = g->cap ? g->cap * 2 : 4;
        g->cands = (OlCand*)realloc(g->cands, sizeof(OlCand) * (size_t)g->cap);
    }
    OlCand* c = &g->cands[g->cnt++];
    c->fn = fn;
    memset(&c->def_shell, 0, sizeof(AstNode));
    c->def_shell.type = AST_FUNC_DEF;
    c->def_shell.u.func_def.params = params;
    if(is_first_out) *is_first_out = is_first;
}

/* 给裸名插入别名（仅当不存在），使函数值引用等裸名路径仍可用（返回首版本） */
static void ol_insert_bare_alias(BytecodeFunc* fn) {
    RBTree* t = func_table_tree();
    if(!rbtree_find(t, NS_FUNCTION, NULL, fn->name))
        rbtree_insert(t, NS_FUNCTION, NULL, fn->name, fn);
}

/* 类型族：1=整数族 2=浮点族 3=对象/指针族 0=动态 */
static int ck_family(CastKind k) {
    switch(k) {
        case CAST_INT: case CAST_BOOL: case CAST_ASCII: case CAST_CHAR:
        case CAST_BYTE: case CAST_INT8: case CAST_INT16: case CAST_INT32:
        case CAST_INT64: case CAST_UINT8: case CAST_UINT16: case CAST_UINT32:
        case CAST_UINT: case CAST_UINT64: case CAST_LONG: case CAST_LONGLONG:
        case CAST_ULONG: case CAST_UCHAR: case CAST_SHORT: case CAST_USHORT:
        case CAST_SIZE_T: case CAST_SSIZE_T: case CAST_PTR:
            return 1;
        case CAST_DOUBLE: case CAST_FLOAT: case CAST_LONG_DOUBLE:
            return 2;
        case CAST_NONE:
            return 0;
        default:
            return 3;
    }
}

/* 单个形参位的匹配评分：越小越优 */
static int ol_slot_score(CastKind pck, CastKind ack) {
    if(pck == CAST_NONE) return 2;                 /* 形参动态：宽松，降权 */
    if(ack == CAST_NONE) return 1;                 /* 实参动态、形参有类型：运行时校验 */
    if(ack == pck) return 0;                       /* 精确匹配 */
    if(ck_family(ack) == ck_family(pck)) return 1; /* 同族可转换 */
    return 6;                                      /* 跨族：勉强/差匹配 */
}

/* ol_resolve 结果状态 */
#define OL_OK 0
#define OL_NOMATCH 1
#define OL_AMBIG 2

/* 解析重载：对 args（文本序，命名实参取其值节点）按 arity 可行 + 类型评分选唯一最佳。
 * 返回选中的候选下标（OL_OK），否则 *status 为 NOMATCH/AMBIG。 */
static int ol_resolve(Ctx* c, const char* name, AstNode* args, int* status) {
    *status = OL_OK;
    OlGroup* g = ol_find_group(name);
    if(!g || g->cnt == 0) { *status = OL_NOMATCH; return -1; }

    /* 收集实参；AST_ASSIGN 取其值节点（命名实参参与类型评分，最终仍按名绑定） */
    int ac = 0, acap = 0;
    AstNode** raw = NULL;
    collect_call_args(args, &raw, &ac, &acap);
    CastKind* ak = (CastKind*)malloc(sizeof(CastKind) * (size_t)(ac > 0 ? ac : 1));
    for(int i = 0; i < ac; i++) {
        AstNode* node = raw[i];
        if(node->type == AST_ASSIGN) node = node->u.assign.expr;
        ak[i] = c_expr_cast_type(c, node);
    }

    int best = -1, best_score = 0;
    int ties = 0;
    for(int ci = 0; ci < g->cnt; ci++) {
        BytecodeFunc* fn = g->cands[ci].fn;
        /* 统计 required（无默认）与形参类型 */
        int required = 0, slot = 0;
        for(AstNode* p = g->cands[ci].def_shell.u.func_def.params; p;
            p = p->u.param.next, slot++) {
            if(p->u.param.is_ellipsis) break;
            if(!p->u.param.default_val) required++;
        }
        int variadic = fn->has_variadic;
        int pcnt = fn->param_cnt;
        /* arity 可行：实参数 >= 必填，且非可变时 <= 形参数 */
        if(ac < required) continue;
        if(!variadic && ac > pcnt) continue;

        int score = 0;
        for(int i = 0; i < ac; i++) {
            CastKind pck = CAST_NONE;
            if(i < pcnt && i < fn->sym_cnt) {
                int tag = fn->var_type_tags[i];
                pck = (tag >= 0) ? (CastKind)tag : CAST_NONE;  /* -1=无标注→动态 */
            }
            score += ol_slot_score(pck, ak[i]);
        }
        if(best < 0 || score < best_score) {
            best = ci; best_score = score; ties = 1;
        } else if(score == best_score) {
            ties++;
        }
    }
    free(ak);
    free(raw);
    if(best < 0) { *status = OL_NOMATCH; return -1; }
    if(ties > 1) { *status = OL_AMBIG; return best; }
    return best;
}

/* 供调用路径统一解析自由函数：成功返回候选（*fn_out/*def_out），失败按 status 报错并 exit1 */
static int resolve_free_call(Ctx* c, const char* name, AstNode* args,
                             BytecodeFunc** fn_out, AstNode** def_out) {
    int status = OL_OK;
    int ci = ol_resolve(c, name, args, &status);
    OlGroup* g = ol_find_group(name);
    if(status == OL_NOMATCH) {
        fprintf(stderr, "IR: 没有匹配函数 %s 的重载版本\n", name);
        return 0;
    }
    if(status == OL_AMBIG) {
        fprintf(stderr, "IR: 调用 %s 存在多个同样匹配的重载，存在歧义\n", name);
        return 0;
    }
    *fn_out = g->cands[ci].fn;
    *def_out = &g->cands[ci].def_shell;
    return 1;
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
