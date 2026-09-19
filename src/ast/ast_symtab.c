#include "ast_symtab.h"
#include "ir/rbtree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 符号表用红黑树存储，键为符号名，class_name 为 NULL */
static RBTree* static_sym_tree = NULL;

/* 作用域层级栈：每个层级记录该层级新创建的符号名，用于恢复时删除 */
typedef struct ScopeLevel {
    char** names;        /* 该层级新创建的符号名 */
    int count;
    int cap;
    struct ScopeLevel* next;
} ScopeLevel;

static ScopeLevel* scope_stack = NULL;  /* 栈顶 */

/* 创建新的作用域层级（压栈） */
void static_sym_save(void)
{
    ScopeLevel* level = (ScopeLevel*)calloc(1, sizeof(ScopeLevel));
    level->next = scope_stack;
    scope_stack = level;
}

/* 恢复到上一个作用域层级（弹栈），逻辑删除当前层级新创建的所有符号。
 * 注意：rbtree_delete 是未实现的 stub，不能从树中移除节点；因此采用逻辑删除
 * （置 entry->deleted=1），既避免 use-after-free，又使 static_sym_get 视其为不存在。 */
void static_sym_restore(void)
{
    if(!scope_stack) return;
    ScopeLevel* level = scope_stack;
    for(int i = 0; i < level->count; i++) {
        if(static_sym_tree) {
            SymStaticEntry* entry = (SymStaticEntry*)rbtree_find(static_sym_tree, NS_VARIABLE, NULL, level->names[i]);
            if(entry) {
                entry->deleted = 1;  /* 逻辑删除，不 free */
            }
        }
        free(level->names[i]);
    }
    free(level->names);
    scope_stack = level->next;
    free(level);
}

/* 记录当前层级新创建的符号名 */
static void scope_record_name(const char* name)
{
    if(!scope_stack) return;  /* 不在作用域中，不需要记录 */
    ScopeLevel* level = scope_stack;
    if(level->count >= level->cap) {
        level->cap = level->cap ? level->cap * 2 : 16;
        level->names = (char**)realloc(level->names, (size_t)level->cap * sizeof(char*));
    }
    level->names[level->count++] = strdup(name);
}

/* static_sym_reset 的释放回调函数 */
static void static_sym_free_cb(const char* class_name, const char* method_name, void* data, void* user_data)
{
    (void)class_name; (void)method_name; (void)user_data;
    SymStaticEntry* entry = (SymStaticEntry*)data;
    free(entry->name);
    free(entry);
}

void static_sym_reset(void)
{
    if(static_sym_tree) {
        /* 遍历红黑树，释放所有符号 */
        rbtree_foreach(static_sym_tree, static_sym_free_cb, NULL);
        rbtree_destroy(static_sym_tree);
        static_sym_tree = NULL;
    }
    /* 清空作用域栈 */
    while(scope_stack) {
        ScopeLevel* level = scope_stack;
        for(int i = 0; i < level->count; i++) {
            free(level->names[i]);
        }
        free(level->names);
        scope_stack = level->next;
        free(level);
    }
}

int static_sym_put(const char* name, ValueType ty)
{
    if(!static_sym_tree) static_sym_tree = rbtree_create();
    SymStaticEntry* existing = (SymStaticEntry*)rbtree_find(static_sym_tree, NS_VARIABLE, NULL, name);
    if(existing) {
        /* 已存在：若为逻辑删除则复活（当前作用域接管所有权），否则就地更新类型 */
        if(existing->deleted) {
            existing->deleted = 0;
            scope_record_name(name);
        }
        existing->ty = ty;
        return 1;
    }
    /* 不存在，创建新符号 */
    SymStaticEntry* entry = (SymStaticEntry*)malloc(sizeof(SymStaticEntry));
    entry->name = strdup(name);
    entry->ty = ty;
    entry->next = NULL;
    entry->deleted = 0;
    rbtree_insert(static_sym_tree, NS_VARIABLE, NULL, name, entry);
    scope_record_name(name);
    return 1;
}

int static_sym_get(const char* name, ValueType* out_ty)
{
    if(strcmp(name, "log") == 0) { *out_ty = VAL_MAP; return 1; }  // 预定义全局对象 log
    if(!static_sym_tree) return 0;
    SymStaticEntry* entry = (SymStaticEntry*)rbtree_find(static_sym_tree, NS_VARIABLE, NULL, name);
    if(entry && !entry->deleted) {
        *out_ty = entry->ty;
        return 1;
    }
    return 0;
}
