/* lm_type.c — 统一 struct/class/type 运行时类型系统
 * 替代旧 lm_struct.c + lm_class.c 的全部重复实现
 */
#include "lm_type.h"
#include "lm_value.h"
#include "gc_runtime.h"
#include "lumyr_value.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ==================== 红黑树（类型注册表） ==================== */

typedef struct TypeRBNode {
    char* key;
    RuntimeTypeInfo* value;
    int color;  /* 0=红, 1=黑 */
    struct TypeRBNode* left, *right, *parent;
} TypeRBNode;

typedef struct {
    TypeRBNode* root;
    TypeRBNode* nil;
    int count;
} TypeRBTree;

static TypeRBTree* g_type_tree = NULL;

static TypeRBNode* type_rb_create_node(const char* key, RuntimeTypeInfo* value)
{
    TypeRBNode* node = (TypeRBNode*)malloc(sizeof(TypeRBNode));
    if(!node) return NULL;
    node->key = strdup(key);
    node->value = value;
    node->color = 0;
    node->left = node->right = node->parent = NULL;
    return node;
}

static void type_rb_left_rotate(TypeRBTree* t, TypeRBNode* x)
{
    TypeRBNode* y = x->right;
    x->right = y->left;
    if(y->left != t->nil) y->left->parent = x;
    y->parent = x->parent;
    if(x->parent == t->nil) t->root = y;
    else if(x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void type_rb_right_rotate(TypeRBTree* t, TypeRBNode* y)
{
    TypeRBNode* x = y->left;
    y->left = x->right;
    if(x->right != t->nil) x->right->parent = y;
    x->parent = y->parent;
    if(y->parent == t->nil) t->root = x;
    else if(y == y->parent->right) y->parent->right = x;
    else y->parent->left = x;
    x->right = y;
    y->parent = x;
}

static void type_rb_insert_fixup(TypeRBTree* t, TypeRBNode* z)
{
    while(z->parent && z->parent->color == 0) {
        if(z->parent == z->parent->parent->left) {
            TypeRBNode* y = z->parent->parent->right;
            if(y && y->color == 0) {
                z->parent->color = 1; y->color = 1;
                z->parent->parent->color = 0;
                z = z->parent->parent;
            } else {
                if(z == z->parent->right) { z = z->parent; type_rb_left_rotate(t, z); }
                z->parent->color = 1; z->parent->parent->color = 0;
                type_rb_right_rotate(t, z->parent->parent);
            }
        } else {
            TypeRBNode* y = z->parent->parent->left;
            if(y && y->color == 0) {
                z->parent->color = 1; y->color = 1;
                z->parent->parent->color = 0;
                z = z->parent->parent;
            } else {
                if(z == z->parent->left) { z = z->parent; type_rb_right_rotate(t, z); }
                z->parent->color = 1; z->parent->parent->color = 0;
                type_rb_left_rotate(t, z->parent->parent);
            }
        }
    }
    t->root->color = 1;
}

static void type_rb_insert(TypeRBTree* t, const char* key, RuntimeTypeInfo* value)
{
    if(!t || !key) return;
    TypeRBNode* z = type_rb_create_node(key, value);
    if(!z) return;
    TypeRBNode* y = t->nil;
    TypeRBNode* x = t->root;
    while(x != t->nil) {
        y = x;
        if(strcmp(z->key, x->key) < 0) x = x->left;
        else x = x->right;
    }
    z->parent = y;
    if(y == t->nil) t->root = z;
    else if(strcmp(z->key, y->key) < 0) y->left = z;
    else y->right = z;
    z->left = t->nil; z->right = t->nil; z->color = 0;
    type_rb_insert_fixup(t, z);
    t->count++;
}

static TypeRBNode* type_rb_find_node(TypeRBTree* t, const char* key)
{
    if(!t || !key) return NULL;
    TypeRBNode* x = t->root;
    while(x != t->nil) {
        int cmp = strcmp(key, x->key);
        if(cmp == 0) return x;
        x = cmp < 0 ? x->left : x->right;
    }
    return NULL;
}

static void type_rb_free_subtree(TypeRBTree* t, TypeRBNode* n)
{
    if(!n || n == t->nil) return;
    type_rb_free_subtree(t, n->left);
    type_rb_free_subtree(t, n->right);
    free(n->key);
    free(n);
}

/* ==================== 统一 API 实现 ==================== */

RuntimeTypeInfo* lumyr_type_register(
    const char* name, int nfields, FieldInfo* fields, int instance_size,
    TypeKind kind, RuntimeFunc** methods, int nmethods, const char** method_names,
    struct RuntimeTypeInfo* parent, int ninterfaces, const char** interfaces,
    uint8_t is_abstract)
{
    if(!name) return NULL;

    /* 首次调用初始化树 */
    if(!g_type_tree) {
        g_type_tree = (TypeRBTree*)malloc(sizeof(TypeRBTree));
        g_type_tree->nil = (TypeRBNode*)calloc(1, sizeof(TypeRBNode));
        g_type_tree->nil->color = 1;
        g_type_tree->root = g_type_tree->nil;
        g_type_tree->count = 0;
    }

    /* 创建 RuntimeTypeInfo */
    RuntimeTypeInfo* info = (RuntimeTypeInfo*)calloc(1, sizeof(RuntimeTypeInfo));
    info->name = strdup(name);
    info->nfields = nfields;
    info->fields = fields;
    info->instance_size = instance_size;
    info->kind = kind;
    info->parent = parent;
    info->ninterfaces = ninterfaces;
    info->interfaces = interfaces;
    info->is_abstract = is_abstract;

    /* 方法表初始化：
     * - 有 parent：深拷贝父类方法表（含继承链全部方法），子类方法稍后通过 add 覆盖原位置 → 多态
     * - 无 parent：使用调用方显式传入的方法表（通常为空） */
    if(parent && parent->nmethods > 0) {
        info->nmethods = parent->nmethods;
        info->methods = (RuntimeFunc**)malloc((size_t)parent->nmethods * sizeof(RuntimeFunc*));
        info->method_names = (const char**)malloc((size_t)parent->nmethods * sizeof(const char*));
        for(int i = 0; i < parent->nmethods; i++) {
            info->methods[i] = parent->methods[i];
            info->method_names[i] = parent->method_names[i] ? strdup(parent->method_names[i]) : NULL;
        }
    } else {
        info->nmethods = nmethods;
        info->methods = methods;
        info->method_names = method_names;
    }

    /* 注册到树（按名，仅用于 type() 显示等反射场景） */
    type_rb_insert(g_type_tree, name, info);

    return info;
}

RuntimeTypeInfo* lumyr_type_lookup(const char* name)
{
    if(!g_type_tree || !name) return NULL;
    TypeRBNode* n = type_rb_find_node(g_type_tree, name);
    return n ? n->value : NULL;
}

FieldInfo* lumyr_type_find_field(RuntimeTypeInfo* info, const char* field_name)
{
    if(!info || !field_name || !info->fields) return NULL;
    for(int i = 0; i < info->nfields; i++) {
        if(info->fields[i].name && strcmp(info->fields[i].name, field_name) == 0)
            return &info->fields[i];
    }
    /* 父类字段已在扁平化合并时包含，无需遍历 parent */
    return NULL;
}

/* 查找方法实现：方法表在注册时已扁平化包含父类方法（覆盖在原位置），直接查表。
 * 完整性由 lumyr_type_set_method 的后代传播保证（见下），任何注册顺序下
 * "后代方法表 ⊇ 祖先方法表" 恒成立，故此处无需沿 parent 链兜底。 */
RuntimeFunc* lumyr_type_find_method(RuntimeTypeInfo* info, const char* method_name)
{
    if(!info || !method_name) return NULL;
    for(int i = 0; i < info->nmethods; i++) {
        if(info->method_names[i] && strcmp(info->method_names[i], method_name) == 0)
            return info->methods[i];
    }
    return NULL;
}

/* ============ 方法表后代传播辅助 ============ */

/* 中序收集注册表全部类型（红黑树遍历） */
static void type_rb_collect(TypeRBNode* n, TypeRBNode* nil,
                            RuntimeTypeInfo*** arr, int* cnt, int* cap) {
    if(!n || n == nil) return;
    type_rb_collect(n->left, nil, arr, cnt, cap);
    if(*cnt >= *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *arr = (RuntimeTypeInfo**)realloc(*arr, (size_t)*cap * sizeof(RuntimeTypeInfo*));
    }
    (*arr)[(*cnt)++] = n->value;
    type_rb_collect(n->right, nil, arr, cnt, cap);
}

/* t 是否为 info 的后代（沿 parent 链上溯命中 info） */
static int type_is_descendant(const RuntimeTypeInfo* t, const RuntimeTypeInfo* info) {
    for(const RuntimeTypeInfo* p = t ? t->parent : NULL; p; p = p->parent)
        if(p == info) return 1;
    return 0;
}

/* 设置方法：同名覆盖（重写），新名追加到方法表 */
void lumyr_type_set_method(RuntimeTypeInfo* info, const char* method_name, RuntimeFunc* rf)
{
    if(!info || !method_name) return;
    for(int i = 0; i < info->nmethods; i++) {
        if(info->method_names[i] && strcmp(info->method_names[i], method_name) == 0) {
            info->methods[i] = rf;   /* 方法重写：替换实现，方法名位置不变 */
            return;
        }
    }
    int n = info->nmethods + 1;
    info->methods = (RuntimeFunc**)realloc(info->methods, (size_t)n * sizeof(RuntimeFunc*));
    info->method_names = (const char**)realloc(info->method_names, (size_t)n * sizeof(const char*));
    info->methods[info->nmethods] = rf;
    info->method_names[info->nmethods] = strdup(method_name);
    info->nmethods = n;

    /* 根因修复：新增方法传播到全部已注册后代。
     * 子类注册时仅深拷贝父类"当时"的方法表（flatten-at-register），
     * 父类此后新增的方法对子类不可见 → 多态分派缺失（子类实例调父类方法报"没有方法"）。
     * 传播使 "后代方法表 ⊇ 祖先方法表" 在任何注册顺序下恒成立。
     * 后代已有同名方法（重写）→ 保留后代实现，不覆盖（多态语义）。 */
    if(g_type_tree) {
        RuntimeTypeInfo** all = NULL;
        int cnt = 0, cap = 0;
        type_rb_collect(g_type_tree->root, g_type_tree->nil, &all, &cnt, &cap);
        for(int i = 0; i < cnt; i++) {
            RuntimeTypeInfo* t = all[i];
            if(t == info || !type_is_descendant(t, info)) continue;
            int has = 0;
            for(int k = 0; k < t->nmethods; k++) {
                if(t->method_names[k] && strcmp(t->method_names[k], method_name) == 0) { has = 1; break; }
            }
            if(has) continue;
            int tn = t->nmethods + 1;
            t->methods = (RuntimeFunc**)realloc(t->methods, (size_t)tn * sizeof(RuntimeFunc*));
            t->method_names = (const char**)realloc(t->method_names, (size_t)tn * sizeof(const char*));
            t->methods[t->nmethods] = rf;
            t->method_names[t->nmethods] = strdup(method_name);
            t->nmethods = tn;
        }
        free(all);
    }
}

/* ==================== 实例创建 ==================== */

Value lumyr_instance_new(RuntimeTypeInfo* info)
{
    if(!info) {
        runtime_error("instance_new: 类型信息为空");
        return val_none();
    }
    if(info->is_abstract) {
        char buf[256];
        snprintf(buf, sizeof(buf), "instance_new: 抽象类 '%s' 不可实例化", info->name);
        runtime_error(buf);
        return val_none();
    }

    /* GC 分配实例内存 */
    void* mem = gc_alloc(info->instance_size, VAL_STRUCT_PTR);
    if(!mem) {
        runtime_error("instance_new: GC 分配失败");
        return val_none();
    }
    memset(mem, 0, info->instance_size);

    /* 偏移 0 设 RuntimeTypeInfo 指针（统一，struct 和 class 一样） */
    *(RuntimeTypeInfo**)mem = info;

    Value v;
    /* class 实例用 VAL_CLASS_PTR，struct 用 VAL_STRUCT_PTR（区分用于 type() 显示） */
    v.type = (info->kind == TYPE_KIND_CLASS) ? VAL_CLASS_PTR : VAL_STRUCT_PTR;
    v.v.struct_ptr = mem;
    return v;
}

Value lumyr_instance_copy(Value obj)
{
    if((obj.type != VAL_STRUCT_PTR && obj.type != VAL_CLASS_PTR) || !obj.v.struct_ptr) {
        runtime_error("instance_copy: 对象不是 struct/class 实例");
        return val_none();
    }

    RuntimeTypeInfo* info = *(RuntimeTypeInfo**)obj.v.struct_ptr;
    if(!info) {
        runtime_error("instance_copy: 无法获取类型信息");
        return val_none();
    }

    /* GC 分配新实例 + 浅拷贝 */
    void* mem = gc_alloc(info->instance_size, VAL_STRUCT_PTR);
    if(!mem) {
        runtime_error("instance_copy: GC 分配失败");
        return val_none();
    }
    memcpy(mem, obj.v.struct_ptr, info->instance_size);
    /* 偏移 0 的 info 指针随 memcpy 自动复制 */

    Value v;
    v.type = obj.type;   /* 保留原类型（class copy 必须仍是 VAL_CLASS_PTR，否则方法分派失败） */
    v.v.struct_ptr = mem;
    return v;
}

/* ==================== 类型名 / info 获取 ==================== */

const char* lumyr_instance_get_name(Value obj)
{
    if((obj.type != VAL_STRUCT_PTR && obj.type != VAL_CLASS_PTR) || !obj.v.struct_ptr) return NULL;
    RuntimeTypeInfo* info = *(RuntimeTypeInfo**)obj.v.struct_ptr;
    return info ? info->name : NULL;
}

RuntimeTypeInfo* lumyr_instance_get_info(Value obj)
{
    if((obj.type != VAL_STRUCT_PTR && obj.type != VAL_CLASS_PTR) || !obj.v.struct_ptr) return NULL;
    return *(RuntimeTypeInfo**)obj.v.struct_ptr;
}

/* ==================== 实例相等比较 ==================== */

int lumyr_instance_eq(Value a, Value b)
{
    if((a.type != VAL_STRUCT_PTR && a.type != VAL_CLASS_PTR) ||
       (b.type != VAL_STRUCT_PTR && b.type != VAL_CLASS_PTR)) return 0;
    if(a.v.struct_ptr == b.v.struct_ptr) return 1;
    RuntimeTypeInfo* info = *(RuntimeTypeInfo**)a.v.struct_ptr;
    if(!info) return 0;
    RuntimeTypeInfo* info_b = *(RuntimeTypeInfo**)b.v.struct_ptr;
    if(info != info_b) return 0; /* 不同类型不相等 */

    /* 按字段逐个比较 */
    char* base_a = (char*)a.v.struct_ptr;
    char* base_b = (char*)b.v.struct_ptr;
    for(int i = 0; i < info->nfields; i++) {
        FieldInfo* fi = &info->fields[i];
        char* fa = base_a + fi->offset;
        char* fb = base_b + fi->offset;
        int cls = lumyr_etype_stackcls(fi->valtype);
        if(cls == 1) { /* 整型族：按 size 比较 */
            if(memcmp(fa, fb, fi->size) != 0) return 0;
        } else if(cls == 2) { /* 浮点族 */
            if(fi->valtype == VAL_FLOAT) { if(*(float*)fa != *(float*)fb) return 0; }
            else if(fi->valtype == VAL_LONG_DOUBLE) { if(*(long double*)fa != *(long double*)fb) return 0; }
            else { if(*(double*)fa != *(double*)fb) return 0; }
        } else { /* PTR 族 */
            if(*(void**)fa != *(void**)fb) return 0;
        }
    }
    return 1;
}

/* ==================== CC 模式字段读写（VM 模式直接用 FieldInfo） ==================== */

Value lumyr_field_get(Value obj, const char* field_name)
{
    if((obj.type != VAL_STRUCT_PTR && obj.type != VAL_CLASS_PTR) || !obj.v.struct_ptr || !field_name) {
        runtime_error("field_get: 对象不是实例或字段名为空");
        return val_none();
    }
    RuntimeTypeInfo* info = *(RuntimeTypeInfo**)obj.v.struct_ptr;
    if(!info) { runtime_error("field_get: 无法获取类型信息"); return val_none(); }

    FieldInfo* fi = lumyr_type_find_field(info, field_name);
    if(!fi) {
        char buf[256];
        snprintf(buf, sizeof(buf), "field_get: 类型 '%s' 没有字段 '%s'", info->name, field_name);
        runtime_error(buf);
        return val_none();
    }

    char* field_ptr = (char*)obj.v.struct_ptr + fi->offset;
    int cls = lumyr_etype_stackcls(fi->valtype);

    if(cls == 1) { /* 整型族：按 size 精确读取，返回正确 ValueType */
        long long ival;
        switch(fi->valtype) {
            case VAL_INT8:    ival = *(int8_t*)field_ptr; break;
            case VAL_INT16:   ival = *(int16_t*)field_ptr; break;
            case VAL_INT32:   ival = *(int32_t*)field_ptr; break;
            case VAL_INT:     ival = *(int*)field_ptr; break;
            case VAL_INT64:   ival = *(int64_t*)field_ptr; break;
            case VAL_LONG:    ival = *(long*)field_ptr; break;
            case VAL_LONG_LONG: ival = *(long long*)field_ptr; break;
            case VAL_SHORT:   ival = *(short*)field_ptr; break;
            case VAL_UINT8:   ival = *(uint8_t*)field_ptr; break;
            case VAL_UINT16:  ival = *(uint16_t*)field_ptr; break;
            case VAL_UINT32:  ival = *(uint32_t*)field_ptr; break;
            case VAL_UINT:    ival = *(unsigned int*)field_ptr; break;
            case VAL_UINT64:  ival = *(uint64_t*)field_ptr; break;
            case VAL_ULONG:   ival = *(unsigned long*)field_ptr; break;
            case VAL_UCHAR:   ival = *(unsigned char*)field_ptr; break;
            case VAL_USHORT:  ival = *(unsigned short*)field_ptr; break;
            case VAL_SIZE_T:  ival = *(size_t*)field_ptr; break;
            case VAL_SSIZE_T: ival = *(ssize_t*)field_ptr; break;
            case VAL_BOOL:    ival = *(int*)field_ptr ? 1 : 0; break;
            case VAL_CHAR:    ival = *(char*)field_ptr; break;
            case VAL_BYTE:    ival = *(unsigned char*)field_ptr; break;
            default:          ival = *(long long*)field_ptr; break;
        }
        /* 返回正确类型的 Value */
        Value v; memset(&v, 0, sizeof(v));
        v.type = fi->valtype;
        switch(fi->size) {
            case 1: v.v.i8 = (int8_t)ival; v.v.uc = (unsigned char)ival; v.v.by = (unsigned char)ival; break;
            case 2: v.v.i16 = (int16_t)ival; v.v.u16 = (uint16_t)ival; v.v.sh = (short)ival; v.v.us = (unsigned short)ival; break;
            case 4: v.v.i32 = (int32_t)ival; v.v.u32 = (uint32_t)ival; v.v.i = (int)ival; v.v.ui = (unsigned int)ival; break;
            default: v.v.i64 = ival; v.v.ll = ival; v.v.l = ival; break;
        }
        return v;
    }
    if(cls == 2) { /* 浮点族 */
        Value v; memset(&v, 0, sizeof(v));
        v.type = fi->valtype;
        if(fi->valtype == VAL_FLOAT) v.v.f = *(float*)field_ptr;
        else if(fi->valtype == VAL_LONG_DOUBLE) v.v.ld = *(long double*)field_ptr;
        else { v.type = VAL_DOUBLE; v.v.d = *(double*)field_ptr; }
        return v;
    }
    /* PTR 族：空指针（可空字段 T? 未指向对象）读作 VAL_NONE(null) */
    void* ptr = *(void**)field_ptr;
    if(!ptr) return val_none();
    Value v; memset(&v, 0, sizeof(v));
    if(fi->valtype == VAL_STRING) {
        return lumyr_make_string((const char*)ptr);
    }
    v.type = fi->valtype;
    /* 容器字段：装箱身份以 GC 头运行时 vtype 为准（声明 valtype 仅是静态提示）。
     * 根因修复：字段声明 <T>array（valtype=VAL_TYPED_ARRAY）经动态赋值实际持有
     * 普通数组（如 self.value = []）时，按声明打包会把 ValueArray* 误标为
     * VAL_TYPED_ARRAY，后续按 TypedArray* 解释 → 布局错位（len 对、items 为垃圾，
     * 元素读 null / sum=0 / SIGSEGV）。与 BOX_PTR 的根修同一原则。 */
    if(fi->valtype == VAL_ARRAY || fi->valtype == VAL_TYPED_ARRAY || fi->valtype == VAL_MAP) {
        int rt = gc_obj_vtype(ptr);
        if(rt == VAL_ARRAY || rt == VAL_TYPED_ARRAY || rt == VAL_MAP)
            v.type = (ValueType)rt;
    }
    v.v.struct_ptr = ptr;
    return v;
}

void lumyr_field_set(Value obj, const char* field_name, Value value)
{
    if((obj.type != VAL_STRUCT_PTR && obj.type != VAL_CLASS_PTR) || !obj.v.struct_ptr || !field_name) {
        runtime_error("field_set: 对象不是实例或字段名为空");
        return;
    }
    RuntimeTypeInfo* info = *(RuntimeTypeInfo**)obj.v.struct_ptr;
    if(!info) { runtime_error("field_set: 无法获取类型信息"); return; }

    FieldInfo* fi = lumyr_type_find_field(info, field_name);
    if(!fi) {
        char buf[256];
        snprintf(buf, sizeof(buf), "field_set: 类型 '%s' 没有字段 '%s'", info->name, field_name);
        runtime_error(buf);
        return;
    }

    /* 访问修饰符运行时检查（仅 class 动态路径） */
    if(info->kind == TYPE_KIND_CLASS && fi->access != ACCESS_PUBLIC) {
        if(!lumyr_is_accessor_inside_class(info->name) &&
           !lumyr_is_accessor_subclass_of(info->name)) {
            char buf[256];
            snprintf(buf, sizeof(buf), "field_set: 字段 '%s.%s' 不可访问", info->name, field_name);
            runtime_error(buf);
            return;
        }
    }

    char* field_ptr = (char*)obj.v.struct_ptr + fi->offset;
    int cls = lumyr_etype_stackcls(fi->valtype);

    if(cls == 1) { /* 整型族：按 size 精确写入 */
        long long ival = lumyr_extract_ll(value);
        switch(fi->size) {
            case 1: *(int8_t*)field_ptr = (int8_t)ival; break;
            case 2: *(int16_t*)field_ptr = (int16_t)ival; break;
            case 4: *(int32_t*)field_ptr = (int32_t)ival; break;
            default: *(long long*)field_ptr = ival; break;
        }
        return;
    }
    if(cls == 2) { /* 浮点族 */
        double dval = value_as_number(value);
        if(fi->valtype == VAL_FLOAT) *(float*)field_ptr = (float)dval;
        else if(fi->valtype == VAL_LONG_DOUBLE) *(long double*)field_ptr = (long double)dval;
        else *(double*)field_ptr = dval;
        return;
    }
    /* PTR 族 */
    if(fi->valtype == VAL_STRING) {
        /* 字符串字段：GC 分配拷贝（修复旧 strdup 内存泄漏） */
        const char* s = lumyr_str_cstr(&value);
        size_t len = strlen(s);
        char* gc_str = (char*)gc_alloc(len + 1, VAL_STRING);
        memcpy(gc_str, s, len + 1);
        *(char**)field_ptr = gc_str;
        return;
    }
    /* 其他指针类型：直接拷指针 */
    *(void**)field_ptr = value.v.struct_ptr;
}

/* ==================== 类型判断 ==================== */

int lumyr_type_implements(RuntimeTypeInfo* info, const char* interface_name)
{
    if(!info || !interface_name) return 0;
    /* 检查自身接口 */
    for(int i = 0; i < info->ninterfaces; i++) {
        if(info->interfaces[i] && strcmp(info->interfaces[i], interface_name) == 0)
            return 1;
    }
    /* 沿父链查找 */
    if(info->parent) return lumyr_type_implements(info->parent, interface_name);
    return 0;
}

int lumyr_type_is(Value obj, const char* type_name)
{
    if((obj.type != VAL_STRUCT_PTR && obj.type != VAL_CLASS_PTR) || !obj.v.struct_ptr || !type_name) return 0;
    RuntimeTypeInfo* info = *(RuntimeTypeInfo**)obj.v.struct_ptr;
    if(!info) return 0;

    /* 沿 parent 链比较类型名 */
    while(info) {
        if(info->name && strcmp(info->name, type_name) == 0) return 1;
        /* 同时检查接口 */
        if(lumyr_type_implements(info, type_name)) return 1;
        info = info->parent;
    }
    return 0;
}

/* ==================== 方法分派（CC 模式用） ==================== */

Value lumyr_type_call_method(Value obj, const char* method_name,
                             int argc, Value* args,
                             struct EvalCtx* ctx, struct StackFrame* frame)
{
    (void)ctx; (void)frame;
    if((obj.type != VAL_STRUCT_PTR && obj.type != VAL_CLASS_PTR) || !obj.v.struct_ptr || !method_name) {
        runtime_error("call_method: 对象不是实例或方法名为空");
        return val_none();
    }
    RuntimeTypeInfo* info = *(RuntimeTypeInfo**)obj.v.struct_ptr;
    if(!info) { runtime_error("call_method: 无法获取类型信息"); return val_none(); }

    /* 沿继承链查找方法 */
    while(info) {
        for(int i = 0; i < info->nmethods; i++) {
            if(info->method_names[i] && strcmp(info->method_names[i], method_name) == 0) {
                /* 找到方法，调用（CC 模式） */
                /* TODO: 实现方法调用，初版先返回 none */
                return val_none();
            }
        }
        info = info->parent;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "call_method: 方法 '%s' 未找到", method_name);
    runtime_error(buf);
    return val_none();
}

/* ==================== 访问修饰符检查 ==================== */

static const char* g_current_class = NULL;

void lumyr_set_current_class(const char* class_name) { g_current_class = class_name; }
const char* lumyr_get_current_class(void) { return g_current_class; }

int lumyr_is_accessor_inside_class(const char* class_name)
{
    return g_current_class && class_name && strcmp(g_current_class, class_name) == 0;
}

int lumyr_is_accessor_subclass_of(const char* class_name)
{
    if(!g_current_class || !class_name) return 0;
    /* 检查当前类是否是 class_name 的子类 */
    RuntimeTypeInfo* info = lumyr_type_lookup(g_current_class);
    while(info) {
        if(info->name && strcmp(info->name, class_name) == 0) return 1;
        info = info->parent;
    }
    return 0;
}
