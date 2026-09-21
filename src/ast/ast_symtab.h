#ifndef AST_SYMTAB_H
#define AST_SYMTAB_H

#include "lumyr_types.h"

#define STATIC_SYM_INITIAL_CAP 128

typedef struct SymStaticEntry {
    char* name;
    ValueType ty;
    struct SymStaticEntry* next;  /* 符号栈，用于变量遮蔽 */
    int deleted;                  /* 逻辑删除标记（rbtree_delete 未实现，作用域恢复时置位） */
    int is_const;                 /* const 声明标记：1=不可重新赋值（const x = ..） */
} SymStaticEntry;

/* 静态符号表：用红黑树存储 */
void static_sym_reset(void);
int static_sym_put(const char* name, ValueType ty);
int static_sym_get(const char* name, ValueType* out_ty);

/* const 标记：put 登记名字后 set_const 标记为常量；is_const 查询是否不可重新赋值 */
void static_sym_set_const(const char* name);
int static_sym_is_const(const char* name);

/* 作用域保存/恢复 */
void static_sym_save(void);
void static_sym_restore(void);

/* 当前是否处于 save 的作用域（函数/lambda 内）；顶层返回 0 */
int static_sym_scope_active(void);
/* 名字是否已在当前作用域层级登记（新建或遮蔽）；用于区分首次定义与重复赋值 */
int static_sym_level_knows(const char* name);

#endif
