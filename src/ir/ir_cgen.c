/*
 * ir_cgen.c - C 代码生成器（已清空，待重构）
 *
 * 当前状态：VM 通道优先，C 通道待重构对齐新栈设计后重写。
 * 此文件仅保留接口空实现，保证编译通过。
 */

#include "ir_cgen.h"
#include <stdio.h>

void ir_cgen_file(const char* out_c_path, BytecodeFunc* main_fn) {
    (void)main_fn;
    FILE* out = fopen(out_c_path, "w");
    if(!out) return;
    fprintf(out, "/* C 代码生成器待重构 */\n");
    fprintf(out, "int main(void) { return 0; }\n");
    fclose(out);
}

/* FFI 声明列表（暂为空） */
void ffi_decl_add(const char* name, const char* libname, int ret_type, int* param_types, int param_count) {
    (void)name; (void)libname; (void)ret_type; (void)param_types; (void)param_count;
}
int ffi_decl_count(void) { return 0; }
FFIDecl* ffi_decl_get(int idx) { (void)idx; return NULL; }
