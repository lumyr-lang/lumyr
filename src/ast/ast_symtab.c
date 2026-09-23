#include "ast_symtab.h"
#include "ir/rbtree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 符号表用红黑树存储，键为符号名，class_name 为 NULL */
static RBTree* static_sym_tree = NULL;

/* 作用域层级栈：每个层级记录该层级新创建的符号名，用于恢复时删除 */
typedef struct OverwrittenConst {
    SymStaticEntry* entry;   /* 被本层就地更新的既有 entry */
    int old_is_const;        /* 进入本层前的 is_const 旧值 */
} OverwrittenConst;
typedef struct ScopeLevel {
    char** names;        /* 该层级新创建的符号名 */
    int count;
    int cap;
    OverwrittenConst* overwritten;  /* 就地更新 entry 的 is_const 旧值（恢复时还原） */
    int ow_count;
    int ow_cap;
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
    /* 还原被本层就地遮蔽的既有 entry 的 is_const 旧值 */
    for(int i = 0; i < level->ow_count; i++) {
        if(level->overwritten[i].entry)
            level->overwritten[i].entry->is_const = level->overwritten[i].old_is_const;
    }
    free(level->overwritten);
    scope_stack = level->next;
    free(level);
}

/* 当前是否处于 save 的作用域（函数/lambda 内） */
int static_sym_scope_active(void)
{
    return scope_stack != NULL;
}

/* 名字是否已在当前作用域层级登记（本层新建/复活，或本层首次遮蔽的存活 entry） */
int static_sym_level_knows(const char* name)
{
    if(!scope_stack || !name) return 0;
    ScopeLevel* level = scope_stack;
    for(int i = 0; i < level->count; i++)
        if(strcmp(level->names[i], name) == 0) return 1;
    for(int i = 0; i < level->ow_count; i++)
        if(level->overwritten[i].entry &&
           strcmp(level->overwritten[i].entry->name, name) == 0) return 1;
    return 0;
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

/* 本层首次就地更新既有 entry 前，保存其 is_const 旧值（每个 entry 每层只存一次） */
static void scope_record_overwrite(SymStaticEntry* entry)
{
    if(!scope_stack || !entry) return;
    ScopeLevel* level = scope_stack;
    for(int i = 0; i < level->ow_count; i++)
        if(level->overwritten[i].entry == entry) return;
    if(level->ow_count >= level->ow_cap) {
        level->ow_cap = level->ow_cap ? level->ow_cap * 2 : 8;
        level->overwritten = (OverwrittenConst*)realloc(level->overwritten,
                                                        (size_t)level->ow_cap * sizeof(OverwrittenConst));
    }
    level->overwritten[level->ow_count].entry = entry;
    level->overwritten[level->ow_count].old_is_const = entry->is_const;
    level->ow_count++;
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
        free(level->overwritten);
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
            existing->is_const = 0;
            scope_record_name(name);
        } else {
            /* 存活 entry 被本层就地更新（形参/局部遮蔽）：保存 is_const 旧值后清除，
             * 避免外层 const 标记导致本层赋值被误判；作用域恢复时还原 */
            scope_record_overwrite(existing);
            existing->is_const = 0;
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
    entry->is_const = 0;
    rbtree_insert(static_sym_tree, NS_VARIABLE, NULL, name, entry);
    scope_record_name(name);
    return 1;
}

void static_sym_set_const(const char* name)
{
    if(!static_sym_tree) return;
    SymStaticEntry* entry = (SymStaticEntry*)rbtree_find(static_sym_tree, NS_VARIABLE, NULL, name);
    if(entry && !entry->deleted) entry->is_const = 1;
}

int static_sym_is_const(const char* name)
{
    if(!static_sym_tree) return 0;
    SymStaticEntry* entry = (SymStaticEntry*)rbtree_find(static_sym_tree, NS_VARIABLE, NULL, name);
    return (entry && !entry->deleted) ? entry->is_const : 0;
}

int static_sym_get(const char* name, ValueType* out_ty)
{
    if(strcmp(name, "logging") == 0) { *out_ty = VAL_MAP; return 1; }  // 预定义全局对象 logging（日志）
    if(!static_sym_tree) return 0;
    SymStaticEntry* entry = (SymStaticEntry*)rbtree_find(static_sym_tree, NS_VARIABLE, NULL, name);
    if(entry && !entry->deleted) {
        *out_ty = entry->ty;
        return 1;
    }
    return 0;
}
