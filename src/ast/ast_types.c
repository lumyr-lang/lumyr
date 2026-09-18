// ast_types.c —— type 声明类型表（编译期全局注册）
#include "ast_types.h"
#include "ast_runtime_sym.h"
#include "ir/ir_compile.h"
#include "ir/rbtree.h"
#include "ast_node.h"
#include "lm_class.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "func_compile.h"

/* 类型表用红黑树存储，键为类型名，class_name 为 NULL */
static RBTree* g_types_tree = NULL;

TypeDef* type_register(const char* name, char** props, ValueType* ptypes, int nprops, char** generic_params, int generic_param_count, char** interfaces, int ninterfaces)
{
    if(!g_types_tree) g_types_tree = rbtree_create();
    // 重名：覆盖（后声明优先，与变量赋值一致）
    TypeDef* td = (TypeDef*)rbtree_find(g_types_tree, NS_STRUCT, NULL, name);
    if(!td) {
        td = (TypeDef*)calloc(1, sizeof(TypeDef));
        td->name = strdup(name);
        rbtree_insert(g_types_tree, NS_STRUCT, NULL, name, td);
    }
    // 释放旧属性（重声明覆盖）
    if(td->props) {
        for(int k = 0; k < td->nprops; k++) free(td->props[k]);
        free(td->props);
        free(td->ptypes);
    }
    if(td->generic_params) {
        for(int k = 0; k < td->generic_param_count; k++) free(td->generic_params[k]);
        free(td->generic_params);
    }
    if(td->interfaces) {
        for(int k = 0; k < td->ninterfaces; k++) free(td->interfaces[k]);
        free(td->interfaces);
    }
    if(td->field_cast_kinds) free(td->field_cast_kinds);
    if(td->field_offsets) free(td->field_offsets);
    td->props = (char**)malloc((size_t)(nprops > 0 ? nprops : 1) * sizeof(char*));
    td->ptypes = (ValueType*)malloc((size_t)(nprops > 0 ? nprops : 1) * sizeof(ValueType));
    for(int k = 0; k < nprops; k++) {
        td->props[k] = strdup(props[k]);
        td->ptypes[k] = ptypes[k];
    }
    td->nprops = nprops;
    // 泛型参数
    if(generic_params && generic_param_count > 0) {
        td->generic_params = (char**)malloc((size_t)generic_param_count * sizeof(char*));
        for(int k = 0; k < generic_param_count; k++) {
            td->generic_params[k] = strdup(generic_params[k]);
        }
        td->generic_param_count = generic_param_count;
    } else {
        td->generic_params = NULL;
        td->generic_param_count = 0;
    }
    // 设置接口实现关系（不管 generic_params 是否存在都要设置）
    if(interfaces && ninterfaces > 0) {
        td->interfaces = (char**)malloc((size_t)ninterfaces * sizeof(char*));
        for(int k = 0; k < ninterfaces; k++) {
            td->interfaces[k] = strdup(interfaces[k]);
        }
        td->ninterfaces = ninterfaces;
    } else {
        td->interfaces = NULL;
        td->ninterfaces = 0;
    }
    return td;
}

TypeDef* type_lookup(const char* name)
{
    if(!g_types_tree || !name) return NULL;
    return (TypeDef*)rbtree_find(g_types_tree, NS_STRUCT, NULL, name);
}

/* type_get 已废弃，请使用 type_lookup 按名称查找 */
TypeDef* type_get(int idx)
{
    (void)idx;
    return NULL;
}

int type_count(void)
{
    return g_types_tree ? rbtree_count(g_types_tree) : 0;
}

/* type_foreach 的包装函数上下文 */
typedef struct {
    void (*cb)(const char*, TypeDef*, void*);
    void* ud;
} TypeForeachCtx;

/* type_foreach 的包装函数 */
static void type_foreach_wrapper(const char* class_name, const char* method_name, void* data, void* user_data)
{
    (void)class_name;
    TypeForeachCtx* ctx = (TypeForeachCtx*)user_data;
    ctx->cb(method_name, (TypeDef*)data, ctx->ud);
}

/* 遍历所有类型（红黑树中序遍历） */
void type_foreach(void (*callback)(const char* name, TypeDef* td, void* user_data), void* user_data)
{
    if(!g_types_tree) return;
    TypeForeachCtx ctx = { callback, user_data };
    rbtree_foreach(g_types_tree, type_foreach_wrapper, &ctx);
}

ValueType type_name_to_valtype(const char* tname)
{
    if(!tname) return VAL_NONE;
    if(strcmp(tname, "string") == 0)  return VAL_STRING;
    if(strcmp(tname, "int") == 0)     return VAL_INT;
    if(strcmp(tname, "double") == 0)  return VAL_DOUBLE;
    if(strcmp(tname, "bool") == 0)    return VAL_BOOL;
    if(strcmp(tname, "char") == 0)    return VAL_CHAR;
    if(strcmp(tname, "ascii") == 0)   return VAL_INT;  /* ASCII 码值按 int 处理 */
    if(strcmp(tname, "byte") == 0)    return VAL_BYTE;
    return VAL_NONE;
}

ValueType castkind_to_valtype(int ck)
{
    switch(ck) {
        case CAST_STRING: return VAL_STRING;
        case CAST_INT: case CAST_ASCII: return VAL_INT;
        case CAST_DOUBLE: return VAL_DOUBLE;
        case CAST_BOOL: return VAL_BOOL;
        case CAST_CHAR: return VAL_CHAR;
        case CAST_BYTE: return VAL_BYTE;
        case CAST_INT8: return VAL_INT8;
        case CAST_INT16: return VAL_INT16;
        case CAST_INT32: return VAL_INT32;
        case CAST_INT64: return VAL_INT64;
        case CAST_UINT8: return VAL_UINT8;
        case CAST_UINT16: return VAL_UINT16;
        case CAST_UINT32: return VAL_UINT32;
        case CAST_UINT64: return VAL_UINT64;
        case CAST_LONG: return VAL_LONG;
        case CAST_LONGLONG: return VAL_INT64;  // long long 等价于 int64
        case CAST_FLOAT: return VAL_FLOAT;
        case CAST_ULONG: return VAL_ULONG;
        case CAST_UCHAR: return VAL_UCHAR;
        case CAST_SHORT: return VAL_SHORT;  // short 类型，与 int16 彻底隔离
        case CAST_USHORT: return VAL_USHORT;  // unsigned short 类型，与 uint16 彻底隔离
        case CAST_SIZE_T: return VAL_SIZE_T;
        case CAST_SSIZE_T: return VAL_SSIZE_T;
        case CAST_LONG_DOUBLE: return VAL_LONG_DOUBLE;
        case CAST_PTR: return VAL_PTR;
        case CAST_VOID: return VAL_NONE;
        default: return VAL_NONE;
    }
}

/* CAST_xxx -> 类型名字符串（用于 FFI extern 函数返回类型存储） */
int valuetype_to_castkind(int vt) {
    switch(vt) {
        case VAL_INT: return CAST_INT;
        case VAL_DOUBLE: return CAST_DOUBLE;
        case VAL_BOOL: return CAST_BOOL;
        case VAL_CHAR: return CAST_CHAR;
        case VAL_STRING: return CAST_STRING;
        case VAL_BYTE: return CAST_BYTE;
        default: return CAST_LONGLONG;  /* 默认整数类型 */
    }
}

char* castkind_to_name(int ck) {
    switch(ck) {
        case CAST_STRING: return strdup("string");
        case CAST_INT: return strdup("int");
        case CAST_DOUBLE: return strdup("double");
        case CAST_BOOL: return strdup("bool");
        case CAST_CHAR: return strdup("char");
        case CAST_BYTE: return strdup("byte");
        case CAST_INT8: return strdup("int8");
        case CAST_INT16: return strdup("int16");
        case CAST_INT32: return strdup("int32");
        case CAST_INT64: return strdup("int64");
        case CAST_UINT8: return strdup("uint8");
        case CAST_UINT16: return strdup("uint16");
        case CAST_UINT32: return strdup("uint32");
        case CAST_UINT64: return strdup("uint64");
        case CAST_LONG: return strdup("long");
        case CAST_LONGLONG: return strdup("long long");
        case CAST_FLOAT: return strdup("float");
        case CAST_ASCII: return strdup("ascii");
        case CAST_ULONG: return strdup("ulong");
        case CAST_UCHAR: return strdup("uchar");
        case CAST_SHORT: return strdup("short");
        case CAST_USHORT: return strdup("ushort");
        case CAST_SIZE_T: return strdup("size_t");
        case CAST_SSIZE_T: return strdup("ssize_t");
        case CAST_VOID: return strdup("void");
        case CAST_LONG_DOUBLE: return strdup("long double");
        case CAST_PTR: return strdup("ptr");
        case CAST_BIGINT: return strdup("bigint");
        case CAST_DECIMAL: return strdup("decimal");
        default: return strdup("int");
    }
}

/* ValueType -> 类型名字符串（用于接口方法返回类型存储） */
char* valtype_to_name(ValueType vt) {
    switch(vt) {
        case VAL_INT: return strdup("int");
        case VAL_STRING: return strdup("string");
        case VAL_DOUBLE: return strdup("double");
        case VAL_BOOL: return strdup("bool");
        case VAL_CHAR: return strdup("char");
        case VAL_NONE: return strdup("void");
        default: return strdup("any");
    }
}


/* ===== 接口/trait 系统实现 ===== */

/* 接口表用红黑树存储，键为接口名，class_name 为 NULL */
static RBTree* g_interfaces_tree = NULL;

InterfaceDef* interface_register(const char* name, void* methods, const char* parent) {
    if(!g_interfaces_tree) g_interfaces_tree = rbtree_create();
    /* 检查是否已存在 */
    InterfaceDef* existing = (InterfaceDef*)rbtree_find(g_interfaces_tree, NS_CLASS, NULL, name);
    if(existing) {
        return existing; /* 已存在，返回原指针 */
    }

    /* 创建新的接口定义 */
    InterfaceDef* idef = (InterfaceDef*)calloc(1, sizeof(InterfaceDef));
    idef->name = strdup(name);
    idef->methods = NULL;
    idef->nmethods = 0;
    idef->parent = parent ? strdup(parent) : NULL;

    /* 先收集父接口的方法（如果有父接口且已注册） */
    int parent_methods_count = 0;
    InterfaceMethod* parent_methods = NULL;
    if(parent) {
        InterfaceDef* pdef = interface_lookup(parent);
        if(pdef && pdef->nmethods > 0) {
            parent_methods_count = pdef->nmethods;
            parent_methods = pdef->methods;
        }
    }

    /* 遍历当前接口的方法列表（AstNode* param 链表） */
    AstNode* m = (AstNode*)methods;
    int own_count = 0;
    AstNode* cur = m;
    while(cur) { own_count++; cur = cur->u.param.next; }

    int total_count = parent_methods_count + own_count;
    if(total_count > 0) {
        idef->methods = (InterfaceMethod*)malloc((size_t)total_count * sizeof(InterfaceMethod));
        int idx = 0;
        /* 先复制父接口的方法 */
        for(int i = 0; i < parent_methods_count; i++) {
            idef->methods[idx].name = strdup(parent_methods[i].name);
            idef->methods[idx].return_type = parent_methods[i].return_type ? strdup(parent_methods[i].return_type) : NULL;
            idx++;
        }
        /* 再复制当前接口的方法 */
        cur = m;
        for(int i = 0; i < own_count; i++) {
            idef->methods[idx].name = strdup(cur->u.param.name);
            idef->methods[idx].return_type = cur->u.param.constraint ? strdup(cur->u.param.constraint) : NULL;
            cur = cur->u.param.next;
            idx++;
        }
        idef->nmethods = total_count;
    }

    /* 插入到红黑树 */
    rbtree_insert(g_interfaces_tree, NS_CLASS, NULL, name, idef);
    return idef;
}

InterfaceDef* interface_lookup(const char* name) {
    if(!g_interfaces_tree || !name) return NULL;
    return (InterfaceDef*)rbtree_find(g_interfaces_tree, NS_CLASS, NULL, name);
}

/* interface_get 已废弃，请使用 interface_lookup 按名称查找 */
InterfaceDef* interface_get(int idx) {
    (void)idx;
    return NULL;
}

/* interface_foreach 的包装函数上下文 */
typedef struct {
    void (*cb)(const char*, InterfaceDef*, void*);
    void* ud;
} InterfaceForeachCtx;

/* interface_foreach 的包装函数 */
static void interface_foreach_wrapper(const char* class_name, const char* method_name, void* data, void* user_data)
{
    (void)class_name;
    InterfaceForeachCtx* ctx = (InterfaceForeachCtx*)user_data;
    ctx->cb(method_name, (InterfaceDef*)data, ctx->ud);
}

/* 遍历所有接口（红黑树中序遍历） */
void interface_foreach(void (*callback)(const char* name, InterfaceDef* idef, void* user_data), void* user_data)
{
    if(!g_interfaces_tree) return;
    InterfaceForeachCtx ctx = { callback, user_data };
    rbtree_foreach(g_interfaces_tree, interface_foreach_wrapper, &ctx);
}

/* ===== struct 注册 ===== */
TypeDef* struct_register(const char* name, char** props, CastKind* cast_kinds, char** struct_names, int nprops)
{
    // 先注册为普通 type（用 ValueType，从 CastKind 转换）
    ValueType* vtypes = (ValueType*)malloc((size_t)(nprops > 0 ? nprops : 1) * sizeof(ValueType));
    for(int k = 0; k < nprops; k++) {
        vtypes[k] = castkind_to_valtype(cast_kinds[k]);
    }
    TypeDef* td = type_register(name, props, vtypes, nprops, NULL, 0, NULL, 0);
    free(vtypes);

    // 标记为 struct 并保存精确 CastKind 类型
    td->is_struct = 1;
    td->field_cast_kinds = (CastKind*)malloc((size_t)(nprops > 0 ? nprops : 1) * sizeof(CastKind));
    td->field_struct_names = (char**)calloc((size_t)(nprops > 0 ? nprops : 1), sizeof(char*));
    for(int k = 0; k < nprops; k++) {
        td->field_cast_kinds[k] = cast_kinds[k];
        if(struct_names && struct_names[k]) {
            td->field_struct_names[k] = strdup(struct_names[k]);
        }
    }
    td->field_offsets = NULL; // 编译通道计算偏移时填充
    td->method_names = NULL;
    td->method_nodes = NULL;
    td->method_funcs = NULL;
    td->nmethods = 0;
    td->constructor = NULL;
    td->constructor_func = NULL;
    return td;
}

// 添加 struct 方法
void struct_add_method(const char* struct_name, const char* method_name, struct AstNode* method_node)
{
    TypeDef* td = struct_lookup(struct_name);
    if(!td) return;
    int n = td->nmethods + 1;
    td->method_names = (char**)realloc(td->method_names, (size_t)n * sizeof(char*));
    td->method_nodes = (struct AstNode**)realloc(td->method_nodes, (size_t)n * sizeof(struct AstNode*));
    td->method_names[td->nmethods] = strdup(method_name);
    td->method_nodes[td->nmethods] = method_node;
    td->nmethods = n;
}

// 查找 struct 方法
struct AstNode* struct_find_method(const char* struct_name, const char* method_name)
{
    TypeDef* td = struct_lookup(struct_name);
    if(!td) return NULL;
    for(int i = 0; i < td->nmethods; i++) {
        if(strcmp(td->method_names[i], method_name) == 0) {
            return td->method_nodes[i];
        }
    }
    return NULL;
}

// 查找是否是 struct（返回 TypeDef* 或 NULL）
TypeDef* struct_lookup(const char* name)
{
    TypeDef* td = type_lookup(name);
    if(!td) return NULL;
    if(!td->is_struct) return NULL;
    return td;
}

/* ===== class 注册 ===== */
TypeDef* class_register(const char* name, char** props, ValueType* ptypes, int* prop_access_modifiers, int nprops, const char* parent, char** interfaces)
{
    // 合并父类和子类的属性（父类属性在前，子类属性在后）
    char** merged_props = props;
    ValueType* merged_ptypes = ptypes;
    int* merged_access_modifiers = prop_access_modifiers;
    int merged_nprops = nprops;

    if(parent) {
        TypeDef* parent_td = type_lookup(parent);
        if(parent_td) {
            int parent_nprops = parent_td->nprops;
            if(parent_nprops > 0) {
                // 分配合并后的数组
                merged_nprops = parent_nprops + nprops;
                merged_props = (char**)malloc((size_t)merged_nprops * sizeof(char*));
                merged_ptypes = (ValueType*)malloc((size_t)merged_nprops * sizeof(ValueType));
                merged_access_modifiers = (int*)malloc((size_t)merged_nprops * sizeof(int));
                // 父类属性在前
                for(int i = 0; i < parent_nprops; i++) {
                    merged_props[i] = strdup(parent_td->props[i]);
                    merged_ptypes[i] = parent_td->ptypes[i];
                    merged_access_modifiers[i] = parent_td->prop_access_modifiers ? parent_td->prop_access_modifiers[i] : 0;
                }
                // 子类属性在后
                for(int i = 0; i < nprops; i++) {
                    merged_props[parent_nprops + i] = strdup(props[i]);
                    merged_ptypes[parent_nprops + i] = ptypes[i];
                    merged_access_modifiers[parent_nprops + i] = prop_access_modifiers ? prop_access_modifiers[i] : 0;
                }
            }
        }
    }

    // 先注册为普通 type（使用合并后的属性）
    int nifaces = 0;
    if(interfaces) {
        while(interfaces[nifaces]) nifaces++;
    }
    TypeDef* td = type_register(name, merged_props, merged_ptypes, merged_nprops, NULL, 0, interfaces, nifaces);

    // 标记为 class 并保存父类
    td->is_class = 1;
    td->parent = parent ? strdup(parent) : NULL;
    /* 设置 field_cast_kinds：根据 ValueType 转换成 CastKind，用于 C 代码生成时生成精确的字段类型 */
    td->field_cast_kinds = (CastKind*)malloc((size_t)(merged_nprops > 0 ? merged_nprops : 1) * sizeof(CastKind));
    td->field_struct_names = (char**)calloc((size_t)(merged_nprops > 0 ? merged_nprops : 1), sizeof(char*));
    for(int k = 0; k < merged_nprops; k++) {
        td->field_cast_kinds[k] = valuetype_to_castkind(merged_ptypes[k]);
    }
    td->field_offsets = NULL; // 编译通道计算偏移时填充
    td->method_names = NULL;
    td->method_nodes = NULL;
    td->method_funcs = NULL;
    td->nmethods = 0;
    td->constructor = NULL;
    td->constructor_func = NULL;
    // 设置属性访问修饰符（默认 public=0）
    if(merged_access_modifiers) {
        td->prop_access_modifiers = (int*)malloc((size_t)(merged_nprops > 0 ? merged_nprops : 1) * sizeof(int));
        for(int k = 0; k < merged_nprops; k++) {
            td->prop_access_modifiers[k] = merged_access_modifiers[k];
        }
    } else {
        td->prop_access_modifiers = NULL;
    }

    /* 创建并注册 ClassVTable（VM 模式下使用结构体+虚表） */
    {
        ClassVTable* vt = (ClassVTable*)malloc(sizeof(ClassVTable));
        memset(vt, 0, sizeof(ClassVTable));
        vt->class_name = strdup(name);
        vt->nfields = merged_nprops;
        if(merged_nprops > 0) {
            vt->field_names = (const char**)malloc((size_t)merged_nprops * sizeof(const char*));
            vt->field_offsets = (int*)malloc((size_t)merged_nprops * sizeof(int));
            vt->field_types = (ClassFieldType*)malloc((size_t)merged_nprops * sizeof(ClassFieldType));
            int offset = 0;
            for(int i = 0; i < merged_nprops; i++) {
                vt->field_names[i] = strdup(merged_props[i]);
                vt->field_offsets[i] = offset;
                /* 根据 ValueType 转换为 ClassFieldType，并计算字段大小 */
                switch(merged_ptypes[i]) {
                    case VAL_INT:
                        vt->field_types[i] = CLASS_FIELD_INT;
                        offset += sizeof(long long);
                        break;
                    case VAL_DOUBLE:
                        vt->field_types[i] = CLASS_FIELD_DOUBLE;
                        offset += sizeof(double);
                        break;
                    case VAL_BOOL:
                        vt->field_types[i] = CLASS_FIELD_BOOL;
                        offset += sizeof(int);
                        break;
                    case VAL_STRING:
                        vt->field_types[i] = CLASS_FIELD_STRING;
                        offset += sizeof(char*);
                        break;
                    default:
                        vt->field_types[i] = CLASS_FIELD_PTR;
                        offset += sizeof(void*);
                        break;
                }
            }
            vt->instance_size = offset;
        } else {
            vt->field_names = NULL;
            vt->field_offsets = NULL;
            vt->field_types = NULL;
            vt->instance_size = 0;
        }
        /* 方法表暂时为空，后续在编译阶段填充 */
        vt->nmethods = 0;
        vt->methods = NULL;
        vt->method_names = NULL;
        vt->parent = NULL;
        /* 填充接口信息 */
        vt->ninterfaces = td->ninterfaces;
        if(td->ninterfaces > 0 && td->interfaces) {
            vt->interfaces = (const char**)malloc((size_t)td->ninterfaces * sizeof(const char*));
            for(int ii = 0; ii < td->ninterfaces; ii++) {
                vt->interfaces[ii] = strdup(td->interfaces[ii]);
            }
        } else {
            vt->interfaces = NULL;
        }
        /* 注册到运行时库的红黑树中 */
        lumyr_class_vtable_register(vt);
    }

    return td;
}

// 查找是否是 class（返回 TypeDef* 或 NULL）
TypeDef* class_lookup(const char* name)
{
    TypeDef* td = type_lookup(name);
    if(!td) return NULL;
    if(!td->is_class) return NULL;
    return td;
}

// 添加 class 方法（同时编译为 RuntimeFunc 存储）
void class_add_method(const char* class_name, const char* method_name, struct AstNode* method_node)
{
    TypeDef* td = class_lookup(class_name);
    if(!td) return;
    /* 跳过构造函数：构造函数已经通过 class_set_constructor 单独设置，
       不需要再作为普通方法添加，否则会导致 class_name 重复设置和函数名冲突 */
    if(method_node && method_node->type == AST_FUNC_DEF && method_node->u.func_def.name) {
        size_t name_len = strlen(method_node->u.func_def.name);
        if(name_len >= 9 && strcmp(method_node->u.func_def.name + name_len - 9, "___init__") == 0) {
            return;
        }
    }
    // 给 self 参数设置 constraint（class:<类名>），让编译时自动给 self 打上 class 类型标记
    if(method_node && method_node->type == AST_FUNC_DEF && method_node->u.func_def.params) {
        AstNode* self_param = method_node->u.func_def.params;
        if(self_param->u.param.name && strcmp(self_param->u.param.name, "self") == 0) {
            if(self_param->u.param.constraint) free(self_param->u.param.constraint);
            size_t marked_len = strlen(class_name) + 7; /* "class:" + 类名 + \0 */
            self_param->u.param.constraint = (char*)malloc(marked_len);
            snprintf(self_param->u.param.constraint, marked_len, "class:%s", class_name);
        }
    }
    // 编译方法为 RuntimeFunc（传递 class_name，让函数表注册时正确设置 class_name）
    RuntimeFunc* rf = compile_func_from_ast_with_class(method_node, class_name);
    // 把方法注册到全局符号表中，用 (class_name, method_name) 作为键（红黑树，支持扩展）
    if(rf && method_node && method_node->type == AST_FUNC_DEF && method_node->u.func_def.name) {
        Value method_val;
        method_val.type = VAL_FUNC;
        method_val.v.func.func_obj = rf;
        method_val.v.func.ffi_func = NULL;
        method_val.v.func.is_ffi = 0;
        sym_set_class(class_name, method_node->u.func_def.name, method_val);
    }
    // class_name 字段已经在编译时通过 compile_func_from_ast_with_class 设置好了
    if(rf && rf->capture_count == -1) {
        InterpFuncPayload* pl = (InterpFuncPayload*)rf->captures;
        if(pl && pl->bytecode) {
            /* 给 self 参数打上 class 类型标记，让 self.x 访问生成 OPC_LOAD_FIELD（C 结构体直接偏移访问） */
            BytecodeFunc* bfn = pl->bytecode;
            if(bfn->is_method && bfn->param_cnt > 0) {
                /* self 是第一个参数，查找它在符号表中的索引 */
                int self_idx = -1;
                for(int i = 0; i < bfn->sym_cnt; i++) {
                    if(bfn->syms[i] && strcmp(bfn->syms[i], "self") == 0) {
                        self_idx = i;
                        break;
                    }
                }
                if(self_idx >= 0) {
                    /* 确保 var_struct_names 被分配 */
                    if(!bfn->var_struct_names) {
                        bfn->var_struct_names = (char**)calloc(bfn->sym_cnt > 16 ? bfn->sym_cnt : 16, sizeof(char*));
                    }
                    /* 用 class: 前缀标记这是 class 类型 */
                    size_t marked_len = strlen(class_name) + 7; /* "class:" + 类名 + \0 */
                    char* marked_name = (char*)malloc(marked_len);
                    snprintf(marked_name, marked_len, "class:%s", class_name);
                    if(bfn->var_struct_names[self_idx]) free(bfn->var_struct_names[self_idx]);
                    bfn->var_struct_names[self_idx] = marked_name;
                    if(bfn->method_self_struct) free(bfn->method_self_struct);
                    bfn->method_self_struct = strdup(marked_name);
                }
            }
        }
    }
    // 检查是否已有同名方法（方法重写）
    for(int i = 0; i < td->nmethods; i++) {
        if(strcmp(td->method_names[i], method_name) == 0) {
            // 方法重写：替换旧方法
            td->method_nodes[i] = method_node;
            td->method_funcs[i] = rf;
            // 同时更新 vtable 中的方法
            ClassVTable* vt = lumyr_class_vtable_lookup(class_name);
            if(vt) {
                for(int vi = 0; vi < vt->nmethods; vi++) {
                    if(strcmp(vt->method_names[vi], method_name) == 0) {
                        vt->methods[vi] = rf;
                        break;
                    }
                }
            }
            return;
        }
    }
    // 新方法：添加到方法表
    int n = td->nmethods + 1;
    td->method_names = (char**)realloc(td->method_names, (size_t)n * sizeof(char*));
    td->method_nodes = (struct AstNode**)realloc(td->method_nodes, (size_t)n * sizeof(struct AstNode*));
    td->method_funcs = (void**)realloc(td->method_funcs, (size_t)n * sizeof(void*));
    td->method_names[td->nmethods] = strdup(method_name);
    td->method_nodes[td->nmethods] = method_node;
    td->method_funcs[td->nmethods] = rf;
    td->nmethods = n;
    // 同时添加到 vtable 中
    ClassVTable* vt = lumyr_class_vtable_lookup(class_name);
    if(vt) {
        int vn = vt->nmethods + 1;
        vt->method_names = (const char**)realloc(vt->method_names, (size_t)vn * sizeof(const char*));
        vt->methods = (RuntimeFunc**)realloc(vt->methods, (size_t)vn * sizeof(RuntimeFunc*));
        vt->method_names[vt->nmethods] = strdup(method_name);
        vt->methods[vt->nmethods] = rf;
        vt->nmethods = vn;
    }
}

// 查找 class 方法的 RuntimeFunc（支持继承链查找）
void* class_find_method_func(const char* class_name, const char* method_name)
{
    TypeDef* td = class_lookup(class_name);
    if(!td) return NULL;
    // 先查当前类
    for(int i = 0; i < td->nmethods; i++) {
        if(strcmp(td->method_names[i], method_name) == 0) {
            return td->method_funcs[i];
        }
    }
    // 再查父类（递归）
    if(td->parent) {
        return class_find_method_func(td->parent, method_name);
    }
    return NULL;
}

// 设置 class 构造函数（__init__ 方法）
void class_set_constructor(const char* class_name, struct AstNode* constructor_node, void* constructor_func)
{
    TypeDef* td = class_lookup(class_name);
    if(!td) return;
    td->constructor = constructor_node;
    td->constructor_func = constructor_func;
}

// 获取 class 构造函数的 RuntimeFunc（支持继承链查找）
void* class_get_constructor_func(const char* class_name)
{
    TypeDef* td = class_lookup(class_name);
    if(!td) return NULL;
    // 先查当前类
    if(td->constructor_func) return td->constructor_func;
    // 再查父类（递归）
    if(td->parent) {
        return class_get_constructor_func(td->parent);
    }
    return NULL;
}

// 查找 class 方法（返回 AST 节点或 NULL，包含继承的方法）
struct AstNode* class_find_method(const char* class_name, const char* method_name)
{
    TypeDef* td = class_lookup(class_name);
    if(!td) return NULL;
    // 先在当前类中查找
    for(int i = 0; i < td->nmethods; i++) {
        if(strcmp(td->method_names[i], method_name) == 0) {
            return td->method_nodes[i];
        }
    }
    // 如果有父类，递归查找父类的方法
    if(td->parent) {
        return class_find_method(td->parent, method_name);
    }
    return NULL;
}

int type_implements_interface(const char* type_name, const char* interface_name) {
    /* 鸭子类型检查：类型是否有接口要求的所有方法 */
    InterfaceDef* idef = interface_lookup(interface_name);
    if(!idef) return 0; /* 接口不存在 */

    /* 查找类型定义 */
    TypeDef* tdef = type_lookup(type_name);
    if(!tdef) return 0; /* 类型不存在 */

    /* 检查类型是否有接口要求的所有方法（属性） */
    for(int i = 0; i < idef->nmethods; i++) {
        int found = 0;
        for(int j = 0; j < tdef->nprops; j++) {
            if(strcmp(tdef->props[j], idef->methods[i].name) == 0) {
                found = 1;
                break;
            }
        }
        if(!found) return 0; /* 缺少方法 */
    }

    return 1; /* 实现了所有方法 */
}

/* 检查 class 是否实现了接口中定义的所有方法（包括继承的方法）
   返回 1=实现了所有方法，0=缺少方法，-1=接口不存在或class不存在 */
int class_check_interface_implementation(const char* class_name, const char* interface_name) {
    InterfaceDef* idef = interface_lookup(interface_name);
    if(!idef) return -1; /* 接口不存在 */

    TypeDef* td = class_lookup(class_name);
    if(!td) return -1; /* class不存在 */

    /* 检查 class 是否有接口要求的所有方法（包括继承的方法） */
    for(int i = 0; i < idef->nmethods; i++) {
        struct AstNode* method = class_find_method(class_name, idef->methods[i].name);
        if(!method) {
            /* 缺少方法，打印警告 */
            fprintf(stderr, "警告：class \"%s\" 未实现接口 \"%s\" 要求的方法 \"%s\"\n",
                    class_name, interface_name, idef->methods[i].name);
            return 0;
        }
    }

    return 1; /* 实现了所有方法 */
}
