/*
 * 红黑树实现 - 用于全局符号表
 * 键：(namespace, class_name, name) 三维复合键
 * 支持：普通函数、类方法、构造函数、变量、类变量、结构体等
 */
#ifndef RBTREE_H
#define RBTREE_H

#include <stddef.h>

/* 命名空间类型 */
typedef enum {
    NS_FUNCTION = 0,    /* 普通函数 */
    NS_METHOD = 1,      /* 类方法 */
    NS_CONSTRUCTOR = 2, /* 构造函数（与类名同名） */
    NS_VARIABLE = 3,    /* 全局变量 */
    NS_CLASS_VAR = 4,   /* 类变量（静态属性） */
    NS_CLASS = 5,       /* 类定义 */
    NS_STRUCT = 6,      /* 结构体定义 */
    NS_PROPERTY = 7,    /* 类属性（实例属性） */
    NS_NAMESPACE = 8    /* 命名空间/模块 */
} RBTNamespace;

typedef struct RBNode {
    RBTNamespace ns;       /* 命名空间 */
    char* class_name;      /* 类名/结构体名（NULL 表示全局） */
    char* name;            /* 函数名/变量名/属性名 */
    void* data;            /* 存储的数据 */
    int color;             /* 0: 红, 1: 黑 */
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
void rbtree_insert(RBTree* tree, RBTNamespace ns, const char* class_name, const char* name, void* data);

/* 查找节点 - 精确匹配 */
void* rbtree_find(RBTree* tree, RBTNamespace ns, const char* class_name, const char* name);

/* 删除节点 */
void rbtree_delete(RBTree* tree, RBTNamespace ns, const char* class_name, const char* name);

/* 获取节点数量 */
int rbtree_count(RBTree* tree);

/* 按命名空间查找所有节点（遍历） */
void rbtree_foreach_ns(RBTree* tree, RBTNamespace ns, 
                      void (*callback)(RBTNamespace ns, const char* class_name, const char* name, void* data, void* user_data), 
                      void* user_data);

/* 遍历所有节点 */
void rbtree_foreach(RBTree* tree, 
                    void (*callback)(RBTNamespace ns, const char* class_name, const char* name, void* data, void* user_data), 
                    void* user_data);

#endif /* RBTREE_H */
