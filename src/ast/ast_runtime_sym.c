/*
 * ast_runtime_sym.c - 运行时符号表（红黑树实现）
 * 键为 (namespace, class_name, name)，支持函数/变量/类方法
 */
#include "ast_runtime_sym.h"
#include "ir/rbtree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 符号表条目 */
typedef struct {
    char* name;
    char* class_name;
    Value value;
} SymEntry;

/* 全局符号表 */
static RBTree* g_sym_tree = NULL;
static const char* g_current_file_name = NULL;

/* 初始化符号表 */
void sym_init(void)
{
    if(!g_sym_tree) {
        g_sym_tree = rbtree_create();
    }
}

/* 清空符号表 */
void sym_clear(void)
{
    if(g_sym_tree) {
        rbtree_destroy(g_sym_tree);
        g_sym_tree = NULL;
    }
}

/* 设置当前文件名 */
void sym_set_current_file(const char* file_name)
{
    g_current_file_name = file_name;
}

/* 获取当前文件名 */
const char* sym_get_current_file(void)
{
    return g_current_file_name;
}

/* 设置符号 */
void sym_set(const char* n, Value v)
{
    sym_init();
    SymEntry* entry = (SymEntry*)malloc(sizeof(SymEntry));
    entry->name = strdup(n);
    entry->class_name = NULL;
    entry->value = v;
    rbtree_insert(g_sym_tree, NS_VARIABLE, NULL, n, entry);
}

/* 设置类符号 */
void sym_set_class(const char* class_name, const char* n, Value v)
{
    sym_init();
    SymEntry* entry = (SymEntry*)malloc(sizeof(SymEntry));
    entry->name = strdup(n);
    entry->class_name = strdup(class_name);
    entry->value = v;
    rbtree_insert(g_sym_tree, NS_METHOD, class_name, n, entry);
}

/* 获取符号 */
Value sym_get(const char* n)
{
    if(!g_sym_tree) return (Value){.type=VAL_NONE};
    SymEntry* entry = (SymEntry*)rbtree_find(g_sym_tree, NS_VARIABLE, NULL, n);
    if(entry) return entry->value;
    return (Value){.type=VAL_NONE};
}

/* 获取类符号 */
Value sym_get_class(const char* class_name, const char* n)
{
    if(!g_sym_tree) return (Value){.type=VAL_NONE};
    SymEntry* entry = (SymEntry*)rbtree_find(g_sym_tree, NS_METHOD, class_name, n);
    if(entry) return entry->value;
    return (Value){.type=VAL_NONE};
}

/* 获取符号指针 */
Value* sym_get_ptr(const char* n)
{
    if(!g_sym_tree) return NULL;
    SymEntry* entry = (SymEntry*)rbtree_find(g_sym_tree, NS_VARIABLE, NULL, n);
    if(entry) return &entry->value;
    return NULL;
}

/* 检查符号是否存在 */
_Bool sym_has(const char* n)
{
    if(!g_sym_tree) return 0;
    return rbtree_find(g_sym_tree, NS_VARIABLE, NULL, n) != NULL;
}

/* 检查类符号是否存在 */
_Bool sym_has_class(const char* class_name, const char* n)
{
    if(!g_sym_tree) return 0;
    return rbtree_find(g_sym_tree, NS_METHOD, class_name, n) != NULL;
}

/* 删除符号 */
void sym_del(const char* n)
{
    if(!g_sym_tree) return;
    rbtree_delete(g_sym_tree, NS_VARIABLE, NULL, n);
}

/* 工具函数：值转数字 */
double val_to_num(Value v)
{
    switch(v.type) {
        case VAL_INT: return (double)v.v.i;
        case VAL_DOUBLE: return v.v.d;
        case VAL_BOOL: return v.v.b ? 1.0 : 0.0;
        case VAL_CHAR: return (double)v.v.c;
        default: return 0.0;
    }
}

/* 工具函数：值比较 */
int value_equal(Value a, Value b)
{
    if(a.type != b.type) return 0;
    switch(a.type) {
        case VAL_INT: return a.v.i == b.v.i;
        case VAL_DOUBLE: return a.v.d == b.v.d;
        case VAL_BOOL: return a.v.b == b.v.b;
        case VAL_CHAR: return a.v.c == b.v.c;
        case VAL_STRING: return strcmp(a.v.s, b.v.s) == 0;
        default: return 0;
    }
}

/* 工具函数：字符串拼接 */
char* lumyr_concat(const char* s1, const char* s2)
{
    int len1 = strlen(s1);
    int len2 = strlen(s2);
    char* result = (char*)malloc(len1 + len2 + 1);
    memcpy(result, s1, len1);
    memcpy(result + len1, s2, len2);
    result[len1 + len2] = '\0';
    return result;
}
