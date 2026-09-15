/*
 * 红黑树实现 - 用于 ir_func_table
 * 键为 (class_name, method_name) 复合键，class_name 为 NULL 表示普通函数
 */
#ifndef RBTREE_H
#define RBTREE_H

#include <stddef.h>

typedef struct RBNode {
    char* class_name;   /* NULL 表示普通函数 */
    char* method_name;  /* 函数名 */
    void* data;         /* 存储的数据（BytecodeFunc*） */
    int color;          /* 0: 红, 1: 黑 */
    struct RBNode* left;
    struct RBNode* right;
    struct RBNode* parent;
} RBNode;

typedef struct RBTree {
    RBNode* root;
    RBNode* nil;  /* 哨兵节点 */
    int count;
} RBTree;

/* 创建红黑树 */
RBTree* rbtree_create(void);

/* 销毁红黑树 */
void rbtree_destroy(RBTree* tree);

/* 插入节点 */
void rbtree_insert(RBTree* tree, const char* class_name, const char* method_name, void* data);

/* 查找节点 - 精确匹配 (class_name, method_name) */
void* rbtree_find(RBTree* tree, const char* class_name, const char* method_name);


/* 删除节点 */
void rbtree_delete(RBTree* tree, const char* class_name, const char* method_name);

/* 获取节点数量 */
int rbtree_count(RBTree* tree);

/* 中序遍历（用于迭代） */
void rbtree_foreach(RBTree* tree, void (*callback)(const char* class_name, const char* method_name, void* data, void* user_data), void* user_data);

#endif /* RBTREE_H */
