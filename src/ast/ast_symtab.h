#ifndef AST_SYMTAB_H
#define AST_SYMTAB_H

#include "lumyr_types.h"

#define STATIC_SYM_INITIAL_CAP 128

typedef struct SymStaticEntry {
    char* name;
    ValueType ty;
    struct SymStaticEntry* next;  /* 符号栈，用于变量遮蔽 */
    int deleted;                  /* 逻辑删除标记（rbtree_delete 未实现，作用域恢复时置位） */
} SymStaticEntry;

/* 静态符号表：用红黑树存储 */
void static_sym_reset(void);
int static_sym_put(const char* name, ValueType ty);
int static_sym_get(const char* name, ValueType* out_ty);

/* 作用域保存/恢复 */
void static_sym_save(void);
void static_sym_restore(void);

#endif
