#include "ast_runtime_sym.h"
#include "ir/rbtree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 运行时符号表：红黑树存储，键为 (class_name, name)，class_name 为 NULL 表示普通符号 */
static RBTree* g_sym_tree = NULL;

/* 初始化符号表 */
void sym_init(void)
{
    if(!g_sym_tree) g_sym_tree = rbtree_create();
}

/* 清空符号表 */
void sym_clear(void)
{
    if(g_sym_tree) {
        rbtree_destroy(g_sym_tree);
        g_sym_tree = NULL;
    }
}

/* 确保符号表已初始化 */
static void sym_ensure_init(void)
{
    if(!g_sym_tree) sym_init();
}

/* 设置符号（class_name 为 NULL 表示普通符号） */
void sym_set_class(const char* class_name, const char* n, Value v)
{
    sym_ensure_init();
    /* 先查找是否已存在，如果存在则释放旧的 Value */
    Value* old_v = (Value*)rbtree_find(g_sym_tree, class_name, n);
    if(old_v) {
        if(old_v->type == VAL_STRING && !old_v->str_inline) {
            free(old_v->v.s);
        }
        *old_v = v;
        return;
    }
    /* 不存在则分配新的 Value 并插入 */
    Value* new_v = (Value*)malloc(sizeof(Value));
    *new_v = v;
    rbtree_insert(g_sym_tree, class_name, n, new_v);
}

/* 设置符号（普通符号） */
void sym_set(const char* n, Value v)
{
    sym_set_class(NULL, n, v);
}

/* 获取符号（class_name 为 NULL 表示普通符号） */
Value sym_get_class(const char* class_name, const char* n)
{
    sym_ensure_init();
    Value* v = (Value*)rbtree_find(g_sym_tree, class_name, n);
    if(!v) {
        fprintf(stderr,"未定义变量: %s\n",n);
        exit(EXIT_FAILURE);
    }
    return *v;
}

/* 获取符号（普通符号） */
Value sym_get(const char* n)
{
    return sym_get_class(NULL, n);
}

/* 获取符号指针（用于修改） */
Value* sym_get_ptr(const char* n)
{
    sym_ensure_init();
    Value* v = (Value*)rbtree_find(g_sym_tree, NULL, n);
    if(!v) {
        fprintf(stderr,"未定义变量: %s\n",n);
        exit(EXIT_FAILURE);
    }
    return v;
}

/* 检查符号是否存在（class_name 为 NULL 表示普通符号） */
_Bool sym_has_class(const char* class_name, const char* n)
{
    sym_ensure_init();
    return rbtree_find(g_sym_tree, class_name, n) != NULL;
}

/* 检查符号是否存在（普通符号） */
_Bool sym_has(const char* n)
{
    return sym_has_class(NULL, n);
}

/* 删除符号 */
void sym_del(const char* n)
{
    sym_ensure_init();
    Value* v = (Value*)rbtree_find(g_sym_tree, NULL, n);
    if(v) {
        if(v->type == VAL_STRING && !v->str_inline) {
            free(v->v.s);
        }
        free(v);
        rbtree_delete(g_sym_tree, NULL, n);
    }
}

/* 兼容旧代码：sym_ensure（现在不需要了，保留为空函数） */
void sym_ensure(int need)
{
    (void)need;
    sym_ensure_init();
}

double val_to_num(Value v) {
    if(v.type == VAL_INT) return (double)v.v.i;
    if(v.type == VAL_DOUBLE) return v.v.d;
    if(v.type == VAL_CHAR) return (double)(unsigned char)v.v.c;
    return 0.0;
}

int value_equal(Value a, Value b) {
    if(a.type != b.type) {
        if ((a.type == VAL_CHAR && b.type == VAL_INT)){
            return ((long long)(unsigned char)a.v.c) == b.v.i;
        }
        if ((a.type == VAL_INT && b.type == VAL_CHAR)){
            return a.v.i == (long long)(unsigned char)b.v.c;
        }
        return 0;
    }
    switch(a.type){
        case VAL_INT:     return a.v.i == b.v.i;
        case VAL_DOUBLE:  return a.v.d == b.v.d;
        case VAL_BOOL:    return a.v.b == b.v.b;
        case VAL_CHAR:    return a.v.c == b.v.c;
        case VAL_STRING:  return strcmp(lumyr_str_cstr(&a), lumyr_str_cstr(&b)) == 0;
        default: return 0;
    }
}

char* lumyr_concat(const char* s1, const char* s2) {
    size_t l1 = strlen(s1);
    size_t l2 = strlen(s2);
    char* out = malloc(l1 + l2 + 1);
    memcpy(out, s1, l1);
    memcpy(out+l1, s2, l2);
    out[l1+l2] = '\0';
    return out;
}
