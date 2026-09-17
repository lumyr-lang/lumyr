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
    int struct_size;  /* 结构体的字节大小，用于 memcpy 拷贝 */
} StructInfo;

/* 确保红黑树已初始化 */
static void ensure_struct_tree(void)
{
    if(!g_struct_tree) g_struct_tree = struct_rb_create();
}

/* ==================== struct 信息管理（使用红黑树存储） ==================== */

/* 注册 struct 信息（在代码生成时调用，把 struct 的字段信息导出到运行时） */
void lumyr_struct_register(const char* struct_name, int nfields, StructFieldInfo* fields, int struct_size)
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
            si->struct_size = struct_size;
        }
        return;
    }
    /* 创建新的 StructInfo */
    StructInfo* si = (StructInfo*)malloc(sizeof(StructInfo));
    if(!si) return;
    si->struct_name = strdup(struct_name);
    si->nfields = nfields;
    si->fields = fields;
    si->struct_size = struct_size;
    /* 插入红黑树 */
    struct_rb_insert(g_struct_tree, struct_name, si);
}

/* 获取 struct 信息（返回字段列表和数量，用于相等比较等） */
int lumyr_struct_get_info(const char* struct_name, StructFieldInfo** out_fields, int* out_nfields)
{
    if(!struct_name || !out_fields || !out_nfields) return 0;
    ensure_struct_tree();
    StructRBNode* node = struct_rb_find_node(g_struct_tree, struct_name);
    if(!node || !node->value) return 0;
    StructInfo* si = (StructInfo*)node->value;
    *out_fields = si->fields;
    *out_nfields = si->nfields;
    return 1;
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
            /* 根据字段宽度精确读取，避免越界读取相邻字段 */
            if(fi->size == 1) return lumyr_make_int((long long)*(int8_t*)field_ptr);
            if(fi->size == 2) return lumyr_make_int((long long)*(int16_t*)field_ptr);
            if(fi->size == 4) return lumyr_make_int((long long)*(int32_t*)field_ptr);
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
        case STRUCT_FIELD_INT: {
            /* 根据字段宽度一对一写入，类型匹配时零转换开销 */
            long long __ival;
            if(fi->size == 1) {
                /* 8位有符号字段：匹配 INT8/SHORT/INT16 等8位类型时直接读 */
                if(value.type == VAL_INT8)      __ival = value.v.i8;
                else if(value.type == VAL_BYTE) __ival = value.v.by;
                else if(value.type == VAL_UCHAR) __ival = value.v.uc;
                else __ival = lumyr_extract_ll(value);
                *(int8_t*)field_ptr = (int8_t)__ival;
            } else if(fi->size == 2) {
                /* 16位有符号字段：匹配 INT16/SHORT 时直接读 */
                if(value.type == VAL_INT16)     __ival = value.v.i16;
                else if(value.type == VAL_SHORT) __ival = value.v.sh;
                else if(value.type == VAL_UINT16) __ival = value.v.u16;
                else if(value.type == VAL_USHORT) __ival = value.v.us;
                else __ival = lumyr_extract_ll(value);
                *(int16_t*)field_ptr = (int16_t)__ival;
            } else if(fi->size == 4) {
                /* 32位有符号字段：匹配 INT/INT32/UINT32 时直接读 */
                if(value.type == VAL_INT)        __ival = value.v.i;
                else if(value.type == VAL_INT32) __ival = value.v.i32;
                else if(value.type == VAL_UINT32) __ival = value.v.u32;
                else if(value.type == VAL_UINT)  __ival = value.v.ui;
                else __ival = lumyr_extract_ll(value);
                *(int32_t*)field_ptr = (int32_t)__ival;
            } else {
                /* 64位有符号字段：匹配 INT64/LONG_LONG/LONG 时直接读 */
                if(value.type == VAL_INT64)      __ival = value.v.i64;
                else if(value.type == VAL_LONG_LONG) __ival = value.v.ll;
                else if(value.type == VAL_LONG)  __ival = value.v.l;
                else if(value.type == VAL_SIZE_T) __ival = value.v.st;
                else if(value.type == VAL_SSIZE_T) __ival = value.v.sst;
                else __ival = lumyr_extract_ll(value);
                *(long long*)field_ptr = __ival;
            }
            break;
        }
        case STRUCT_FIELD_DOUBLE:
            /* 浮点类型匹配时直接读，整数类型用 value_as_number 保留小数 */
            if(value.type == VAL_DOUBLE)      *(double*)field_ptr = value.v.d;
            else if(value.type == VAL_FLOAT)  *(double*)field_ptr = (double)value.v.f;
            else if(value.type == VAL_LONG_DOUBLE) *(double*)field_ptr = (double)value.v.ld;
            else *(double*)field_ptr = value_as_number(value);
            break;
        case STRUCT_FIELD_STRING:
            /* 字符串字段需要深拷贝 */
            if(*(char**)field_ptr) free(*(char**)field_ptr);
            *(char**)field_ptr = strdup(lumyr_str_cstr(&value));
            break;
        case STRUCT_FIELD_BOOL:
            *(int*)field_ptr = value.type == VAL_BOOL ? value.v.b : (lumyr_extract_ll(value) ? 1 : 0);
            break;
        case STRUCT_FIELD_PTR:
            *(void**)field_ptr = value.v.struct_ptr;
            break;
        default:
            runtime_error("struct 属性写入：不支持的字段类型");
            break;
    }
}

/* struct 相等比较（专门针对 struct 的函数，按字段比较） */
int lumyr_struct_eq(Value a, Value b)
{
    if(a.type != VAL_STRUCT_PTR || b.type != VAL_STRUCT_PTR) return 0;
    if(!a.v.struct_ptr || !b.v.struct_ptr) return a.v.struct_ptr == b.v.struct_ptr;
    /* 同一个指针直接相等 */
    if(a.v.struct_ptr == b.v.struct_ptr) return 1;
    /* 比较 struct 名 */
    const char* name_a = lumyr_struct_get_name(a);
    const char* name_b = lumyr_struct_get_name(b);
    if(!name_a || !name_b || strcmp(name_a, name_b) != 0) return 0;
    /* 获取字段列表，逐个比较 */
    StructFieldInfo* fields = NULL;
    int nfields = 0;
    if(!lumyr_struct_get_info(name_a, &fields, &nfields)) return 0;
    for(int i = 0; i < nfields; i++) {
        Value va = lumyr_struct_get_field(a, fields[i].name);
        Value vb = lumyr_struct_get_field(b, fields[i].name);
        if(va.type != vb.type) return 0;
        /* 每个类型一对一比较，直接读对应字段，零转换开销 */
        switch(va.type) {
            case VAL_INT:          if(va.v.i != vb.v.i) return 0; break;
            case VAL_INT8:         if(va.v.i8 != vb.v.i8) return 0; break;
            case VAL_INT16:        if(va.v.i16 != vb.v.i16) return 0; break;
            case VAL_SHORT:        if(va.v.sh != vb.v.sh) return 0; break;
            case VAL_INT32:        if(va.v.i32 != vb.v.i32) return 0; break;
            case VAL_INT64:        if(va.v.i64 != vb.v.i64) return 0; break;
            case VAL_LONG_LONG:    if(va.v.ll != vb.v.ll) return 0; break;
            case VAL_LONG:         if(va.v.l != vb.v.l) return 0; break;
            case VAL_BYTE:         if(va.v.by != vb.v.by) return 0; break;
            case VAL_UINT8:        if(va.v.u8 != vb.v.u8) return 0; break;
            case VAL_UCHAR:        if(va.v.uc != vb.v.uc) return 0; break;
            case VAL_UINT16:       if(va.v.u16 != vb.v.u16) return 0; break;
            case VAL_USHORT:       if(va.v.us != vb.v.us) return 0; break;
            case VAL_UINT32:       if(va.v.u32 != vb.v.u32) return 0; break;
            case VAL_UINT:         if(va.v.ui != vb.v.ui) return 0; break;
            case VAL_UINT64:       if(va.v.u64 != vb.v.u64) return 0; break;
            case VAL_ULONG:        if(va.v.ul != vb.v.ul) return 0; break;
            case VAL_SIZE_T:       if(va.v.st != vb.v.st) return 0; break;
            case VAL_SSIZE_T:      if(va.v.sst != vb.v.sst) return 0; break;
            case VAL_FLOAT:        if(va.v.f != vb.v.f) return 0; break;
            case VAL_DOUBLE:       if(va.v.d != vb.v.d) return 0; break;
            case VAL_LONG_DOUBLE:  if(va.v.ld != vb.v.ld) return 0; break;
            case VAL_BOOL:         if(va.v.b != vb.v.b) return 0; break;
            case VAL_CHAR:         if(va.v.c != vb.v.c) return 0; break;
            case VAL_STRING: {
                const char* sa = lumyr_str_cstr(&va);
                const char* sb = lumyr_str_cstr(&vb);
                if(!sa || !sb || strcmp(sa, sb) != 0) return 0;
                break;
            }
            case VAL_STRUCT_PTR:
                if(!lumyr_struct_eq(va, vb)) return 0;
                break;
            case VAL_ARRAY:
            case VAL_MAP:
                /* 数组/map 比较：引用相等即相等（浅比较） */
                if(va.v.array != vb.v.array) return 0;
                break;
            default:
                /* 其他类型：用引用比较 */
                if(va.v.ll != vb.v.ll) return 0;
                break;
        }
    }
    return 1;
}

/* struct 浅拷贝（专门针对 struct 的函数，用于值传递） */
Value lumyr_struct_shallow_copy(Value obj)
{
    if(obj.type != VAL_STRUCT_PTR || !obj.v.struct_ptr) return obj;
    const char* struct_name = lumyr_struct_get_name(obj);
    if(!struct_name) return obj;
    ensure_struct_tree();
    StructRBNode* node = struct_rb_find_node(g_struct_tree, struct_name);
    if(!node || !node->value) return obj;
    StructInfo* si = (StructInfo*)node->value;
    if(si->struct_size <= 0) return obj;
    /* 分配新的结构体并拷贝 */
    void* new_ptr = malloc(si->struct_size);
    if(!new_ptr) return obj;
    memcpy(new_ptr, obj.v.struct_ptr, si->struct_size);
    Value result;
    result.type = VAL_STRUCT_PTR;
    result.v.struct_ptr = new_ptr;
    return result;
}
