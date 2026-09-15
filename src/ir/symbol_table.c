/*
 * 统一符号表实现
 * 基于红黑树，键为 (file_name, scope, name)
 */
#include "symbol_table.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 红黑树节点颜色 */
#define RED 0
#define BLACK 1

/* 红黑树节点 */
typedef struct RBNode {
    char* file_name;
    char* scope;
    char* name;
    SymbolEntry* entry;
    int color;
    struct RBNode* left;
    struct RBNode* right;
    struct RBNode* parent;
} RBNode;

/* 符号表结构 */
struct SymbolTable {
    RBNode* root;
    RBNode* nil;
    size_t count;
};

/* 全局符号表实例 */
SymbolTable* g_symbol_table = NULL;

/* 比较两个键：先比较 file_name，再比较 scope，最后比较 name */
/* NULL 表示最小，排在最前面 */
static int rbtree_compare(const char* file1, const char* scope1, const char* name1,
                           const char* file2, const char* scope2, const char* name2) {
    /* 比较 file_name */
    if(file1 == NULL && file2 == NULL) {
        /* 都为 NULL，继续比较 scope */
    } else if(file1 == NULL) {
        return -1;
    } else if(file2 == NULL) {
        return 1;
    } else {
        int cmp = strcmp(file1, file2);
        if(cmp != 0) return cmp;
    }

    /* 比较 scope */
    if(scope1 == NULL && scope2 == NULL) {
        /* 都为 NULL，继续比较 name */
    } else if(scope1 == NULL) {
        return -1;
    } else if(scope2 == NULL) {
        return 1;
    } else {
        int cmp = strcmp(scope1, scope2);
        if(cmp != 0) return cmp;
    }

    /* 比较 name */
    if(name1 == NULL && name2 == NULL) return 0;
    if(name1 == NULL) return -1;
    if(name2 == NULL) return 1;
    return strcmp(name1, name2);
}

/* 创建哨兵节点 */
static RBNode* rbtree_create_nil(void) {
    RBNode* nil = (RBNode*)malloc(sizeof(RBNode));
    nil->color = BLACK;
    nil->left = nil->right = nil->parent = nil;
    nil->file_name = NULL;
    nil->scope = NULL;
    nil->name = NULL;
    nil->entry = NULL;
    return nil;
}

/* 创建符号表 */
SymbolTable* symbol_table_create(void) {
    SymbolTable* table = (SymbolTable*)malloc(sizeof(SymbolTable));
    table->nil = rbtree_create_nil();
    table->root = table->nil;
    table->count = 0;
    return table;
}

/* 左旋 */
static void rbtree_left_rotate(SymbolTable* table, RBNode* x) {
    RBNode* y = x->right;
    x->right = y->left;
    if(y->left != table->nil) y->left->parent = x;
    y->parent = x->parent;
    if(x->parent == table->nil) {
        table->root = y;
    } else if(x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
}

/* 右旋 */
static void rbtree_right_rotate(SymbolTable* table, RBNode* y) {
    RBNode* x = y->left;
    y->left = x->right;
    if(x->right != table->nil) x->right->parent = y;
    x->parent = y->parent;
    if(y->parent == table->nil) {
        table->root = x;
    } else if(y == y->parent->right) {
        y->parent->right = x;
    } else {
        y->parent->left = x;
    }
    x->right = y;
    y->parent = x;
}

/* 插入修复 */
static void rbtree_insert_fixup(SymbolTable* table, RBNode* z) {
    while(z->parent->color == RED) {
        if(z->parent == z->parent->parent->left) {
            RBNode* y = z->parent->parent->right;
            if(y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if(z == z->parent->right) {
                    z = z->parent;
                    rbtree_left_rotate(table, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rbtree_right_rotate(table, z->parent->parent);
            }
        } else {
            RBNode* y = z->parent->parent->left;
            if(y->color == RED) {
                z->parent->color = BLACK;
                y->color = BLACK;
                z->parent->parent->color = RED;
                z = z->parent->parent;
            } else {
                if(z == z->parent->left) {
                    z = z->parent;
                    rbtree_right_rotate(table, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rbtree_left_rotate(table, z->parent->parent);
            }
        }
    }
    table->root->color = BLACK;
}

/* 添加符号 */
int symbol_table_add(SymbolTable* table, const char* file_name, const char* scope,
                     const char* name, SymbolType type, void* data) {
    if(!table || !name) return -1;

    /* 先查找是否已存在 */
    SymbolEntry* existing = symbol_table_find(table, file_name, scope, name);
    if(existing) {
        /* 已存在，更新数据 */
        existing->type = type;
        existing->data = data;
        return 0;
    }

    /* 创建符号条目 */
    SymbolEntry* entry = (SymbolEntry*)malloc(sizeof(SymbolEntry));
    entry->type = type;
    entry->file_name = file_name ? strdup(file_name) : NULL;
    entry->scope = scope ? strdup(scope) : NULL;
    entry->name = strdup(name);
    entry->data = data;
    entry->c_name = symbol_table_gen_c_name(file_name, scope, name);
    entry->next = NULL;

    /* 创建红黑树节点 */
    RBNode* z = (RBNode*)malloc(sizeof(RBNode));
    z->file_name = entry->file_name;
    z->scope = entry->scope;
    z->name = entry->name;
    z->entry = entry;
    z->color = RED;
    z->left = z->right = z->parent = table->nil;

    /* 插入 */
    RBNode* y = table->nil;
    RBNode* x = table->root;
    while(x != table->nil) {
        y = x;
        int cmp = rbtree_compare(file_name, scope, name, x->file_name, x->scope, x->name);
        if(cmp < 0) {
            x = x->left;
        } else if(cmp > 0) {
            x = x->right;
        } else {
            /* 键相同，不应该发生（因为前面已经查找过了） */
            free(z);
            free(entry->c_name);
            free(entry->name);
            if(entry->scope) free(entry->scope);
            if(entry->file_name) free(entry->file_name);
            free(entry);
            return -1;
        }
    }
    z->parent = y;
    if(y == table->nil) {
        table->root = z;
    } else if(rbtree_compare(file_name, scope, name, y->file_name, y->scope, y->name) < 0) {
        y->left = z;
    } else {
        y->right = z;
    }

    table->count++;
    rbtree_insert_fixup(table, z);
    return 0;
}

/* 查找符号 */
SymbolEntry* symbol_table_find(SymbolTable* table, const char* file_name,
                                const char* scope, const char* name) {
    if(!table || !name) return NULL;

    RBNode* x = table->root;
    while(x != table->nil) {
        int cmp = rbtree_compare(file_name, scope, name, x->file_name, x->scope, x->name);
        if(cmp == 0) return x->entry;
        if(cmp < 0) {
            x = x->left;
        } else {
            x = x->right;
        }
    }
    return NULL;
}

/* 按名字查找辅助函数（中序遍历） */
static SymbolEntry* rbtree_find_by_name_helper(RBNode* node, RBNode* nil, const char* name) {
    if(node == nil) return NULL;
    SymbolEntry* left = rbtree_find_by_name_helper(node->left, nil, name);
    if(left) return left;
    if(node->name && strcmp(node->name, name) == 0) return node->entry;
    return rbtree_find_by_name_helper(node->right, nil, name);
}

/* 按名字查找（不管 scope 和 file_name，用于兼容旧代码） */
SymbolEntry* symbol_table_find_by_name(SymbolTable* table, const char* name) {
    if(!table || !name) return NULL;
    return rbtree_find_by_name_helper(table->root, table->nil, name);
}

/* 按 scope 和 name 查找辅助函数 */
static SymbolEntry* rbtree_find_by_scope_name_helper(RBNode* node, RBNode* nil,
                                                       const char* scope, const char* name) {
    if(node == nil) return NULL;
    SymbolEntry* left = rbtree_find_by_scope_name_helper(node->left, nil, scope, name);
    if(left) return left;
    if(node->name && strcmp(node->name, name) == 0) {
        if((scope == NULL && node->scope == NULL) ||
           (scope != NULL && node->scope != NULL && strcmp(node->scope, scope) == 0)) {
            return node->entry;
        }
    }
    return rbtree_find_by_scope_name_helper(node->right, nil, scope, name);
}

/* 按 scope 和 name 查找（不管 file_name） */
SymbolEntry* symbol_table_find_by_scope_name(SymbolTable* table, const char* scope,
                                               const char* name) {
    if(!table || !name) return NULL;
    return rbtree_find_by_scope_name_helper(table->root, table->nil, scope, name);
}

/* 删除符号（简化实现：只删除节点，不做红黑树删除修复） */
/* 注意：这是一个简化实现，实际使用中应该实现完整的红黑树删除 */
int symbol_table_remove(SymbolTable* table, const char* file_name, const char* scope,
                        const char* name) {
    if(!table || !name) return -1;
    /* 简化实现：查找并标记为已删除 */
    SymbolEntry* entry = symbol_table_find(table, file_name, scope, name);
    if(!entry) return -1;
    entry->type = SYMBOL_UNKNOWN;
    entry->data = NULL;
    return 0;
}

/* 遍历辅助函数 */
static void rbtree_foreach_helper(RBNode* node, RBNode* nil,
                                   SymbolTableCallback callback, void* user_data) {
    if(node == nil) return;
    rbtree_foreach_helper(node->left, nil, callback, user_data);
    if(node->entry && node->entry->type != SYMBOL_UNKNOWN) {
        callback(node->file_name, node->scope, node->name,
                 node->entry->type, node->entry->data, user_data);
    }
    rbtree_foreach_helper(node->right, nil, callback, user_data);
}

/* 遍历所有符号 */
void symbol_table_foreach(SymbolTable* table, SymbolTableCallback callback, void* user_data) {
    if(!table || !callback) return;
    rbtree_foreach_helper(table->root, table->nil, callback, user_data);
}

/* 获取符号表大小 */
size_t symbol_table_size(SymbolTable* table) {
    if(!table) return 0;
    return table->count;
}

/* 生成唯一的 C 名称（带文件名前缀，避免跨文件冲突） */
char* symbol_table_gen_c_name(const char* file_name, const char* scope, const char* name) {
    if(!name) return NULL;

    /* 计算缓冲区大小 */
    size_t len = strlen("lumyr_") + strlen(name) + 1;
    if(file_name) len += strlen(file_name) + 1;
    if(scope) len += strlen(scope) + 1;

    char* c_name = (char*)malloc(len);
    if(!c_name) return NULL;

    strcpy(c_name, "lumyr_");
    if(file_name) {
        /* 把文件名中的非字母数字字符替换成下划线 */
        for(size_t i = 0; i < strlen(file_name); i++) {
            char c = file_name[i];
            if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9')) {
                strncat(c_name, &c, 1);
            } else {
                strcat(c_name, "_");
            }
        }
        strcat(c_name, "_");
    }
    if(scope) {
        strcat(c_name, scope);
        strcat(c_name, "_");
    }
    strcat(c_name, name);

    return c_name;
}

/* 销毁辅助函数 */
static void rbtree_destroy_helper(RBNode* node, RBNode* nil) {
    if(node == nil) return;
    rbtree_destroy_helper(node->left, nil);
    rbtree_destroy_helper(node->right, nil);
    if(node->entry) {
        if(node->entry->c_name) free(node->entry->c_name);
        if(node->entry->name) free(node->entry->name);
        if(node->entry->scope) free(node->entry->scope);
        if(node->entry->file_name) free(node->entry->file_name);
        free(node->entry);
    }
    free(node);
}

/* 销毁符号表 */
void symbol_table_destroy(SymbolTable* table) {
    if(!table) return;
    rbtree_destroy_helper(table->root, table->nil);
    free(table->nil);
    free(table);
}

/* 初始化全局符号表 */
void symbol_table_global_init(void) {
    if(!g_symbol_table) {
        g_symbol_table = symbol_table_create();
    }
}

/* 销毁全局符号表 */
void symbol_table_global_destroy(void) {
    if(g_symbol_table) {
        symbol_table_destroy(g_symbol_table);
        g_symbol_table = NULL;
    }
}
