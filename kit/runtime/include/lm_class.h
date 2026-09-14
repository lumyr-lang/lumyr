#ifndef LUMYR_RUNTIME_LM_CLASS_H
#define LUMYR_RUNTIME_LM_CLASS_H

#include "lumyr_value_type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* class 字段类型枚举 */
typedef enum {
    CLASS_FIELD_INT = 0,      /* 整数类型（long long） */
    CLASS_FIELD_DOUBLE = 1,   /* 浮点数类型（double） */
    CLASS_FIELD_STRING = 2,   /* 字符串类型（const char*） */
    CLASS_FIELD_BOOL = 3,     /* 布尔类型（int） */
    CLASS_FIELD_PTR = 4,      /* 指针类型（void*，包括 struct/class/array/map/func） */
    CLASS_FIELD_OTHER = 5     /* 其他类型 */
} ClassFieldType;

/* 访问修饰符枚举 */
typedef enum {
    CLASS_ACCESS_PUBLIC = 0,     /* public（默认） */
    CLASS_ACCESS_PRIVATE = 1,    /* private */
    CLASS_ACCESS_PROTECTED = 2   /* protected */
} ClassAccessModifier;

/* class 字段信息 */
typedef struct {
    const char* name;       /* 字段名 */
    int offset;             /* 字段在 C 结构体中的偏移量（字节） */
    ClassFieldType type;    /* 字段类型 */
    int access_modifier;    /* 访问修饰符（0=public, 1=private, 2=protected） */
} ClassFieldInfo;

/* class 信息 */
typedef struct {
    const char* class_name;     /* class 名 */
    int nfields;                /* 字段数量 */
    ClassFieldInfo* fields;     /* 字段信息数组 */
    void* vtable;               /* vtable 指针（用于方法调用） */
    int ninterfaces;            /* 实现的接口数量 */
    const char** interfaces;    /* 实现的接口名数组 */
} ClassInfo;

/* 注册 class 信息（在代码生成时调用，把 class 的字段信息导出到运行时） */
void lumyr_class_register(const char* class_name, int nfields, ClassFieldInfo* fields, void* vtable, int ninterfaces, const char** interfaces);

/* 查找 class 信息（通过 class 名） */
ClassInfo* lumyr_class_lookup(const char* class_name);

/* 查找 class 字段信息（通过 class 名和字段名） */
ClassFieldInfo* lumyr_class_find_field(const char* class_name, const char* field_name);

/* 获取 class 实例的 class 名（通过 vtable 指针） */
const char* lumyr_class_get_name(Value obj);

/* 判断一个 VAL_STRUCT_PTR 是不是 class 实例（通过 vtable 指针和 class 红黑树判断） */
int lumyr_is_class_instance(Value obj);

/* class 属性读取（专门针对 class 的函数，不依赖通用的 lumyr_index_get） */
Value lumyr_class_get_field(Value obj, const char* field_name);

/* class 属性写入（专门针对 class 的函数） */
void lumyr_class_set_field(Value obj, const char* field_name, Value value);

/* class 方法调用（专门针对 class 的函数，通过 vtable 调用） */
Value lumyr_class_call_method(Value obj, const char* method_name, int argc, Value* args);

/* 判断 class 是否实现了某个接口（包括父类实现的接口） */
int lumyr_class_implements_interface(const char* class_name, const char* interface_name);

/* 判断对象是否实现了某个接口（对象必须是 class 实例） */
int lumyr_obj_implements_interface(Value obj, const char* interface_name);

/* 通用的接口判断函数（可以处理 class 实例和其他类型，用于 CC 模式代码生成） */
int lumyr_implements_interface(Value obj, const char* iface_name);

/* 接口类型转换：检查对象是否实现了接口，如果没有实现则报错，否则返回对象本身 */
Value lumyr_interface_cast(Value obj, const char* iface_name);

#ifdef __cplusplus
}
#endif

#endif /* LUMYR_RUNTIME_LM_CLASS_H */
