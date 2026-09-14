#include "lm_struct.h"
#include "lm_value.h"
#include "lm_runtime.h"
#include <string.h>
#include <stdlib.h>

/* ==================== 红黑树实现（用于存储 struct 信息，大型项目 struct 特别多时 O(log n) 查找） ==================== */

typedef struct StructRBNode {
    char* key;              /* struct 名 */
    void* value;            /* struct 信息（包含字段信息数组） */
    int color;              /* 0: 红, 1: 黑 */
    struct StructRBNode* left;
    struct StructRBNode* right;
    struct StructRBNode* parent;
} StructRBNode;

typedef struct {
    StructRBNode* root;
    StructRBNode* nil;  /* 哨兵节点 */
    int count;
} StructRBTree;

static StructRBTree* g_struct_tree = NULL;

static StructRBNode* struct_rb_create_node(const char* key, void* value)
{
    StructRBNode* node = (StructRBNode*)malloc(sizeof(StructRBNode));
    if(!node) return NULL;
    node->key = strdup(key);
    node->value = value;
    node->color = 0;  /* 红色 */
    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;
    return node;
}

static void struct_rb_left_rotate(StructRBTree* tree, StructRBNode* x)
{
    StructRBNode* y = x->right;
    x->right = y->left;
    if(y->left != tree->nil) y->left->parent = x;
    y->parent = x->parent;
    if(x->parent == tree->nil) tree->root = y;
    else if(x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void struct_rb_right_rotate(StructRBTree* tree, StructRBNode* y)
{
    StructRBNode* x = y->left;
    y->left = x->right;
    if(x->right != tree->nil) x->right->parent = y;
    x->parent = y->parent;
    if(y->parent == tree->nil) tree->root = x;
    else if(y == y->parent->right) y->parent->right = x;
    else y->parent->left = x;
    x->right = y;
    y->parent = x;
}

static void struct_rb_insert_fixup(StructRBTree* tree, StructRBNode* z)
{
    while(z->parent->color == 0) {  /* 红色 */
        if(z->parent == z->parent->parent->left) {
            StructRBNode* y = z->parent->parent->right;
            if(y->color == 0) {
                z->parent->color = 1;  /* 黑色 */
                y->color = 1;
                z->parent->parent->color = 0;
                z = z->parent->parent;
            } else {
                if(z == z->parent->right) {
                    z = z->parent;
                    struct_rb_left_rotate(tree, z);
                }
                z->parent->color = 1;
                z->parent->parent->color = 0;
                struct_rb_right_rotate(tree, z->parent->parent);
            }
        } else {
            StructRBNode* y = z->parent->parent->left;
            if(y->color == 0) {
                z->parent->color = 1;
                y->color = 1;
                z->parent->parent->color = 0;
                z = z->parent->parent;
            } else {
                if(z == z->parent->left) {
                    z = z->parent;
                    struct_rb_right_rotate(tree, z);
                }
                z->parent->color = 1;
                z->parent->parent->color = 0;
                struct_rb_left_rotate(tree, z->parent->parent);
            }
        }
    }
    tree->root->color = 1;  /* 黑色 */
}

static void struct_rb_insert(StructRBTree* tree, const char* key, void* value)
{
    if(!tree || !key) return;
    StructRBNode* z = struct_rb_create_node(key, value);
    if(!z) return;
    StructRBNode* y = tree->nil;
    StructRBNode* x = tree->root;
    while(x != tree->nil) {
        y = x;
        if(strcmp(z->key, x->key) < 0) x = x->left;
        else x = x->right;
    }
    z->parent = y;
    if(y == tree->nil) tree->root = z;
    else if(strcmp(z->key, y->key) < 0) y->left = z;
    else y->right = z;
    z->left = tree->nil;
    z->right = tree->nil;
    z->color = 0;
    struct_rb_insert_fixup(tree, z);
    tree->count++;
}

static StructRBNode* struct_rb_find_node(StructRBTree* tree, const char* key)
{
    if(!tree || !key) return NULL;
    StructRBNode* x = tree->root;
    while(x != tree->nil) {
        int cmp = strcmp(key, x->key);
        if(cmp == 0) return x;
        else if(cmp < 0) x = x->left;
        else x = x->right;
    }
    return NULL;
}

static StructRBTree* struct_rb_create(void)
{
    StructRBTree* tree = (StructRBTree*)malloc(sizeof(StructRBTree));
    if(!tree) return NULL;
    tree->nil = (StructRBNode*)malloc(sizeof(StructRBNode));
    if(!tree->nil) { free(tree); return NULL; }
    tree->nil->color = 1;  /* 黑色 */
    tree->nil->left = NULL;
    tree->nil->right = NULL;
    tree->nil->parent = NULL;
    tree->nil->key = NULL;
    tree->nil->value = NULL;
    tree->root = tree->nil;
    tree->count = 0;
    return tree;
}

/* struct 信息内部结构体（用于存储在红黑树中） */
typedef struct {
    const char* struct_name;
    int nfields;
    StructFieldInfo* fields;
} StructInfo;

/* 确保红黑树已初始化 */
static void ensure_struct_tree(void)
{
    if(!g_struct_tree) g_struct_tree = struct_rb_create();
}

/* ==================== struct 信息管理（使用红黑树存储） ==================== */

/* 注册 struct 信息（在代码生成时调用，把 struct 的字段信息导出到运行时） */
void lumyr_struct_register(const char* struct_name, int nfields, StructFieldInfo* fields)
{
    if(!struct_name) return;
    ensure_struct_tree();
    /* 检查是否已经注册过 */
    StructRBNode* existing = struct_rb_find_node(g_struct_tree, struct_name);
    if(existing) {
        /* 已经注册过，更新信息 */
        if(existing->value) {
            StructInfo* si = (StructInfo*)existing->value;
            if(si->fields) free(si->fields);
            si->nfields = nfields;
            si->fields = fields;
        }
        return;
    }
    /* 创建新的 StructInfo */
    StructInfo* si = (StructInfo*)malloc(sizeof(StructInfo));
    if(!si) return;
    si->struct_name = strdup(struct_name);
    si->nfields = nfields;
    si->fields = fields;
    /* 插入红黑树 */
    struct_rb_insert(g_struct_tree, struct_name, si);
}

/* 查找 struct 字段信息（通过 struct 名和字段名） */
StructFieldInfo* lumyr_struct_find_field(const char* struct_name, const char* field_name)
{
    if(!struct_name || !field_name) return NULL;
    ensure_struct_tree();
    StructRBNode* node = struct_rb_find_node(g_struct_tree, struct_name);
    if(!node || !node->value) return NULL;
    StructInfo* si = (StructInfo*)node->value;
    for(int i = 0; i < si->nfields; i++) {
        if(strcmp(si->fields[i].name, field_name) == 0) {
            return &si->fields[i];
        }
    }
    return NULL;
}

/* 获取 struct 实例的 struct 名（通过 __structname__ 字段，它是结构体的第一个字段）
   __structname__ 字段的类型是 const char*，在结构体的偏移量 0 的位置 */
const char* lumyr_struct_get_name(Value obj)
{
    if(obj.type != VAL_STRUCT_PTR || !obj.v.struct_ptr) return NULL;
    /* __structname__ 字段在偏移量 0 的位置，类型是 const char* */
    const char** name_ptr = (const char**)obj.v.struct_ptr;
    return *name_ptr;
}

/* struct 属性读取（专门针对 struct 的函数，不依赖通用的 lumyr_index_get） */
Value lumyr_struct_get_field(Value obj, const char* field_name)
{
    if(obj.type != VAL_STRUCT_PTR || !obj.v.struct_ptr || !field_name) {
        runtime_error("struct 属性读取：对象不是 struct 实例或字段名为空");
        return val_none();
    }
    /* 特殊处理 __structname__ 只读属性 */
    if(strcmp(field_name, "__structname__") == 0) {
        const char* name = lumyr_struct_get_name(obj);
        return lumyr_make_string(name ? name : "");
    }
    /* 获取 struct 名 */
    const char* struct_name = lumyr_struct_get_name(obj);
    if(!struct_name) {
        runtime_error("struct 属性读取：无法获取 struct 名");
        return val_none();
    }
    /* 查找字段信息（O(log n) 红黑树查找 + O(n) 字段遍历，字段数量通常不多） */
    StructFieldInfo* fi = lumyr_struct_find_field(struct_name, field_name);
    if(!fi) {
        char buf[256];
        snprintf(buf, sizeof(buf), "struct 属性读取：struct '%s' 没有字段 '%s'", struct_name, field_name);
        runtime_error(buf);
        return val_none();
    }
    /* 根据字段类型读取字段值 */
    char* field_ptr = (char*)obj.v.struct_ptr + fi->offset;
    switch(fi->type) {
        case STRUCT_FIELD_INT:
            return lumyr_make_int(*(long long*)field_ptr);
        case STRUCT_FIELD_DOUBLE:
            return lumyr_make_double(*(double*)field_ptr);
        case STRUCT_FIELD_STRING:
            return lumyr_make_string(*(const char**)field_ptr ? *(const char**)field_ptr : "");
        case STRUCT_FIELD_BOOL:
            return lumyr_make_bool(*(int*)field_ptr ? 1 : 0);
        case STRUCT_FIELD_PTR: {
            /* 指针类型（struct/class/array/map/func），直接返回指针包装成 Value */
            Value v;
            v.type = VAL_STRUCT_PTR;
            v.v.struct_ptr = *(void**)field_ptr;
            return v;
        }
        default:
            runtime_error("struct 属性读取：不支持的字段类型");
            return val_none();
    }
}

/* struct 属性写入（专门针对 struct 的函数） */
void lumyr_struct_set_field(Value obj, const char* field_name, Value value)
{
    if(obj.type != VAL_STRUCT_PTR || !obj.v.struct_ptr || !field_name) {
        runtime_error("struct 属性写入：对象不是 struct 实例或字段名为空");
        return;
    }
    /* __structname__ 是只读属性，不允许修改 */
    if(strcmp(field_name, "__structname__") == 0) {
        runtime_error("struct 属性写入：__structname__ 是只读属性");
        return;
    }
    /* 获取 struct 名 */
    const char* struct_name = lumyr_struct_get_name(obj);
    if(!struct_name) {
        runtime_error("struct 属性写入：无法获取 struct 名");
        return;
    }
    /* 查找字段信息 */
    StructFieldInfo* fi = lumyr_struct_find_field(struct_name, field_name);
    if(!fi) {
        char buf[256];
        snprintf(buf, sizeof(buf), "struct 属性写入：struct '%s' 没有字段 '%s'", struct_name, field_name);
        runtime_error(buf);
        return;
    }
    /* 根据字段类型写入字段值 */
    char* field_ptr = (char*)obj.v.struct_ptr + fi->offset;
    switch(fi->type) {
        case STRUCT_FIELD_INT:
            *(long long*)field_ptr = value.v.i;
            break;
        case STRUCT_FIELD_DOUBLE:
            *(double*)field_ptr = value.type == VAL_DOUBLE ? value.v.d : (double)value.v.i;
            break;
        case STRUCT_FIELD_STRING:
            /* 字符串字段需要深拷贝 */
            if(*(char**)field_ptr) free(*(char**)field_ptr);
            *(char**)field_ptr = strdup(lumyr_str_cstr(&value));
            break;
        case STRUCT_FIELD_BOOL:
            *(int*)field_ptr = value.type == VAL_BOOL ? value.v.b : (value.v.i ? 1 : 0);
            break;
        case STRUCT_FIELD_PTR:
            *(void**)field_ptr = value.v.struct_ptr;
            break;
        default:
            runtime_error("struct 属性写入：不支持的字段类型");
            break;
    }
}
