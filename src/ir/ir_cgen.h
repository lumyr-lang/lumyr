#ifndef LUMYR_IR_CGEN_H
#define LUMYR_IR_CGEN_H

#include "bytecode.h"

// 从 IR（字节码）生成 C 文件（函数 + main），语义与解释器一致
void ir_cgen_file(const char* out_c_path, BytecodeFunc* main_fn);

/* FFI 外部函数声明列表（编译通道用） */
typedef struct {
    char* name;       /* 函数名 */
    char* libname;    /* 库名（NULL = 默认） */
    int ret_type;     /* 返回值类型（ValueType） */
    int param_count;  /* 参数数量 */
    int* param_types; /* 参数类型数组（ValueType） */
} FFIDecl;

void ffi_decl_add(const char* name, const char* libname, int ret_type, int* param_types, int param_count);
int ffi_decl_count(void);
FFIDecl* ffi_decl_get(int idx);

#endif // LUMYR_IR_CGEN_H
