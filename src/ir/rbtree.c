/*
 * 红黑树实现
 */
#include "rbtree.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define RED 0
#define BLACK 1

/* 比较两个键：先比较 class_name（NULL 最小），再比较 method_name */
static int rbtree_compare(const char* class1, const char* method1, const char* class2, const char* method2) {
    /* class_name 为 NULL 表示普通函数，排在最前面 */
    if(class1 == NULL && class2 == NULL) {
        return strcmp(method1, method2);
    }
    if(class1 == NULL) return -1;
    if(class2 == NULL) return 1;
    int cmp = strcmp(class1, class2);
    if(cmp != 0) return cmp;
    return strcmp(method1, method2);
}

/* 创建哨兵节点 */
static RBNode* rbtree_create_nil(void) {
    RBNode* nil = (RBNode*)malloc(sizeof(RBNode));
    nil->color = BLACK;
    nil->left = nil->right = nil->parent = nil;
    nil->class_name = NULL;
    nil->method_name = NULL;
    nil->data = NULL;
    return nil;
}

/* 创建红黑树 */
RBTree* rbtree_create(void) {
    RBTree* tree = (RBTree*)malloc(sizeof(RBTree));
    tree->nil = rbtree_create_nil();
    tree->root = tree->nil;
    tree->count = 0;
    return tree;
}

/* 左旋 */
static void rbtree_left_rotate(RBTree* tree, RBNode* x) {
    RBNode* y = x->right;
    x->right = y->left;
    if(y->left != tree->nil) y->left->parent = x;
    y->parent = x->parent;
    if(x->parent == tree->nil) tree->root = y;
    else if(x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

/* 右旋 */
static void rbtree_right_rotate(RBTree* tree, RBNode* y) {
    RBNode* x = y->left;
    y->left = x->right;
    if(x->right != tree->nil) x->right->parent = y;
    x->parent = y->parent;
    if(y->parent == tree->nil) tree->root = x;
    else if(y == y->parent->right) y->parent->right = x;
    else y->parent->left = x;
    x->right = y;
    y->parent = x;
}

/* 插入修复 */
static void rbtree_insert_fixup(RBTree* tree, RBNode* z) {
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
                    rbtree_left_rotate(tree, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rbtree_right_rotate(tree, z->parent->parent);
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
                    rbtree_right_rotate(tree, z);
                }
                z->parent->color = BLACK;
                z->parent->parent->color = RED;
                rbtree_left_rotate(tree, z->parent->parent);
            }
        }
    }
    tree->root->color = BLACK;
}

/* 插入节点 */
void rbtree_insert(RBTree* tree, const char* class_name, const char* method_name, void* data) {
    RBNode* z = (RBNode*)malloc(sizeof(RBNode));
    z->class_name = class_name ? strdup(class_name) : NULL;
    z->method_name = strdup(method_name);
    z->data = data;
    z->color = RED;
    z->left = z->right = z->parent = tree->nil;

    RBNode* y = tree->nil;
    RBNode* x = tree->root;
    while(x != tree->nil) {
        y = x;
        if(rbtree_compare(class_name, method_name, x->class_name, x->method_name) < 0)
            x = x->left;
        else
            x = x->right;
    }
    z->parent = y;
    if(y == tree->nil) tree->root = z;
    else if(rbtree_compare(class_name, method_name, y->class_name, y->method_name) < 0)
        y->left = z;
    else
        y->right = z;

    rbtree_insert_fixup(tree, z);
    tree->count++;
}

/* 查找节点 - 精确匹配 */
void* rbtree_find(RBTree* tree, const char* class_name, const char* method_name) {
    RBNode* x = tree->root;
    while(x != tree->nil) {
        int cmp = rbtree_compare(class_name, method_name, x->class_name, x->method_name);
        if(cmp == 0) return x->data;
        else if(cmp < 0) x = x->left;
        else x = x->right;
    }
    return NULL;
}

/* 查找第一个匹配 method_name 的节点（用于兼容旧代码） */
static void* rbtree_find_by_name_helper(RBNode* node, RBNode* nil, const char* method_name) {
    if(node == nil) return NULL;
    if(node->method_name && strcmp(node->method_name, method_name) == 0)
        return node->data;
    void* left = rbtree_find_by_name_helper(node->left, nil, method_name);
    if(left) return left;
    return rbtree_find_by_name_helper(node->right, nil, method_name);
}

void* rbtree_find_by_name(RBTree* tree, const char* method_name) {
    return rbtree_find_by_name_helper(tree->root, tree->nil, method_name);
}

/* 中序遍历 */
static void rbtree_foreach_helper(RBNode* node, RBNode* nil, void (*callback)(const char*, const char*, void*, void*), void* user_data) {
    if(node == nil) return;
    rbtree_foreach_helper(node->left, nil, callback, user_data);
    callback(node->class_name, node->method_name, node->data, user_data);
    rbtree_foreach_helper(node->right, nil, callback, user_data);
}

void rbtree_foreach(RBTree* tree, void (*callback)(const char* class_name, const char* method_name, void* data, void* user_data), void* user_data) {
    rbtree_foreach_helper(tree->root, tree->nil, callback, user_data);
}

/* 获取节点数量 */
int rbtree_count(RBTree* tree) {
    return tree->count;
}

/* 销毁节点 */
static void rbtree_destroy_node(RBNode* node, RBNode* nil) {
    if(node == nil) return;
    rbtree_destroy_node(node->left, nil);
    rbtree_destroy_node(node->right, nil);
    if(node->class_name) free(node->class_name);
    if(node->method_name) free(node->method_name);
    free(node);
}

/* 销毁红黑树 */
void rbtree_destroy(RBTree* tree) {
    rbtree_destroy_node(tree->root, tree->nil);
    free(tree->nil);
    free(tree);
}

/* 删除节点（简化实现，暂不使用） */
void rbtree_delete(RBTree* tree, const char* class_name, const char* method_name) {
    /* 暂不实现，当前项目不需要删除操作 */
    (void)tree;
    (void)class_name;
    (void)method_name;
}
