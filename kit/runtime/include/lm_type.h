#ifndef LUMYR_RUNTIME_LM_TYPE_H
#define LUMYR_RUNTIME_LM_TYPE_H

#include "lumyr_value_type.h"

/* 前置声明 */
struct EvalCtx;
struct StackFrame;

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 类型种类 ==================== */

typedef enum {
    TYPE_KIND_TYPE = 0,     /* type { field: T } 形状声明 */
    TYPE_KIND_STRUCT = 1,   /* struct { field: T } 值类型 */
    TYPE_KIND_CLASS = 2,    /* class { field: T } 引用类型 + 方法表 */
} TypeKind;

/* ==================== 访问修饰符 ==================== */

typedef enum {
    ACCESS_PUBLIC = 0,
    ACCESS_PRIVATE = 1,
    ACCESS_PROTECTED = 2,
} AccessModifier;

/* ==================== 字段信息 ==================== */

typedef struct {
    const char* name;
    int offset;                 /* 字段在实例中的偏移量（字节，从 8 开始） */
    ValueType valtype;          /* 28+ 精确类型（枚举） */
    int size;                   /* 字节宽度 */
    AccessModifier access;      /* struct 默认 PUBLIC，class 可设 PRIVATE/PROTECTED */
    int is_const;               /* const 字段：1=构造后不可修改 */
    /* 当 valtype 为 VAL_STRUCT_PTR/VAL_CLASS_PTR 时，指向字段的自定义类型名
     * （如 inner: Inner → "Inner"），供方法链调用编译期推断接收者类型；其余类型为 NULL */
    const char* type_name;
    /* 注解（后续扩展，初版 count=0、annotations=NULL） */
    int annotation_count;
    void** annotations;         /* AnnotationInfo* 指针数组（编译器侧管理） */
} FieldInfo;

/* ==================== 运行时类型信息 ==================== */

typedef struct RuntimeTypeInfo {
    const char* name;           /* 类型名 */
    int nfields;
    FieldInfo* fields;          /* 含父类字段（扁平化合并） */
    int instance_size;          /* 实例总大小（含 info 指针） */
    TypeKind kind;
    /* class 特有：方法表、父类、接口 */
    int nmethods;
    RuntimeFunc** methods;      /* class 方法（struct 为 NULL）；含父类方法（覆盖在原位置） */
    const char** method_names;
    struct RuntimeTypeInfo* parent; /* 父类（struct/type 为 NULL） */
    int ninterfaces;
    const char** interfaces;
    /* 注解与修饰 */
    uint8_t is_abstract;        /* @abstract 标记，不可实例化 */
} RuntimeTypeInfo;

/* ==================== 统一 API ==================== */

/* 注册类型信息，返回 RuntimeTypeInfo* 指针（编译期绑定，不查名） */
RuntimeTypeInfo* lumyr_type_register(
    const char* name,
    int nfields,
    FieldInfo* fields,
    int instance_size,
    TypeKind kind,
    RuntimeFunc** methods,
    int nmethods,
    const char** method_names,
    struct RuntimeTypeInfo* parent,
    int ninterfaces,
    const char** interfaces,
    uint8_t is_abstract
);

/* 按名查找类型信息（仅用于 type() 显示等反射场景，不用于实例创建） */
RuntimeTypeInfo* lumyr_type_lookup(const char* name);

/* 查找字段信息（用 RuntimeTypeInfo* 指针，不查名） */
FieldInfo* lumyr_type_find_field(RuntimeTypeInfo* info, const char* field_name);

/* 方法表操作：方法表在类型注册时已扁平化包含父类方法（重写覆盖在原位置）
 * find 返回方法实现 RuntimeFunc*（未找到 NULL）；set 同名覆盖、新名追加 */
RuntimeFunc* lumyr_type_find_method(RuntimeTypeInfo* info, const char* method_name);
void lumyr_type_set_method(RuntimeTypeInfo* info, const char* method_name, RuntimeFunc* rf);

/* 创建实例（用 RuntimeTypeInfo* 指针，不查注册表）
 * 分配 instance_size 字节，偏移 0 设 info 指针
 * 抽象类（is_abstract=1）拒绝创建，返回 none */
Value lumyr_instance_new(RuntimeTypeInfo* info);

/* 浅拷贝实例（struct 值语义用） */
Value lumyr_instance_copy(Value obj);

/* 实例相等比较（按字段逐个比较） */
int lumyr_instance_eq(Value a, Value b);

/* 获取实例的类型名（通过偏移 0 的 info 指针） */
const char* lumyr_instance_get_name(Value obj);

/* 获取实例的 RuntimeTypeInfo 指针 */
RuntimeTypeInfo* lumyr_instance_get_info(Value obj);

/* CC 模式字段读写（VM 模式直接用 FieldInfo 做 typed 读写，不走这里） */
Value lumyr_field_get(Value obj, const char* field_name);
void lumyr_field_set(Value obj, const char* field_name, Value value);
/* 受信写入（check_access=0 时绕过访问修饰符检查）：供反序列化恢复 private 字段 */
void lumyr_field_set_trusted(Value obj, const char* field_name, Value value, int check_access);

/* 类型判断：obj is TypeName（沿 parent 链 + interfaces 查） */
int lumyr_type_is(Value obj, const char* type_name);

/* 接口判断 */
int lumyr_type_implements(RuntimeTypeInfo* info, const char* interface_name);

/* 方法分派（CC 模式用）
 * VM 模式用 OPC_CALL_METHOD 直接查 method_names + 调用 */
Value lumyr_type_call_method(Value obj, const char* method_name,
                             int argc, Value* args,
                             struct EvalCtx* ctx, struct StackFrame* frame);

/* 访问修饰符运行时检查（动态路径 INDEX_GET 用） */
void lumyr_set_current_class(const char* class_name);
const char* lumyr_get_current_class(void);
int lumyr_is_accessor_inside_class(const char* class_name);
int lumyr_is_accessor_subclass_of(const char* class_name);

#ifdef __cplusplus
}
#endif

#endif /* LUMYR_RUNTIME_LM_TYPE_H */
