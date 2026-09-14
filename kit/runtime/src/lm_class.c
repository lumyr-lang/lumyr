#include "lm_class.h"
#include "lm_value.h"
#include "lm_runtime.h"
#include <string.h>
#include <stdlib.h>

/* ==================== 红黑树实现（用于存储 class 信息，大型项目 class 特别多时 O(log n) 查找） ==================== */

typedef struct ClassRBNode {
    char* key;              /* class 名 */
    ClassInfo* value;       /* class 信息 */
    int color;              /* 0: 红, 1: 黑 */
    struct ClassRBNode* left;
    struct ClassRBNode* right;
    struct ClassRBNode* parent;
} ClassRBNode;

typedef struct {
    ClassRBNode* root;
    ClassRBNode* nil;  /* 哨兵节点 */
    int count;
} ClassRBTree;

static ClassRBTree* g_class_tree = NULL;

static ClassRBNode* class_rb_create_node(const char* key, ClassInfo* value)
{
    ClassRBNode* node = (ClassRBNode*)malloc(sizeof(ClassRBNode));
    if(!node) return NULL;
    node->key = strdup(key);
    node->value = value;
    node->color = 0;  /* 红色 */
    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;
    return node;
}

static void class_rb_left_rotate(ClassRBTree* tree, ClassRBNode* x)
{
    ClassRBNode* y = x->right;
    x->right = y->left;
    if(y->left != tree->nil) y->left->parent = x;
    y->parent = x->parent;
    if(x->parent == tree->nil) tree->root = y;
    else if(x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void class_rb_right_rotate(ClassRBTree* tree, ClassRBNode* y)
{
    ClassRBNode* x = y->left;
    y->left = x->right;
    if(x->right != tree->nil) x->right->parent = y;
    x->parent = y->parent;
    if(y->parent == tree->nil) tree->root = x;
    else if(y == y->parent->right) y->parent->right = x;
    else y->parent->left = x;
    x->right = y;
    y->parent = x;
}

static void class_rb_insert_fixup(ClassRBTree* tree, ClassRBNode* z)
{
    while(z->parent->color == 0) {  /* 红色 */
        if(z->parent == z->parent->parent->left) {
            ClassRBNode* y = z->parent->parent->right;
            if(y->color == 0) {
                z->parent->color = 1;  /* 黑色 */
                y->color = 1;
                z->parent->parent->color = 0;
                z = z->parent->parent;
            } else {
                if(z == z->parent->right) {
                    z = z->parent;
                    class_rb_left_rotate(tree, z);
                }
                z->parent->color = 1;
                z->parent->parent->color = 0;
                class_rb_right_rotate(tree, z->parent->parent);
            }
        } else {
            ClassRBNode* y = z->parent->parent->left;
            if(y->color == 0) {
                z->parent->color = 1;
                y->color = 1;
                z->parent->parent->color = 0;
                z = z->parent->parent;
            } else {
                if(z == z->parent->left) {
                    z = z->parent;
                    class_rb_right_rotate(tree, z);
                }
                z->parent->color = 1;
                z->parent->parent->color = 0;
                class_rb_left_rotate(tree, z->parent->parent);
            }
        }
    }
    tree->root->color = 1;  /* 黑色 */
}

static void class_rb_insert(ClassRBTree* tree, const char* key, ClassInfo* value)
{
    if(!tree || !key) return;
    ClassRBNode* z = class_rb_create_node(key, value);
    if(!z) return;
    ClassRBNode* y = tree->nil;
    ClassRBNode* x = tree->root;
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
    class_rb_insert_fixup(tree, z);
    tree->count++;
}

static ClassRBNode* class_rb_find_node(ClassRBTree* tree, const char* key)
{
    if(!tree || !key) return NULL;
    ClassRBNode* x = tree->root;
    while(x != tree->nil) {
        int cmp = strcmp(key, x->key);
        if(cmp == 0) return x;
        else if(cmp < 0) x = x->left;
        else x = x->right;
    }
    return NULL;
}

static ClassRBTree* class_rb_create(void)
{
    ClassRBTree* tree = (ClassRBTree*)malloc(sizeof(ClassRBTree));
    if(!tree) return NULL;
    tree->nil = (ClassRBNode*)malloc(sizeof(ClassRBNode));
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

/* ==================== class 信息管理（使用红黑树存储） ==================== */

/* 确保红黑树已初始化 */
static void ensure_class_tree(void)
{
    if(!g_class_tree) g_class_tree = class_rb_create();
}

/* 注册 class 信息（在代码生成时调用，把 class 的字段信息导出到运行时） */
void lumyr_class_register(const char* class_name, int nfields, ClassFieldInfo* fields, void* vtable)
{
    if(!class_name) return;
    ensure_class_tree();
    /* 检查是否已经注册过 */
    ClassRBNode* existing = class_rb_find_node(g_class_tree, class_name);
    if(existing) {
        /* 已经注册过，更新信息 */
        if(existing->value) {
            if(existing->value->fields) free(existing->value->fields);
            existing->value->nfields = nfields;
            existing->value->fields = fields;
            existing->value->vtable = vtable;
        }
        return;
    }
    /* 创建新的 ClassInfo */
    ClassInfo* ci = (ClassInfo*)malloc(sizeof(ClassInfo));
    if(!ci) return;
    ci->class_name = strdup(class_name);
    ci->nfields = nfields;
    ci->fields = fields;
    ci->vtable = vtable;
    /* 插入红黑树 */
    class_rb_insert(g_class_tree, class_name, ci);
}

/* 查找 class 信息（通过 class 名，O(log n)） */
ClassInfo* lumyr_class_lookup(const char* class_name)
{
    if(!class_name) return NULL;
    ensure_class_tree();
    ClassRBNode* node = class_rb_find_node(g_class_tree, class_name);
    return node ? node->value : NULL;
}

/* 查找 class 字段信息（通过 class 名和字段名） */
ClassFieldInfo* lumyr_class_find_field(const char* class_name, const char* field_name)
{
    ClassInfo* ci = lumyr_class_lookup(class_name);
    if(!ci || !field_name) return NULL;
    for(int i = 0; i < ci->nfields; i++) {
        if(strcmp(ci->fields[i].name, field_name) == 0) {
            return &ci->fields[i];
        }
    }
    return NULL;
}

/* 获取 class 实例的 class 名（通过 vtable 指针）
   vtable 指针总是在结构体的偏移量 0 的位置（无论继承链有多深，
   因为嵌套结构体的字段和直接字段的偏移量是一样的）
   vtable 的第一个字段是 class_name */
const char* lumyr_class_get_name(Value obj)
{
    if(obj.type != VAL_STRUCT_PTR || !obj.v.struct_ptr) return NULL;
    /* vtable 指针在偏移量 0 的位置 */
    void** vtable_ptr = (void**)obj.v.struct_ptr;
    if(!*vtable_ptr) return NULL;
    /* vtable 的第一个字段是 class_name */
    const char** class_name_ptr = (const char**)*vtable_ptr;
    return *class_name_ptr;
}

/* 判断一个 VAL_STRUCT_PTR 是不是 class 实例（通过 vtable 指针和 class 红黑树判断）
   class 的第一个字段是 vtable 指针，vtable 的第一个字段是 class_name
   struct 的第一个字段是 __structname__（直接是 const char*）
   区分方法：尝试通过 vtable 获取 class 名，如果能在 class 红黑树中找到，就是 class */
int lumyr_is_class_instance(Value obj)
{
    if(obj.type != VAL_STRUCT_PTR || !obj.v.struct_ptr) return 0;
    /* class 的第一个字段是 vtable 指针，vtable 的第一个字段是 class_name */
    void** vtable_ptr = (void**)obj.v.struct_ptr;
    if(!*vtable_ptr) return 0;
    const char** class_name_ptr = (const char**)*vtable_ptr;
    if(!*class_name_ptr) return 0;
    /* 检查这个 class 名是否在 class 红黑树中 */
    ensure_class_tree();
    ClassRBNode* node = class_rb_find_node(g_class_tree, *class_name_ptr);
    return node ? 1 : 0;
}

/* class 属性读取（专门针对 class 的函数，不依赖通用的 lumyr_index_get） */
Value lumyr_class_get_field(Value obj, const char* field_name)
{
    if(obj.type != VAL_STRUCT_PTR || !obj.v.struct_ptr || !field_name) {
        runtime_error("class 属性读取：对象不是 class 实例或字段名为空");
        return val_none();
    }
    /* 特殊处理 __classname__ 只读属性 */
    if(strcmp(field_name, "__classname__") == 0) {
        const char* name = lumyr_class_get_name(obj);
        return lumyr_make_string(name ? name : "");
    }
    /* 获取 class 名 */
    const char* class_name = lumyr_class_get_name(obj);
    if(!class_name) {
        runtime_error("class 属性读取：无法获取 class 名");
        return val_none();
    }
    /* 查找字段信息（O(log n) 红黑树查找 + O(n) 字段遍历，字段数量通常不多） */
    ClassFieldInfo* fi = lumyr_class_find_field(class_name, field_name);
    if(!fi) {
        char buf[256];
        snprintf(buf, sizeof(buf), "class 属性读取：class '%s' 没有字段 '%s'", class_name, field_name);
        runtime_error(buf);
        return val_none();
    }
    /* 根据字段类型读取字段值 */
    char* field_ptr = (char*)obj.v.struct_ptr + fi->offset;
    switch(fi->type) {
        case CLASS_FIELD_INT:
            return lumyr_make_int(*(long long*)field_ptr);
        case CLASS_FIELD_DOUBLE:
            return lumyr_make_double(*(double*)field_ptr);
        case CLASS_FIELD_STRING:
            return lumyr_make_string(*(const char**)field_ptr ? *(const char**)field_ptr : "");
        case CLASS_FIELD_BOOL:
            return lumyr_make_bool(*(int*)field_ptr ? 1 : 0);
        case CLASS_FIELD_PTR: {
            /* 指针类型（struct/class/array/map/func），直接返回指针包装成 Value */
            Value v;
            v.type = VAL_STRUCT_PTR;
            v.v.struct_ptr = *(void**)field_ptr;
            return v;
        }
        default:
            runtime_error("class 属性读取：不支持的字段类型");
            return val_none();
    }
}

/* class 属性写入（专门针对 class 的函数） */
void lumyr_class_set_field(Value obj, const char* field_name, Value value)
{
    if(obj.type != VAL_STRUCT_PTR || !obj.v.struct_ptr || !field_name) {
        runtime_error("class 属性写入：对象不是 class 实例或字段名为空");
        return;
    }
    /* __classname__ 是只读属性，不允许修改 */
    if(strcmp(field_name, "__classname__") == 0) {
        runtime_error("class 属性写入：__classname__ 是只读属性");
        return;
    }
    /* 获取 class 名 */
    const char* class_name = lumyr_class_get_name(obj);
    if(!class_name) {
        runtime_error("class 属性写入：无法获取 class 名");
        return;
    }
    /* 查找字段信息 */
    ClassFieldInfo* fi = lumyr_class_find_field(class_name, field_name);
    if(!fi) {
        char buf[256];
        snprintf(buf, sizeof(buf), "class 属性写入：class '%s' 没有字段 '%s'", class_name, field_name);
        runtime_error(buf);
        return;
    }
    /* 根据字段类型写入字段值 */
    char* field_ptr = (char*)obj.v.struct_ptr + fi->offset;
    switch(fi->type) {
        case CLASS_FIELD_INT:
            *(long long*)field_ptr = value.v.i;
            break;
        case CLASS_FIELD_DOUBLE:
            *(double*)field_ptr = value.type == VAL_DOUBLE ? value.v.d : (double)value.v.i;
            break;
        case CLASS_FIELD_STRING:
            /* 字符串字段需要深拷贝 */
            if(*(char**)field_ptr) free(*(char**)field_ptr);
            *(char**)field_ptr = strdup(lumyr_str_cstr(&value));
            break;
        case CLASS_FIELD_BOOL:
            *(int*)field_ptr = value.type == VAL_BOOL ? value.v.b : (value.v.i ? 1 : 0);
            break;
        case CLASS_FIELD_PTR:
            *(void**)field_ptr = value.v.struct_ptr;
            break;
        default:
            runtime_error("class 属性写入：不支持的字段类型");
            break;
    }
}

/* class 方法调用（专门针对 class 的函数，通过 vtable 调用）
   注意：这个函数目前是占位实现，实际的方法调用通过 vtable 函数指针直接调用，
   在代码生成时已经内联了，不需要通过这个函数。
   这个函数主要用于没有类型标记的变量的方法调用（如函数参数）。 */
Value lumyr_class_call_method(Value obj, const char* method_name, int argc, Value* args)
{
    (void)obj;
    (void)method_name;
    (void)argc;
    (void)args;
    /* 占位实现：实际的方法调用在代码生成时通过 vtable 函数指针直接调用
       这个函数主要用于未来扩展，目前不需要实现 */
    runtime_error("class 方法调用：请使用 vtable 直接调用");
    return val_none();
}
