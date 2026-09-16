#include "lm_class.h"
#ifdef _WIN32
#include <windows.h>
#endif
#include "lm_value.h"
#include "lm_runtime.h"
#include "lm_struct.h"
#include <string.h>
#include <stdlib.h>

/* ==================== VM 特定函数的弱符号存根（CC 模式下使用） ==================== */
/* 这些函数在 vm.c 中定义，CC 模式下没有链接 vm.c，所以提供弱符号存根 */
/* 前向声明 */
typedef struct RuntimeFunc RuntimeFunc;
typedef struct EvalCtx EvalCtx;
typedef struct StackFrame StackFrame;

__attribute__((weak)) RuntimeFunc* interp_set_current_rf(RuntimeFunc* rf) {
    (void)rf;
    return NULL;
}

__attribute__((weak)) Value vm_func_entry(int arg_cnt, const Value* args, EvalCtx* ctx, StackFrame* frame) {
    (void)arg_cnt;
    (void)args;
    (void)ctx;
    (void)frame;
    return val_none();
}

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

/* 当前执行的函数所属的类名（NULL 表示全局函数或非类方法，用于访问修饰符检查） */
static const char* g_current_class_name = NULL;

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
void lumyr_class_register(const char* class_name, int nfields, ClassFieldInfo* fields, void* vtable, int ninterfaces, const char** interfaces)
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
            existing->value->ninterfaces = ninterfaces;
            existing->value->interfaces = interfaces;
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
    ci->ninterfaces = ninterfaces;
    ci->interfaces = interfaces;
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
    if(obj.type != VAL_CLASS_PTR || !obj.v.struct_ptr) return NULL;
    /* vtable 指针在偏移量 0 的位置 */
    void** vtable_ptr = (void**)obj.v.struct_ptr;
    if(!*vtable_ptr) return NULL;
    /* vtable 的第一个字段是 class_name */
    const char** class_name_ptr = (const char**)*vtable_ptr;
    return *class_name_ptr;
}

/* 判断一个 Value 是不是 class 实例（直接检查 type 字段）
   类型拆分后，class 实例使用 VAL_CLASS_PTR 类型，struct 实例使用 VAL_STRUCT_PTR 类型
   不再需要通过检查 vtable 指针和 class 红黑树来区分 */
int lumyr_is_class_instance(Value obj)
{
    return (obj.type == VAL_CLASS_PTR && obj.v.struct_ptr) ? 1 : 0;
}

/* class 属性读取（专门针对 class 的函数，不依赖通用的 lumyr_index_get） */
Value lumyr_class_get_field(Value obj, const char* field_name)
{
    if(obj.type != VAL_CLASS_PTR || !obj.v.struct_ptr || !field_name) {
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
    /* 访问修饰符检查 */
    if(fi->access_modifier == CLASS_ACCESS_PRIVATE) {
        /* private 属性：只有类内部的方法才能访问 */
        if(!lumyr_is_accessor_inside_class(class_name)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "class 属性读取：字段 '%s' 是 private，不允许外部访问", field_name);
            runtime_error(buf);
            return val_none();
        }
    } else if(fi->access_modifier == CLASS_ACCESS_PROTECTED) {
        /* protected 属性：类内部和子类的方法才能访问 */
        if(!lumyr_is_accessor_inside_class(class_name) && !lumyr_is_accessor_subclass_of(class_name)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "class 属性读取：字段 '%s' 是 protected，不允许外部访问", field_name);
            runtime_error(buf);
            return val_none();
        }
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
            v.type = VAL_CLASS_PTR;
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
    if(obj.type != VAL_CLASS_PTR || !obj.v.struct_ptr || !field_name) {
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
    /* 访问修饰符检查 */
    if(fi->access_modifier == CLASS_ACCESS_PRIVATE) {
        /* private 属性：只有类内部的方法才能访问 */
        if(!lumyr_is_accessor_inside_class(class_name)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "class 属性写入：字段 '%s' 是 private，不允许外部访问", field_name);
            runtime_error(buf);
            return;
        }
    } else if(fi->access_modifier == CLASS_ACCESS_PROTECTED) {
        /* protected 属性：类内部和子类的方法才能访问 */
        if(!lumyr_is_accessor_inside_class(class_name) && !lumyr_is_accessor_subclass_of(class_name)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "class 属性写入：字段 '%s' 是 protected，不允许外部访问", field_name);
            runtime_error(buf);
            return;
        }
    }
    /* 根据字段类型写入字段值 */
    char* field_ptr = (char*)obj.v.struct_ptr + fi->offset;
    switch(fi->type) {
        case CLASS_FIELD_INT:
            *(long long*)field_ptr = lumyr_extract_ll(value);
            break;
        case CLASS_FIELD_DOUBLE:
            *(double*)field_ptr = value.type == VAL_DOUBLE ? value.v.d : (double)lumyr_extract_ll(value);
            break;
        case CLASS_FIELD_STRING:
            /* 字符串字段需要深拷贝 */
            if(*(char**)field_ptr) free(*(char**)field_ptr);
            *(char**)field_ptr = strdup(lumyr_str_cstr(&value));
            break;
        case CLASS_FIELD_BOOL:
            *(int*)field_ptr = value.type == VAL_BOOL ? value.v.b : (lumyr_extract_ll(value) ? 1 : 0);
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

/* 判断 class 是否实现了某个接口（包括父类实现的接口） */
int lumyr_class_implements_interface(const char* class_name, const char* interface_name)
{
    if(!class_name || !interface_name) return 0;
    ensure_class_tree();
    const char* cur = class_name;
    /* 沿继承链向上查找 */
    while(cur) {
        ClassRBNode* node = class_rb_find_node(g_class_tree, cur);
        if(!node || !node->value) break;
        ClassInfo* ci = (ClassInfo*)node->value;
        /* 检查当前类实现的接口 */
        for(int i = 0; i < ci->ninterfaces; i++) {
            if(ci->interfaces[i] && strcmp(ci->interfaces[i], interface_name) == 0) {
                return 1;
            }
        }
        /* 检查父类（通过 vtable 获取父类名，或者通过其他方式）
           目前 ClassInfo 没有记录父类名，暂时只检查当前类的接口
           后续可以添加 parent_name 字段支持继承链查找 */
        break;
    }
    return 0;
}

/* 判断对象是否实现了某个接口（对象必须是 class 实例） */
int lumyr_obj_implements_interface(Value obj, const char* interface_name)
{
    if(obj.type != VAL_CLASS_PTR || !obj.v.struct_ptr) return 0;
    if(!lumyr_is_class_instance(obj)) return 0;
    const char* class_name = lumyr_class_get_name(obj);
    if(!class_name) return 0;
    return lumyr_class_implements_interface(class_name, interface_name);
}

/* ==================== 访问者判断（用于访问修饰符检查） ==================== */

/* 设置当前执行的函数所属的类名（NULL 表示全局函数或非类方法） */
void lumyr_set_current_class(const char* class_name)
{
    g_current_class_name = class_name;
}

/* 获取当前执行的函数所属的类名（NULL 表示全局函数或非类方法） */
const char* lumyr_get_current_class(void)
{
    return g_current_class_name;
}

/* 判断当前访问者是否是指定类的内部（即当前执行的函数是该类的方法） */
int lumyr_is_accessor_inside_class(const char* class_name)
{
    if(!g_current_class_name || !class_name) return 0;
    return strcmp(g_current_class_name, class_name) == 0;
}

/* 判断当前访问者是否是指定类的子类（即当前执行的函数是该类的子类的方法） */
int lumyr_is_accessor_subclass_of(const char* class_name)
{
    if(!g_current_class_name || !class_name) return 0;
    if(strcmp(g_current_class_name, class_name) == 0) return 0;  /* 自己不是自己的子类 */
    /* 遍历当前类的继承链，看看是否继承自指定类 */
    ClassInfo* ci = lumyr_class_lookup(g_current_class_name);
    if(!ci) return 0;
    /* TODO: 需要在 ClassInfo 中保存父类信息，目前暂时返回 0，后续完善 */
    return 0;
}

/* 通用的接口判断函数（可以处理 class 实例和 map 类型的对象）
   用于 CC 模式的代码生成，避免使用编译器内部的 TypeDef 和 type_lookup */
int lumyr_implements_interface(Value obj, const char* iface_name)
{
    if(!iface_name) return 0;
    /* class 实例：通过 lumyr_obj_implements_interface 判断 */
    if(obj.type == VAL_CLASS_PTR && obj.v.struct_ptr) {
        return lumyr_obj_implements_interface(obj, iface_name);
    }
    /* 其他类型（包括 map）：暂时返回 0，后续可以在运行时添加 type 接口信息表 */
    return 0;
}

/* 接口类型转换：检查对象是否实现了接口，如果没有实现则报错，否则返回对象本身 */
Value lumyr_interface_cast(Value obj, const char* iface_name) {
    if(!iface_name) {
        runtime_error("接口类型转换：接口名为空");
        return val_none();
    }
    if(lumyr_implements_interface(obj, iface_name)) {
        return obj;
    }
    /* 获取对象的类型名，用于错误信息 */
    const char* type_name = "unknown";
    if(obj.type == VAL_CLASS_PTR && obj.v.struct_ptr) {
        type_name = lumyr_class_get_name(obj);
    } else if(obj.type == VAL_STRUCT_PTR && obj.v.struct_ptr) {
        type_name = lumyr_struct_get_name(obj);
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "接口类型转换失败：类型 \"%s\" 未实现接口 \"%s\"", type_name, iface_name);
    runtime_error(msg);
    return val_none();
}


/* ==================== VM 模式下的 class 实例（结构体 + 虚表） ==================== */

/* class 虚表红黑树（用于存储 ClassVTable，大型项目 class 特别多时 O(log n) 查找） */
typedef struct ClassVTableRBNode {
    char* key;
    ClassVTable* value;
    int color;
    struct ClassVTableRBNode* left;
    struct ClassVTableRBNode* right;
    struct ClassVTableRBNode* parent;
} ClassVTableRBNode;

typedef struct {
    ClassVTableRBNode* root;
    ClassVTableRBNode* nil;
    int count;
} ClassVTableRBTree;

static ClassVTableRBTree* g_vtable_tree = NULL;

static ClassVTableRBNode* vtable_rb_create_node(const char* key, ClassVTable* value)
{
    ClassVTableRBNode* node = (ClassVTableRBNode*)malloc(sizeof(ClassVTableRBNode));
    if(!node) return NULL;
    node->key = strdup(key);
    node->value = value;
    node->color = 0;
    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;
    return node;
}

static void vtable_rb_left_rotate(ClassVTableRBTree* tree, ClassVTableRBNode* x)
{
    ClassVTableRBNode* y = x->right;
    x->right = y->left;
    if(y->left != tree->nil) y->left->parent = x;
    y->parent = x->parent;
    if(x->parent == tree->nil) tree->root = y;
    else if(x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void vtable_rb_right_rotate(ClassVTableRBTree* tree, ClassVTableRBNode* y)
{
    ClassVTableRBNode* x = y->left;
    y->left = x->right;
    if(x->right != tree->nil) x->right->parent = y;
    x->parent = y->parent;
    if(y->parent == tree->nil) tree->root = x;
    else if(y == y->parent->right) y->parent->right = x;
    else y->parent->left = x;
    x->right = y;
    y->parent = x;
}

static void vtable_rb_insert(ClassVTableRBTree* tree, const char* key, ClassVTable* value)
{
    if(!tree->nil) {
        tree->nil = (ClassVTableRBNode*)malloc(sizeof(ClassVTableRBNode));
        tree->nil->color = 1;
        tree->nil->left = tree->nil->right = tree->nil->parent = NULL;
        tree->root = tree->nil;
    }
    ClassVTableRBNode* z = vtable_rb_create_node(key, value);
    ClassVTableRBNode* y = tree->nil;
    ClassVTableRBNode* x = tree->root;
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
    /* 简化：跳过红黑树修复（后续完善） */
    tree->count++;
}

static ClassVTable* vtable_rb_search(ClassVTableRBTree* tree, const char* key)
{
    if(!tree || !tree->nil) return NULL;
    ClassVTableRBNode* x = tree->root;
    while(x != tree->nil) {
        int cmp = strcmp(key, x->key);
        if(cmp == 0) return x->value;
        else if(cmp < 0) x = x->left;
        else x = x->right;
    }
    return NULL;
}

/* 注册 class 虚表（VM 模式下使用） */
void lumyr_class_vtable_register(ClassVTable* vtable)
{
    if(!vtable || !vtable->class_name) return;
    if(!g_vtable_tree) {
        g_vtable_tree = (ClassVTableRBTree*)malloc(sizeof(ClassVTableRBTree));
        g_vtable_tree->root = NULL;
        g_vtable_tree->nil = NULL;
        g_vtable_tree->count = 0;
    }
    vtable_rb_insert(g_vtable_tree, vtable->class_name, vtable);
    /* 同时注册 class 信息到 g_class_tree，用于接口判断等 */
    lumyr_class_register(vtable->class_name, vtable->nfields, NULL, vtable,
                         vtable->ninterfaces, vtable->interfaces);
}

/* 查找 class 虚表（通过 class 名） */
ClassVTable* lumyr_class_vtable_lookup(const char* class_name)
{
    if(!class_name || !g_vtable_tree) return NULL;
    return vtable_rb_search(g_vtable_tree, class_name);
}

/* 创建 class 实例（分配结构体内存，设置 vtable 指针） */
Value lumyr_class_instance_new(const char* class_name)
{
    ClassVTable* vt = lumyr_class_vtable_lookup(class_name);
    if(!vt) {
        char buf[256];
        snprintf(buf, sizeof(buf), "class 虚表未注册：%s", class_name);
        runtime_error(buf);
        return val_none();
    }
    /* 分配结构体内存（包含 vtable 指针 + 字段数据） */
    int size = sizeof(ClassVTable*) + vt->instance_size;
    void* ptr = malloc(size);
    if(!ptr) {
        runtime_error("内存分配失败：无法创建 class 实例");
        return val_none();
    }
    memset(ptr, 0, size);
    /* 设置 vtable 指针 */
    ClassInstance* inst = (ClassInstance*)ptr;
    inst->vtable = vt;
    /* 返回 VAL_STRUCT_PTR 类型 */
    Value v;
    v.type = VAL_CLASS_PTR;
    v.v.struct_ptr = ptr;
    return v;
}

/* 从 vtable 中查找方法（返回 RuntimeFunc* 或 NULL） */
static RuntimeFunc* class_vtable_find_method(ClassVTable* vt, const char* method_name)
{
    if(!vt || !method_name) return NULL;
    /* 先在当前类的方法表中查找 */
    for(int i = 0; i < vt->nmethods; i++) {
        if(vt->method_names && strcmp(vt->method_names[i], method_name) == 0) {
            return vt->methods[i];
        }
    }
    /* 再在父类的方法表中查找（递归） */
    if(vt->parent) {
        return class_vtable_find_method(vt->parent, method_name);
    }
    return NULL;
}

/* class 实例属性读取（按偏移量访问） */
Value lumyr_class_instance_get_field(Value obj, const char* field_name)
{
    if(obj.type != VAL_CLASS_PTR || !obj.v.struct_ptr || !field_name) {
        return val_none();
    }
    ClassInstance* inst = (ClassInstance*)obj.v.struct_ptr;
    ClassVTable* vt = inst->vtable;
    if(!vt) return val_none();
    /* 特殊处理 __classname__ 只读属性 */
    if(strcmp(field_name, "__classname__") == 0) {
        return lumyr_make_string(vt->class_name ? vt->class_name : "");
    }
    /* 查找字段偏移量 */
    for(int i = 0; i < vt->nfields; i++) {
        if(strcmp(vt->field_names[i], field_name) == 0) {
            /* 按偏移量读取字段 */
            char* field_ptr = (char*)obj.v.struct_ptr + sizeof(ClassVTable*) + vt->field_offsets[i];
            switch(vt->field_types[i]) {
                case CLASS_FIELD_INT: {
                    long long val = *(long long*)field_ptr;
                    return val_int(val);
                }
                case CLASS_FIELD_DOUBLE: {
                    double val = *(double*)field_ptr;
                    return val_double(val);
                }
                case CLASS_FIELD_BOOL: {
                    int val = *(int*)field_ptr;
                    return val_bool(val);
                }
                case CLASS_FIELD_STRING: {
                    const char* val = *(const char**)field_ptr;
                    if(val) return lumyr_make_string(val);
                    return val_none();
                }
                case CLASS_FIELD_PTR: {
                    void* val = *(void**)field_ptr;
                    Value v;
                    v.type = VAL_CLASS_PTR;
                    v.v.struct_ptr = val;
                    return v;
                }
                default:
                    return val_none();
            }
        }
    }
    return val_none();
}

/* class 实例属性写入（按偏移量访问） */
void lumyr_class_instance_set_field(Value obj, const char* field_name, Value value)
{
    if(obj.type != VAL_CLASS_PTR || !obj.v.struct_ptr || !field_name) return;
    ClassInstance* inst = (ClassInstance*)obj.v.struct_ptr;
    ClassVTable* vt = inst->vtable;
    if(!vt) return;
    /* 查找字段偏移量 */
    for(int i = 0; i < vt->nfields; i++) {
        if(strcmp(vt->field_names[i], field_name) == 0) {
            /* 按偏移量写入字段 */
            char* field_ptr = (char*)obj.v.struct_ptr + sizeof(ClassVTable*) + vt->field_offsets[i];
            switch(vt->field_types[i]) {
                case CLASS_FIELD_INT:
                    *(long long*)field_ptr = lumyr_extract_ll(value);
                    break;
                case CLASS_FIELD_DOUBLE:
                    *(double*)field_ptr = value.v.d;
                    break;
                case CLASS_FIELD_BOOL:
                    *(int*)field_ptr = value.v.b;
                    break;
                case CLASS_FIELD_STRING:
                    if(value.type == VAL_STRING) {
                        /* 字符串字段需要深拷贝，避免指向栈上临时变量的无效指针 */
                        if(*(char**)field_ptr) free(*(char**)field_ptr);
                        *(char**)field_ptr = strdup(lumyr_str_cstr(&value));
                    }
                    break;
                case CLASS_FIELD_PTR:
                    *(void**)field_ptr = value.v.struct_ptr;
                    break;
                default:
                    break;
            }
            return;
        }
    }
}

/* class 实例方法调用（通过 vtable 索引调用） */
Value lumyr_class_instance_call_method(Value obj, const char* method_name, int argc, Value* args, EvalCtx* ctx, StackFrame* frame)
{
    if(obj.type != VAL_CLASS_PTR || !obj.v.struct_ptr || !method_name) {
        return val_none();
    }
    ClassInstance* inst = (ClassInstance*)obj.v.struct_ptr;
    ClassVTable* vt = inst->vtable;
    if(!vt) return val_none();
    /* 查找方法索引 */
    for(int i = 0; i < vt->nmethods; i++) {
        if(strcmp(vt->method_names[i], method_name) == 0) {
            /* 通过 vtable 调用方法 */
            if(vt->methods[i]) {
                /* 构造参数数组：self 作为第一个参数 */
                Value* call_args = (Value*)malloc(sizeof(Value) * (argc + 1));
                call_args[0] = obj;
                for(int j = 0; j < argc; j++) {
                    call_args[j + 1] = args[j];
                }
                /* 设置当前函数上下文，然后调用 vm_func_entry */
                RuntimeFunc* prev_rf = interp_set_current_rf(vt->methods[i]);
                Value ret = vm_func_entry(argc + 1, call_args, ctx, frame);
                interp_set_current_rf(prev_rf);
                free(call_args);
                return ret;
            }
            return val_none();
        }
    }
    /* 查找父类方法 */
    if(vt->parent) {
        /* 临时设置 vtable 为父类，递归调用 */
        ClassVTable* old_vt = inst->vtable;
        inst->vtable = vt->parent;
        Value ret = lumyr_class_instance_call_method(obj, method_name, argc, args, ctx, frame);
        inst->vtable = old_vt;
        return ret;
    }
    return val_none();
}

/* 判断一个 Value 是不是 class 实例（直接检查 type 字段）
   类型拆分后，class 实例使用 VAL_CLASS_PTR 类型，不再需要检查 vtable */
int lumyr_is_class_instance_value(Value obj)
{
    return (obj.type == VAL_CLASS_PTR && obj.v.struct_ptr) ? 1 : 0;
}
