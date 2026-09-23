#ifndef AST_TYPES_H
#define AST_TYPES_H

#include "lumyr_value_type.h"

// type 声明（提前声明对象属性）与枚举声明
// type Person { name: string, age: int }   → 形状表（属性名 + 期望类型）
// enum Color { RED, GREEN }                → 全局 map {RED:"RED", GREEN:"GREEN"}
//
// 类型表为编译期全局注册表：yacc 声明时注册，typecheck 校验构造调用，
// ir_compile 把 Person(...) 构造展开为 map 字面量（属性按序 + 期望类型强转）

typedef struct {
    char* name;          // 类型名
    char** props;        // 属性名（按声明序）
    ValueType* ptypes;   // 属性期望类型（VAL_INT/VAL_STRING/...，VAL_NONE=未标注）
    int* prop_access_modifiers; // 属性访问修饰符（0=public, 1=private, 2=protected，NULL=默认public）
    int* prop_const_flags;      // const 字段标记（1=构造后不可修改，NULL=默认可变）
    int nprops;
    char** generic_params;  // 泛型参数名（NULL=非泛型类型）
    int generic_param_count; // 泛型参数数量
    char** interfaces;   // 实现的接口名列表（NULL=未实现接口）
    int ninterfaces;     // 实现的接口数量
    int is_struct;       // 是否是 struct（1=struct，0=普通 type/动态 Map）
    int is_class;        // 是否是 class（1=class，0=非 class）
    int is_abstract;     // 是否是抽象类（1=抽象类，0=普通类）
    char* parent;        // 父类名（NULL=无父类，仅 class 使用）
    CastKind* field_cast_kinds; // struct 字段的精确 CastKind 类型（NULL=非 struct，CAST_NONE=嵌套struct）
    char** field_struct_names; // struct 字段的嵌套 struct 类型名（NULL=非嵌套struct字段）
    int* field_offsets;    // struct 字段偏移量（编译通道用，NULL=未计算）
    /* struct/class 方法 */
    char** method_names;   // 方法名列表（NULL=无方法）
    struct AstNode** method_nodes; // 方法的 AST 节点（func_def）
    void** method_funcs;   // 方法 RuntimeFunc* 数组（编译后存储，避免重复编译）
    int nmethods;          // 方法数量
    /* class 构造函数（__init__ 方法，NULL=使用默认构造函数） */
    struct AstNode* constructor;  // 主构造函数 AST 节点（首个声明的重载）
    void* constructor_func;       // 主构造函数 RuntimeFunc*
    int nctor_overloads;          // 构造函数重载总数（0=无构造函数）
    /* 统一运行时类型信息指针（struct/class 注册时由 lumyr_type_register 返回） */
    struct RuntimeTypeInfo* runtime_info;
} TypeDef;

/* class 注册（属性用 ValueType 类型；struct_names 为字段自定义类型名，与 props 平行，可为全 NULL） */
TypeDef* class_register(const char* name, char** props, ValueType* ptypes, int* prop_access_modifiers, int* prop_const_flags, char** struct_names, int nprops, const char* parent, char** interfaces);
/* 查找是否是 class（返回 TypeDef* 或 NULL） */
TypeDef* class_lookup(const char* name);
/* 添加 class 方法 */
void class_add_method(const char* class_name, const char* method_name, struct AstNode* method_node);
/* 查找 class 方法（返回 AST 节点或 NULL，包含继承的方法） */
struct AstNode* class_find_method(const char* class_name, const char* method_name);
/* 同上，且输出方法的定义类名（def_owner 非空时写入）。访问控制用。 */
struct AstNode* class_find_method_owner(const char* class_name, const char* method_name, const char** def_owner);
/* 查找 class 方法的 RuntimeFunc（支持继承链查找） */
void* class_find_method_func(const char* class_name, const char* method_name);
/* 按类型名（struct/class 统一）查方法 AST，沿继承链回溯（方法调用签名） */
struct AstNode* type_find_method_ast(const char* type_name, const char* method_name);
/* 设置 class 构造函数（__init__ 方法） */
void class_set_constructor(const char* class_name, struct AstNode* constructor_node, void* constructor_func);
/* 添加 class 构造函数重载（首个成为主构造，后续重载按 <Cls>___init__N 命名） */
void class_add_constructor(const char* class_name, struct AstNode* constructor_node, void* constructor_func);
/* 获取 class 构造函数的 RuntimeFunc（支持继承链查找） */
void* class_get_constructor_func(const char* class_name);

/* 外部声明：yacc.y 中定义，供 class_add_method 前置设置构造函数 */
extern struct AstNode* g_class_constructor;

/* 前向声明 AstNode */
struct AstNode;

// 注册 / 查找（返回 TypeDef*，NULL 未找到）
TypeDef* type_register(const char* name, char** props, ValueType* ptypes, int nprops, char** generic_params, int generic_param_count, char** interfaces, int ninterfaces);
TypeDef* type_lookup(const char* name);
/* type_get 已废弃，请使用 type_lookup 按名称查找 */
TypeDef* type_get(int idx);
int type_count(void);
/* 遍历所有类型（红黑树中序遍历） */
void type_foreach(void (*callback)(const char* name, TypeDef* td, void* user_data), void* user_data);

// 属性类型名（string/int/double/bool/char/ascii/byte）→ ValueType；未知返回 VAL_NONE
ValueType type_name_to_valtype(const char* tname);
char* valtype_to_name(ValueType vt);
ValueType castkind_to_valtype(int ck);
int valuetype_to_castkind(int vt);
char* castkind_to_name(int ck);

/* ===== 接口/trait 系统 ===== */
// interface Printable { func to_string(): string }
// 接口表为编译期全局注册表：yacc 声明时注册，typecheck 校验类型是否实现接口

typedef struct {
    char* name;           // 方法名
    char* return_type;    // 返回类型名（NULL=无返回值/void）
} InterfaceMethod;

typedef struct {
    char* name;              // 接口名
    InterfaceMethod* methods; // 方法签名列表（包含继承的方法）
    int nmethods;
    char* parent;            // 父接口名（NULL=无父接口）
} InterfaceDef;

// 注册 / 查找（返回 InterfaceDef*，NULL 未找到）
InterfaceDef* interface_register(const char* name, void* methods, const char* parent);
InterfaceDef* interface_lookup(const char* name);
/* interface_get 已废弃，请使用 interface_lookup 按名称查找 */
InterfaceDef* interface_get(int idx);
/* 遍历所有接口（红黑树中序遍历） */
void interface_foreach(void (*callback)(const char* name, InterfaceDef* idef, void* user_data), void* user_data);

// struct 注册（字段用精确 CastKind 类型）
TypeDef* struct_register(const char* name, char** props, CastKind* cast_kinds, char** struct_names, int nprops);
// 查找是否是 struct（返回 TypeDef* 或 NULL）
TypeDef* struct_lookup(const char* name);
// 添加 struct 方法
void struct_add_method(const char* struct_name, const char* method_name, struct AstNode* method_node);
// 查找 struct 方法（返回 AST 节点或 NULL）
struct AstNode* struct_find_method(const char* struct_name, const char* method_name);

// 检查类型是否实现了接口（鸭子类型：检查类型是否有接口要求的所有方法）
int type_implements_interface(const char* type_name, const char* interface_name);

// 检查 class 是否实现了接口中定义的所有方法（包括继承的方法）
// 返回 1=实现了所有方法，0=缺少方法，-1=接口不存在或class不存在
int class_check_interface_implementation(const char* class_name, const char* interface_name);

/* ===== 类静态成员访问表（编译期访问控制） =====
 * static 属性/方法以全局名 "类名_成员名" 存在，此表记录其属主类与访问级别，
 * 供 typecheck 在引用这些全局名时做 private/protected 检查。 */
void class_static_member_register(const char* full_name, const char* owner, int access);
/* 查找静态成员：找到返回 1 并通过 owner_out/access_out 输出；未找到返回 0 */
int class_static_member_lookup(const char* full_name, const char** owner_out, int* access_out);

// 判断名称是否为 socket 构造函数名（TcpSocket/UdpSocket/UnixSocket/UnixDgramSocket，
// 含小写别名 tcpSocket 等）。供 lexer/parser 把 Name{...} 简写反糖为 Name({map}) 调用。
int is_socket_ctor_name(const char* name);

#endif //AST_TYPES_H
