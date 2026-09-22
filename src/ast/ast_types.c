// ast_types.c —— type 声明类型表（编译期全局注册）
#include "ast_types.h"
#include "ast_runtime_sym.h"
#include "ir/ir_compile.h"
#include "ir/rbtree.h"
#include "ast_node.h"
#include "lm_type.h"
#include "lumyr_value.h"
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

/* type_foreach 的包装函数。
 * rbtree_foreach 回调为 5 参数 (ns, class_name, name, data, user_data)；
 * 类型表 class_name 恒为 NULL、name 即类型名、data 为 TypeDef*。 */
static void type_foreach_wrapper(RBTNamespace ns, const char* class_name,
                                 const char* name, void* data, void* user_data)
{
    (void)ns;
    (void)class_name;
    TypeForeachCtx* ctx = (TypeForeachCtx*)user_data;
    ctx->cb(name, (TypeDef*)data, ctx->ud);
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
    /* 高精度类型：lexer 将其作为普通标识符返回，在类型位置（含泛型 <K,V>）需正确识别，
       此前落到 VAL_NONE 导致 <string,bigint> 的值被 cast 成 long long */
    if(strcmp(tname, "bigint") == 0)     return VAL_BIGINT;
    if(strcmp(tname, "decimal") == 0)    return VAL_DECIMAL;
    if(strcmp(tname, "bitdecimal") == 0) return VAL_BITDECIMAL;
    /* 容器类型：map/array 字段声明需正确识别，否则落 VAL_NONE 被 cast 成 long long */
    if(strcmp(tname, "map") == 0)        return VAL_MAP;
    if(strcmp(tname, "array") == 0)      return VAL_ARRAY;
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
        case CAST_UINT:   return VAL_UINT;
        case CAST_UINT64: return VAL_UINT64;
        case CAST_LONG: return VAL_LONG;
        case CAST_LONGLONG: return VAL_LONG_LONG;  // 与 int64 区分，独立类型名/元素存储
        case CAST_FLOAT: return VAL_FLOAT;
        case CAST_ULONG: return VAL_ULONG;
        case CAST_UCHAR: return VAL_UCHAR;
        case CAST_SHORT: return VAL_SHORT;  // short 类型，与 int16 彻底隔离
        case CAST_USHORT: return VAL_USHORT;  // unsigned short 类型，与 uint16 彻底隔离
        case CAST_SIZE_T: return VAL_SIZE_T;
        case CAST_SSIZE_T: return VAL_SSIZE_T;
        case CAST_LONG_DOUBLE: return VAL_LONG_DOUBLE;
        case CAST_PTR: return VAL_PTR;
        case CAST_BIGINT: return VAL_BIGINT;
        case CAST_DECIMAL: return VAL_DECIMAL;
        case CAST_BITDECIMAL: return VAL_BITDECIMAL;
        case CAST_DATE: return VAL_DATE;
        case CAST_DATETIME: return VAL_DATETIME;
        case CAST_TIME: return VAL_TIME;
        case CAST_TIMEDELTA: return VAL_TIMEDELTA;
        case CAST_TYPED_ARRAY: return VAL_TYPED_ARRAY;
        /* 容器引用：字段持堆指针（8 字节） */
        case CAST_MAP: return VAL_MAP;
        case CAST_ARRAY: return VAL_ARRAY;
        /* 自定义类型引用：必须映射到对应实例指针，否则字段 valtype=VAL_NONE，
         * 构造写入走错分支、方法分派取错类型信息 */
        case CAST_STRUCT_PTR: return VAL_STRUCT_PTR;
        case CAST_CLASS_PTR: return VAL_CLASS_PTR;
        case CAST_VOID: return VAL_NONE;
        default: return VAL_NONE;
    }
}

/* CAST_xxx -> 类型名字符串（用于 FFI extern 函数返回类型存储） */
int valuetype_to_castkind(int vt) {
    switch(vt) {
        case VAL_INT: return CAST_INT;
        case VAL_INT8: return CAST_INT8;
        case VAL_INT16: return CAST_INT16;
        case VAL_INT32: return CAST_INT32;
        case VAL_INT64: return CAST_INT64;
        case VAL_LONG: return CAST_LONG;
        case VAL_LONG_LONG: return CAST_LONGLONG;
        case VAL_SHORT: return CAST_SHORT;
        case VAL_UINT8: return CAST_UINT8;
        case VAL_UINT16: return CAST_UINT16;
        case VAL_UINT32: return CAST_UINT32;
        case VAL_UINT: return CAST_UINT;
        case VAL_UINT64: return CAST_UINT64;
        case VAL_ULONG: return CAST_ULONG;
        case VAL_UCHAR: return CAST_UCHAR;
        case VAL_USHORT: return CAST_USHORT;
        case VAL_SIZE_T: return CAST_SIZE_T;
        case VAL_SSIZE_T: return CAST_SSIZE_T;
        case VAL_BOOL: return CAST_BOOL;
        case VAL_CHAR: return CAST_CHAR;
        case VAL_BYTE: return CAST_BYTE;
        case VAL_DOUBLE: return CAST_DOUBLE;
        case VAL_FLOAT: return CAST_FLOAT;
        case VAL_LONG_DOUBLE: return CAST_LONG_DOUBLE;
        case VAL_STRING: return CAST_STRING;
        case VAL_PTR: return CAST_PTR;
        case VAL_BIGINT: return CAST_BIGINT;
        case VAL_DECIMAL: return CAST_DECIMAL;
        case VAL_BITDECIMAL: return CAST_BITDECIMAL;
        case VAL_DATE: return CAST_DATE;
        case VAL_DATETIME: return CAST_DATETIME;
        case VAL_TIME: return CAST_TIME;
        case VAL_TIMEDELTA: return CAST_TIMEDELTA;
        /* 容器/引用类型：cast 语义为透传（保持容器不被标量化） */
        case VAL_MAP: return CAST_MAP;
        case VAL_ARRAY: return CAST_ARRAY;
        case VAL_TYPED_ARRAY: return CAST_TYPED_ARRAY;
        case VAL_STRUCT_PTR: return CAST_STRUCT_PTR;
        case VAL_CLASS_PTR: return CAST_CLASS_PTR;
        default: return CAST_LONGLONG;
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
        case CAST_BITDECIMAL: return strdup("bitdecimal");
        case CAST_DATE: return strdup("date");
        case CAST_DATETIME: return strdup("datetime");
        case CAST_TIME: return strdup("time");
        case CAST_TIMEDELTA: return strdup("timedelta");
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
    td->field_offsets = NULL; // 下方 lumyr_type_register 后填充
    td->method_names = NULL;
    td->method_nodes = NULL;
    td->method_funcs = NULL;
    td->nmethods = 0;
    td->constructor = NULL;
    td->constructor_func = NULL;

    /* 创建统一 RuntimeTypeInfo 并注册到运行时 */
    {
        int nfields = nprops;
        FieldInfo* fields = NULL;
        int instance_size = 8;  /* 偏移 0: RuntimeTypeInfo* 指针 */

        if(nfields > 0) {
            fields = (FieldInfo*)calloc(nfields, sizeof(FieldInfo));
            for(int i = 0; i < nfields; i++) {
                ValueType vt = castkind_to_valtype(cast_kinds[i]);
                size_t sz = lumyr_etype_itemsz(vt);
                int align = (sz <= 2) ? (int)sz : ((sz <= 4) ? 4 : ((sz >= 16) ? 16 : 8));
                instance_size = (instance_size + align - 1) & ~(align - 1);
                fields[i].name = strdup(props[i]);
                fields[i].offset = instance_size;
                fields[i].valtype = vt;
                fields[i].size = (int)sz;
                fields[i].access = ACCESS_PUBLIC;
                fields[i].type_name = (struct_names && struct_names[i]) ? strdup(struct_names[i]) : NULL;
                fields[i].annotation_count = 0;
                fields[i].annotations = NULL;
                instance_size += (int)sz;
            }
            instance_size = (instance_size + 7) & ~7;
        }

        td->runtime_info = lumyr_type_register(
            name, nfields, fields, instance_size,
            TYPE_KIND_STRUCT, NULL, 0, NULL,
            NULL, 0, NULL, 0
        );
        /* 填充 field_offsets 供编译器使用 */
        td->field_offsets = (int*)calloc(nfields > 0 ? nfields : 1, sizeof(int));
        for(int i = 0; i < nfields; i++) {
            td->field_offsets[i] = fields[i].offset;
        }
    }

    return td;
}

// 添加 struct 方法
void struct_add_method(const char* struct_name, const char* method_name, struct AstNode* method_node)
{
    TypeDef* td = struct_lookup(struct_name);
    if(!td) return;

    /* 给 self 参数设置 constraint（struct 名），让编译期识别为 STRUCT_PTR */
    if(method_node && method_node->type == AST_FUNC_DEF && method_node->u.func_def.params) {
        AstNode* self_param = method_node->u.func_def.params;
        if(self_param->u.param.name && strcmp(self_param->u.param.name, "self") == 0) {
            if(self_param->u.param.constraint) free(self_param->u.param.constraint);
            self_param->u.param.constraint = strdup(struct_name);
        }
    }

    /* 编译方法（传 struct_name 作为属主：BytecodeFunc 用唯一内部名 <Struct>__m__<method>，
     * 避免多个 struct 的同名方法在函数表互相覆盖） */
    RuntimeFunc* rf = compile_func_from_ast_with_class(method_node, struct_name);

    /* 登记到 TypeDef 方法表 */
    int n = td->nmethods + 1;
    td->method_names = (char**)realloc(td->method_names, (size_t)n * sizeof(char*));
    td->method_nodes = (struct AstNode**)realloc(td->method_nodes, (size_t)n * sizeof(struct AstNode*));
    td->method_funcs = (void**)realloc(td->method_funcs, (size_t)n * sizeof(void*));
    td->method_names[td->nmethods] = strdup(method_name);
    td->method_nodes[td->nmethods] = method_node;
    td->method_funcs[td->nmethods] = rf;
    td->nmethods = n;

    /* 同步到 RuntimeTypeInfo 方法表（VM OPC_CALL_METHOD 分派依据） */
    if(td->runtime_info) lumyr_type_set_method(td->runtime_info, method_name, rf);

    /* 不再以扁平方法名全局注册：方法必须通过接收者类型分派（recv.method()） */
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
TypeDef* class_register(const char* name, char** props, ValueType* ptypes, int* prop_access_modifiers, int* prop_const_flags, char** struct_names, int nprops, const char* parent, char** interfaces)
{
    // 合并父类和子类的属性（父类属性在前，子类属性在后）
    char** merged_props = props;
    ValueType* merged_ptypes = ptypes;
    int* merged_access_modifiers = prop_access_modifiers;
    int* merged_const_flags = prop_const_flags;
    char** merged_struct_names = struct_names;
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
                merged_const_flags = (int*)malloc((size_t)merged_nprops * sizeof(int));
                merged_struct_names = (char**)calloc((size_t)merged_nprops, sizeof(char*));
                // 父类属性在前
                for(int i = 0; i < parent_nprops; i++) {
                    merged_props[i] = strdup(parent_td->props[i]);
                    merged_ptypes[i] = parent_td->ptypes[i];
                    merged_access_modifiers[i] = parent_td->prop_access_modifiers ? parent_td->prop_access_modifiers[i] : 0;
                    merged_const_flags[i] = parent_td->prop_const_flags ? parent_td->prop_const_flags[i] : 0;
                    if(parent_td->field_struct_names && parent_td->field_struct_names[i])
                        merged_struct_names[i] = strdup(parent_td->field_struct_names[i]);
                }
                // 子类属性在后
                for(int i = 0; i < nprops; i++) {
                    merged_props[parent_nprops + i] = strdup(props[i]);
                    merged_ptypes[parent_nprops + i] = ptypes[i];
                    merged_access_modifiers[parent_nprops + i] = prop_access_modifiers ? prop_access_modifiers[i] : 0;
                    merged_const_flags[parent_nprops + i] = prop_const_flags ? prop_const_flags[i] : 0;
                    if(struct_names && struct_names[i])
                        merged_struct_names[parent_nprops + i] = strdup(struct_names[i]);
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
        /* strdup：merged_struct_names 归解析期收集器所有（clear 时释放），td 需长期存活 */
        if(merged_struct_names && merged_struct_names[k])
            td->field_struct_names[k] = strdup(merged_struct_names[k]);
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

    /* 创建统一 RuntimeTypeInfo 并注册到运行时 */
    {
        int nfields = merged_nprops;
        FieldInfo* fields = NULL;
        int instance_size = 8;  /* 偏移 0: RuntimeTypeInfo* 指针 */

        if(nfields > 0) {
            fields = (FieldInfo*)calloc(nfields, sizeof(FieldInfo));
            for(int i = 0; i < nfields; i++) {
                ValueType vt = merged_ptypes[i];
                size_t sz = lumyr_etype_itemsz(vt);
                /* 自然对齐 */
                int align = (sz <= 2) ? (int)sz : ((sz <= 4) ? 4 : ((sz >= 16) ? 16 : 8));
                instance_size = (instance_size + align - 1) & ~(align - 1);
                fields[i].name = strdup(merged_props[i]);
                fields[i].offset = instance_size;
                fields[i].valtype = vt;
                fields[i].size = (int)sz;
                fields[i].access = merged_access_modifiers ?
                    (AccessModifier)merged_access_modifiers[i] : ACCESS_PUBLIC;
                fields[i].is_const = merged_const_flags ? merged_const_flags[i] : 0;
                fields[i].type_name = (merged_struct_names && merged_struct_names[i]) ?
                    strdup(merged_struct_names[i]) : NULL;
                fields[i].annotation_count = 0;
                fields[i].annotations = NULL;
                instance_size += (int)sz;
            }
            /* 末尾对齐到最大对齐（至少 8） */
            instance_size = (instance_size + 7) & ~7;
        }

        /* 查找父类 RuntimeTypeInfo（用于继承链） */
        RuntimeTypeInfo* parent_info = NULL;
        if(parent) {
            parent_info = lumyr_type_lookup(parent);
        }

        td->runtime_info = lumyr_type_register(
            name, nfields, fields, instance_size,
            TYPE_KIND_CLASS, NULL, 0, NULL,
            parent_info, td->ninterfaces, td->interfaces,
            (uint8_t)td->is_abstract
        );
        /* 填充 field_offsets 供编译器使用 */
        td->field_offsets = (int*)calloc(nfields > 0 ? nfields : 1, sizeof(int));
        for(int i = 0; i < nfields; i++) {
            td->field_offsets[i] = fields[i].offset;
        }
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
    /* 登记 TypeDef 方法表：同名覆盖（重写），新名追加 */
    int found = 0;
    for(int i = 0; i < td->nmethods; i++) {
        if(strcmp(td->method_names[i], method_name) == 0) {
            td->method_nodes[i] = method_node;
            td->method_funcs[i] = rf;
            found = 1;
            break;
        }
    }
    if(!found) {
        int n = td->nmethods + 1;
        td->method_names = (char**)realloc(td->method_names, (size_t)n * sizeof(char*));
        td->method_nodes = (struct AstNode**)realloc(td->method_nodes, (size_t)n * sizeof(struct AstNode*));
        td->method_funcs = (void**)realloc(td->method_funcs, (size_t)n * sizeof(void*));
        td->method_names[td->nmethods] = strdup(method_name);
        td->method_nodes[td->nmethods] = method_node;
        td->method_funcs[td->nmethods] = rf;
        td->nmethods = n;
    }

    /* 同步 RuntimeTypeInfo 方法表（含父类继承槽位，重写覆盖在原位置）→ 多态分派依据 */
    if(td->runtime_info) lumyr_type_set_method(td->runtime_info, method_name, rf);
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

/* class_find_method_owner：class_find_method 的扩展版，同时输出方法的
 * "定义类"名（沿继承链回溯时实际声明该方法的类）。private/protected
 * 访问控制必须以定义类为准，而非接收者的静态类型——否则子类方法内
 * self.父类private方法 会因 owner 等于接收者类型而被错误放行。 */
struct AstNode* class_find_method_owner(const char* class_name, const char* method_name, const char** def_owner)
{
    TypeDef* td = class_lookup(class_name);
    if(!td) return NULL;
    for(int i = 0; i < td->nmethods; i++) {
        if(strcmp(td->method_names[i], method_name) == 0) {
            if(def_owner) *def_owner = td->name;
            return td->method_nodes[i];
        }
    }
    if(td->parent) {
        return class_find_method_owner(td->parent, method_name, def_owner);
    }
    return NULL;
}

/* type_find_method_ast：按类型名（struct/class 统一 type_lookup）查方法 AST 节点，
 * 沿 parent 继承链回溯。用于 recv.method() 编译期获取方法形参签名。 */
struct AstNode* type_find_method_ast(const char* type_name, const char* method_name)
{
    TypeDef* td = type_lookup(type_name);
    if(!td) return NULL;
    for(int i = 0; i < td->nmethods; i++) {
        if(strcmp(td->method_names[i], method_name) == 0)
            return td->method_nodes[i];
    }
    if(td->parent) return type_find_method_ast(td->parent, method_name);
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

/* ===== 类静态成员访问表 ===== */
typedef struct {
    char* full_name;   /* 全局名 "类名_成员名" */
    char* owner;       /* 属主类名 */
    int access;        /* 0=public, 1=private, 2=protected */
} StaticMemberEntry;

static StaticMemberEntry* g_static_members = NULL;
static int g_static_member_n = 0;
static int g_static_member_cap = 0;

void class_static_member_register(const char* full_name, const char* owner, int access)
{
    /* 已注册同名成员则只更新访问级别 */
    for(int i = 0; i < g_static_member_n; i++) {
        if(strcmp(g_static_members[i].full_name, full_name) == 0) {
            g_static_members[i].access = access;
            return;
        }
    }
    if(g_static_member_n >= g_static_member_cap) {
        g_static_member_cap = g_static_member_cap == 0 ? 8 : g_static_member_cap * 2;
        g_static_members = (StaticMemberEntry*)realloc(g_static_members,
            (size_t)g_static_member_cap * sizeof(StaticMemberEntry));
    }
    g_static_members[g_static_member_n].full_name = strdup(full_name);
    g_static_members[g_static_member_n].owner = strdup(owner);
    g_static_members[g_static_member_n].access = access;
    g_static_member_n++;
}

int class_static_member_lookup(const char* full_name, const char** owner_out, int* access_out)
{
    for(int i = 0; i < g_static_member_n; i++) {
        if(strcmp(g_static_members[i].full_name, full_name) == 0) {
            if(owner_out) *owner_out = g_static_members[i].owner;
            if(access_out) *access_out = g_static_members[i].access;
            return 1;
        }
    }
    return 0;
}
