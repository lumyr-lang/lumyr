#ifndef LUMYR_RUNTIME_LM_STRUCT_H
#define LUMYR_RUNTIME_LM_STRUCT_H

#include "lumyr_value_type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* struct 字段类型枚举（复用 class 的字段类型定义，保持一致） */
typedef enum {
    STRUCT_FIELD_INT = 0,      /* 整数类型（long long） */
    STRUCT_FIELD_DOUBLE = 1,   /* 浮点数类型（double） */
    STRUCT_FIELD_STRING = 2,   /* 字符串类型（const char*） */
    STRUCT_FIELD_BOOL = 3,     /* 布尔类型（int） */
    STRUCT_FIELD_PTR = 4,      /* 指针类型（void*，包括 struct/class/array/map/func） */
    STRUCT_FIELD_OTHER = 5     /* 其他类型 */
} StructFieldType;

/* struct 字段信息 */
typedef struct {
    const char* name;       /* 字段名 */
    int offset;             /* 字段在 C 结构体中的偏移量（字节） */
    StructFieldType type;   /* 字段类型 */
    int size;               /* 字段的字节宽度（用于整数类型的精确读取） */
} StructFieldInfo;

/* 注册 struct 信息（在代码生成时调用，把 struct 的字段信息导出到运行时） */
void lumyr_struct_register(const char* struct_name, int nfields, StructFieldInfo* fields, int struct_size);

/* 查找 struct 字段信息（通过 struct 名和字段名） */
StructFieldInfo* lumyr_struct_find_field(const char* struct_name, const char* field_name);

/* 获取 struct 信息（返回字段列表和数量，用于相等比较等） */
int lumyr_struct_get_info(const char* struct_name, StructFieldInfo** out_fields, int* out_nfields);

/* 获取 struct 实例的 struct 名（通过 __structname__ 字段，它是结构体的第一个字段） */
const char* lumyr_struct_get_name(Value obj);

/* struct 属性读取（专门针对 struct 的函数，不依赖通用的 lumyr_index_get） */
Value lumyr_struct_get_field(Value obj, const char* field_name);

/* struct 属性写入（专门针对 struct 的函数） */
void lumyr_struct_set_field(Value obj, const char* field_name, Value value);

/* struct 相等比较（专门针对 struct 的函数，按字段比较） */
int lumyr_struct_eq(Value a, Value b);

/* struct 浅拷贝（专门针对 struct 的函数，用于值传递） */
Value lumyr_struct_shallow_copy(Value obj);

#ifdef __cplusplus
}
#endif

#endif /* LUMYR_RUNTIME_LM_STRUCT_H */
