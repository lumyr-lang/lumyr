//
// Created by kai on 2026/9/5.
//

#ifndef AST_NODE_TYPE_H
#define AST_NODE_TYPE_H
#include "lumyr_types.h"
#include "lumyr_value_type.h"

typedef struct AstNode AstNode;

/* catch 子句：支持按类型捕获和多个 catch 块 */
typedef struct {
    char* type;       /* 异常类型名（可为 NULL：捕获所有异常） */
    char* var;        /* 异常变量名 */
    AstNode* body;    /* catch 块 */
} CatchClause;

struct AstNode {
    AstType type;
    ValueType val_type;  // 该节点表达式的类型，语义分析后填充
    int line;            // 源码行号（lexer yylineno，解析时填充；报错定位用）
    CastKind lit_cast;   // 字面量后缀精确类型（AST_INT/AST_NUM 专用；CAST_NONE=按值推断）
    union {
        long long inum;           // AST_INT
        double num;               // AST_NUM
        _Bool bval;               // AST_BOOL
        char ch;
        char* sval;               // AST_STRING，字符串字面量，strdup堆分配
        char* varname;            // AST_VAR
        struct {
            BinOp op;
            AstNode* left;
            AstNode* right;
        } bin;
        struct {
            BinOp op;             // OpCode，复用BinOp别名
            AstNode* child;
        } uny;
        struct {
            char* varname;
            AstNode* expr;
            int is_const;   /* const 声明：1=初始化后不可重新赋值 */
        } assign;
        struct {
            AstNode* args;  /* 参数列表，AST_SEQ 链接；单参数时为单个表达式节点 */
        } print;
        struct {
            AstNode* first;
            AstNode* second;
        } seq;
        struct {
            AstNode* cond;        // if条件
            AstNode* then_stmt;   // if分支
            AstNode* elif_chain;  // elseif链：AST_IF链表
            AstNode* else_stmt;   // else分支，可以NULL
        } ifnode;
        struct {
            AstNode* stmts;
        } block;
        struct {
            AstNode* cond;
            AstNode* if_body;
            AstNode* elif_list;   // AST_ELIF* 链表
            AstNode* else_body;
        } if_chain;
        struct {
            AstNode* cond;
            AstNode* body;
            AstNode* next;
        } elif;
        struct {
            AstNode* cond;
            AstNode* body;
            char* label;   /* 标签名（labeled break/continue），NULL=无标签 */
        } while_node;
        struct {
            char* name;
            AstNode* args;
        } annotation;
        struct {
            AstNode* obj;
            char* method;
            AstNode* args;
        } safe_call;
        struct {
            AstNode* left;
            AstNode* right;
        } null_coalesce;
        struct {
            char* name;
            AstNode* params;  // AST_PARAM 链表
            AstNode* body;    // 宏体（block_stmt）
        } macro_def;
        struct {
            AstNode* init;
            AstNode* cond;
            AstNode* update;
            AstNode* body;
            char* label;   /* 标签名（labeled break/continue），NULL=无标签 */
        } for_node;
        struct {
            CastKind cast_type;
            struct AstNode* child;
        } cast;
        struct {
            AstNode* ret_val;
        } ret;
        struct {
            char* label;   /* 标签名（labeled break/continue），NULL=无标签 */
        } jump;
        struct {
            AstNode* cond;
            AstNode* true_expr;
            AstNode* false_expr;
        } ternary;
        struct {
            AstNode* cond;
            AstNode* cases;
        } sw;
        struct {
            AstNode* const_val;
            AstNode* body;
            int is_default;
            AstNode* next;
            int is_type_match;   /* type match: case int: / case string: etc */
            int match_type;      /* matched ValueType */
            char* bind_var;      /* pattern binding: case x: (NULL = no binding) */
            AstNode* guard;      /* pattern guard: case x if cond: (NULL = no guard) */
        } cs;

        struct {
            char* name;
            AstNode* params;   // AST_PARAM链表
            AstNode* body;     // 函数体 block
            AstNode* annotations;  // 注解列表（AST_SEQ 链，每个节点是 AST_ANNOTATION）
            int is_const;  // 编译期计算函数（const fn）
            AstNode* generic_params;  // 泛型参数列表（AST_PARAM链表，NULL=非泛型函数）
            int is_generator;  // generator function (gen func)
            char* ret_type_name;  // 返回值类型名（如 "int","double","string"），NULL=无类型声明
            int is_class_method;  // 1=class方法，0=普通函数
            int is_static_method;  // 1=静态方法，0=普通方法
            int access_modifier;  // 访问控制：0=public, 1=private, 2=protected
            int is_abstract_method;  // 1=抽象方法，0=普通方法
            int is_override_method;  // 1=重写方法，0=普通方法
            int is_getter;  // 1=getter方法，0=普通方法
            int is_setter;  // 1=setter方法，0=普通方法
        } func_def;

        struct {
            AstNode* value;  // yield value expression, NULL = bare yield
        } yieldnode;

        struct {
            char* name;          // 外部函数名
            AstNode* params;     // AST_PARAM链表（参数类型在 constraint 字段）
            char* ret_type_name; // 返回值类型名（如 "int","double","string","ptr","handle"）
            char* libname;       // 库名（NULL = 从当前进程符号表查找）
        } extern_func;

        struct {
            CastKind cast_type;       // 标注的类型（CastKind枚举），如 CAST_INT/CAST_DOUBLE/CAST_LONG
            AstNode* expr;       // 被标注的表达式
        } type_annotation;

        struct {
            char* interface_name; // 接口名
            AstNode* expr;        // 被标注的表达式
        } interface_annotation;

        struct {
            char* name;
            int is_ellipsis; // 1=...args可变参数，只能最后一个
            AstNode* default_val; // 默认值表达式，NULL=无默认值
            AstNode* next;   // 参数链表
            char* constraint; // 泛型参数类型约束（如 "Comparable"），NULL=无约束/普通函数参数
            int is_ref;      // 1=引用传递参数（ref p），0=值传递（默认）
            int is_nullable; // 1=可空类型 T?（允许 null），0=非空（默认，绑定/写入时校验非空）
        } param;

        struct {
            char* name;
            AstNode* args;   // 实参链表
        } call;

        struct {
            AstNode* callee; // 被调用的表达式（函数值，如 f(1) 的结果）
            AstNode* args;   // 实参链表
        } dyn_call;

        struct {
            AstNode* arr;    // 数组表达式
            AstNode* idx;    // 下标表达式
        } index;

        struct {
            AstNode* arr;    // 数组表达式
            AstNode* idx;    // 下标表达式
            AstNode* value;  // 赋值表达式
        } index_assign;

        struct {
            AstNode* elems;  // AST_SEQ 链：数组元素
            int elem_type;   // 数组元素类型（ValueType，-1 表示通用类型）
        } array_lit;
        struct {
            AstNode* entries;  // AST_SEQ 链：AST_MAP_ENTRY
        } map_lit;
        struct {
            AstNode* key;    // 键表达式（字符串）
            AstNode* value;  // 值表达式
        } map_entry;
        struct {
            AstNode* body;       // try 块
            char* catch_var;     // 兼容旧版：单个 catch 变量名（可为 NULL）
            AstNode* catch_body; // 兼容旧版：单个 catch 块（可为 NULL）
            CatchClause* catches; // 新版：多个 catch 子句（可为 NULL，此时用旧字段）
            int catch_count;     // catch 子句数量
            AstNode* finally_body; // finally 块（可为 NULL）
        } trynode;
        struct {
            AstNode* expr;       // throw 的值表达式
        } thrownode;
        struct {
            char** names;        // 解构变量名数组
            int count;           // 变量个数
            AstNode* rhs;        // 右值表达式（应为数组）
        } destruct;
        struct {
            AstNode* expr;       // 被展开的表达式（数组或 map）
        } spread;
        struct {
            char* class_name;    // class 名字符串
            int argc;            // 构造函数参数数量
            AstNode* args;       // 构造函数参数列表（AST_SEQ 树）
        } class_new;

        struct {
            AstNode* recv;       // 接收者表达式（编译后压 PTR 栈作为 self）
            char* method;        // 方法名（不含类前缀，运行时按 recv 实际类型分派）
            AstNode* args;       // 实参列表（AST_SEQ 链，不含 self）
        } method_call;

        /* 推导式：列表 [expr for x in iter (if cond)] / 字典 {k:v for x in iter (if cond)} */
        struct {
            AstNode* expr;       // 元素表达式（列表）或键表达式（字典）
            AstNode* value;       // 值表达式（字典专用；列表为 NULL）
            AstNode* var;         // 循环变量（AST_VAR）
            AstNode* iter;        // 可迭代对象
            AstNode* cond;        // 过滤条件（NULL=无过滤）
        } comp;
        struct {
            AstNode* body;        // defer 块体（AST_BLOCK）
        } defer;
    } u;
};


#endif //AST_NODE_TYPE_H
