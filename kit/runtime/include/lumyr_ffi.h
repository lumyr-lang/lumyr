/*
 * lumyr_ffi.h - FFI (Foreign Function Interface) 外部函数接口
 *
 * 支持动态加载 C 库并调用外部函数。
 * 编译通道：直接生成 extern 声明和 C 函数调用
 * VM 通道：通过动态加载库 + 函数指针调用
 *
 * 支持类型：
 * - void, int, double, bool, string
 * - ptr（指针/句柄/数组/结构体指针，用 int 存储）
 * - callback（回调函数，Lumyr 函数作为 C 函数指针传递）
 *
 * 支持最多 16 个参数
 */
#ifndef LUMYR_FFI_H
#define LUMYR_FFI_H

#include "lm_value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FFI 类型 - 使用 ValueType（已合并） */

#define FFI_MAX_ARGS 16
#define FFI_MAX_CALLBACKS 16

/* FFI 函数签名 */
struct FFIFunc {
    char* name;           /* 函数名 */
    char* libname;        /* 库名（NULL 表示从当前进程符号表查找） */
    ValueType ret_type;     /* 返回值类型 */
    int param_count;      /* 参数数量 */
    ValueType* param_types; /* 参数类型数组 */
    void* func_ptr;       /* 函数指针（运行时解析） */
    void* lib_handle;     /* 库句柄（运行时加载） */
};

/* 加载库（Windows: LoadLibrary, POSIX: dlopen） */
void* lumyr_ffi_load_lib(const char* libname);

/* 获取函数地址（Windows: GetProcAddress, POSIX: dlsym） */
void* lumyr_ffi_get_func(void* lib_handle, const char* funcname);

/* 调用 FFI 函数（支持最多 FFI_MAX_ARGS 个参数） */
Value lumyr_ffi_call(FFIFunc* func, Value* args, int argc);

/* 创建 FFI 函数对象 */
FFIFunc* lumyr_ffi_func_create(const char* name, const char* libname,
                                 ValueType ret_type, ValueType* param_types, int param_count);

/* 释放 FFI 函数对象 */
void lumyr_ffi_func_free(FFIFunc* func);

/* 类型名转换 */
ValueType lumyr_ffi_type_from_name(const char* name);
const char* lumyr_ffi_type_to_name(ValueType type);

/* 获取 C 类型名字符串（用于编译通道生成代码） */
const char* lumyr_ffi_type_to_cname(ValueType type);

/* ==================== 回调函数支持 ==================== */

/* 注册回调函数，返回槽位 ID（作为函数指针传递给 C） */
int lumyr_ffi_register_callback(Value func_val, int param_count);

/* 注销回调函数 */
void lumyr_ffi_unregister_callback(int slot);

/* 调用 Lumyr 回调函数（由 FFI 调用的 C 函数内部触发） */
Value lumyr_ffi_invoke_callback(int slot, Value* args, int argc);

#ifdef __cplusplus
}
#endif

#endif /* LUMYR_FFI_H */
