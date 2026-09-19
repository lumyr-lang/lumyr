/*
 * 红黑树实现 - 用于全局符号表
 * 键：(namespace, class_name, name) 三维复合键
 */
#include "rbtree.h"
#include <stdlib.h>
#include <string.h>

/* 比较两个节点的键 */
static int rbtree_compare(RBNode* a, RBNode* b) {
    /* 先比较 namespace */
    if(a->ns != b->ns) {
        return (a->ns < b->ns) ? -1 : 1;
    }
    /* 再比较 class_name（NULL 排最前） */
    if(a->class_name == NULL && b->class_name != NULL) return -1;
    if(a->class_name != NULL && b->class_name == NULL) return 1;
    if(a->class_name != NULL && b->class_name != NULL) {
        int cmp = strcmp(a->class_name, b->class_name);
        if(cmp != 0) return cmp;
    }
    /* 最后比较 name */
    return strcmp(a->name, b->name);
}

/* 创建哨兵节点 */
static RBNode* rbtree_create_nil(void) {
    RBNode* nil = calloc(1, sizeof(RBNode));
    nil->color = 1; /* 黑色 */
    nil->left = nil->right = nil->parent = nil;
    return nil;
}

/* 创建红黑树 */
RBTree* rbtree_create(void) {
    RBTree* tree = calloc(1, sizeof(RBTree));
    tree->nil = rbtree_create_nil();
    tree->root = tree->nil;
    tree->count = 0;
    return tree;
}

/* 左旋 */
static void rbtree_left_rotate(RBTree* tree, RBNode* x) {
    RBNode* y = x->right;
    x->right = y->left;
    if(y->left != tree->nil) {
        y->left->parent = x;
    }
    y->parent = x->parent;
    if(x->parent == tree->nil) {
        tree->root = y;
    } else if(x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
}

/* 右旋 */
static void rbtree_right_rotate(RBTree* tree, RBNode* y) {
    RBNode* x = y->left;
    y->left = x->right;
    if(x->right != tree->nil) {
        x->right->parent = y;
    }
    x->parent = y->parent;
    if(y->parent == tree->nil) {
        tree->root = x;
    } else if(y == y->parent->left) {
        y->parent->left = x;
    } else {
        y->parent->right = x;
    }
    x->right = y;
    y->parent = x;
}

/* 插入后修复 */
static void rbtree_insert_fixup(RBTree* tree, RBNode* z) {
    while(z->parent->color == 0) { /* 父节点是红色 */
        if(z->parent == z->parent->parent->left) {
            RBNode* y = z->parent->parent->right; /* 叔节点 */
            if(y->color == 0) {
                /* 情况 1：叔节点是红色 */
                z->parent->color = 1;
                y->color = 1;
                z->parent->parent->color = 0;
                z = z->parent->parent;
            } else {
                if(z == z->parent->right) {
                    /* 情况 2：叔节点黑色，z 是右孩子 */
                    z = z->parent;
                    rbtree_left_rotate(tree, z);
                }
                /* 情况 3：叔节点黑色，z 是左孩子 */
                z->parent->color = 1;
                z->parent->parent->color = 0;
                rbtree_right_rotate(tree, z->parent->parent);
            }
        } else {
            RBNode* y = z->parent->parent->left; /* 叔节点 */
            if(y->color == 0) {
                /* 情况 1：叔节点是红色 */
                z->parent->color = 1;
                y->color = 1;
                z->parent->parent->color = 0;
                z = z->parent->parent;
            } else {
                if(z == z->parent->left) {
                    /* 情况 2：叔节点黑色，z 是左孩子 */
                    z = z->parent;
                    rbtree_right_rotate(tree, z);
                }
                /* 情况 3：叔节点黑色，z 是右孩子 */
                z->parent->color = 1;
                z->parent->parent->color = 0;
                rbtree_left_rotate(tree, z->parent->parent);
            }
        }
    }
    tree->root->color = 1;
}

/* 插入节点 */
void rbtree_insert(RBTree* tree, RBTNamespace ns, const char* class_name, const char* name, void* data) {
    if(!tree || !name) return;
    
    RBNode* z = calloc(1, sizeof(RBNode));
    z->ns = ns;
    z->class_name = class_name ? strdup(class_name) : NULL;
    z->name = strdup(name);
    z->data = data;
    z->color = 0; /* 新节点默认红色 */
    z->left = z->right = z->parent = tree->nil;
    
    RBNode* y = tree->nil;
    RBNode* x = tree->root;
    
    while(x != tree->nil) {
        y = x;
        if(rbtree_compare(z, x) < 0) {
            x = x->left;
        } else {
            x = x->right;
        }
    }
    
    z->parent = y;
    if(y == tree->nil) {
        tree->root = z;
    } else if(rbtree_compare(z, y) < 0) {
        y->left = z;
    } else {
        y->right = z;
    }
    
    tree->count++;
    rbtree_insert_fixup(tree, z);
}

/* 查找节点 */
void* rbtree_find(RBTree* tree, RBTNamespace ns, const char* class_name, const char* name) {
    if(!tree || !name) return NULL;
    
    RBNode* x = tree->root;
    while(x != tree->nil) {
        RBNode key = {.ns = ns, .class_name = (char*)class_name, .name = (char*)name};
        int cmp = rbtree_compare(&key, x);
        if(cmp == 0) {
            return x->data;
        } else if(cmp < 0) {
            x = x->left;
        } else {
            x = x->right;
        }
    }
    return NULL;
}

/* 获取节点数量 */
int rbtree_count(RBTree* tree) {
    return tree ? tree->count : 0;
}

/* 中序遍历（递归） */
static void rbtree_inorder(RBTree* tree, RBNode* node, 
                          void (*callback)(RBTNamespace, const char*, const char*, void*, void*), 
                          void* user_data) {
    if(node == tree->nil) return;
    rbtree_inorder(tree, node->left, callback, user_data);
    callback(node->ns, node->class_name, node->name, node->data, user_data);
    rbtree_inorder(tree, node->right, callback, user_data);
}

/* 遍历所有节点 */
void rbtree_foreach(RBTree* tree, 
                   void (*callback)(RBTNamespace, const char*, const char*, void*, void*), 
                   void* user_data) {
    if(!tree || !callback) return;
    rbtree_inorder(tree, tree->root, callback, user_data);
}

/* 按命名空间遍历 */
static void rbtree_inorder_ns(RBTree* tree, RBNode* node, RBTNamespace ns,
                              void (*callback)(RBTNamespace, const char*, const char*, void*, void*), 
                              void* user_data) {
    if(node == tree->nil) return;
    rbtree_inorder_ns(tree, node->left, ns, callback, user_data);
    if(node->ns == ns) {
        callback(node->ns, node->class_name, node->name, node->data, user_data);
    }
    rbtree_inorder_ns(tree, node->right, ns, callback, user_data);
}

void rbtree_foreach_ns(RBTree* tree, RBTNamespace ns,
                      void (*callback)(RBTNamespace, const char*, const char*, void*, void*), 
                      void* user_data) {
    if(!tree || !callback) return;
    rbtree_inorder_ns(tree, tree->root, ns, callback, user_data);
}

/* 销毁红黑树（递归释放） */
static void rbtree_free_node(RBTree* tree, RBNode* node) {
    if(node == tree->nil) return;
    rbtree_free_node(tree, node->left);
    rbtree_free_node(tree, node->right);
    free(node->class_name);
    free(node->name);
    free(node);
}

void rbtree_destroy(RBTree* tree) {
    if(!tree) return;
    rbtree_free_node(tree, tree->root);
    free(tree->nil);
    free(tree);
}

/* 删除节点（简化版：标记删除，实际不删除） */
void rbtree_delete(RBTree* tree, RBTNamespace ns, const char* class_name, const char* name) {
    /* TODO: 完整的红黑树删除实现 */
    (void)tree; (void)ns; (void)class_name; (void)name;
}

/* 替换已存在节点的 data，返回旧 data；键不存在返回 NULL */
void* rbtree_set_data(RBTree* tree, RBTNamespace ns, const char* class_name, const char* name, void* data) {
    if(!tree || !name) return NULL;
    RBNode* x = tree->root;
    while(x != tree->nil) {
        RBNode key;
        key.ns = ns;
        key.class_name = (char*)class_name;
        key.name = (char*)name;
        int cmp = rbtree_compare(&key, x);
        if(cmp == 0) {
            void* old = x->data;
            x->data = data;
            return old;
        }
        x = cmp < 0 ? x->left : x->right;
    }
    return NULL;
}
