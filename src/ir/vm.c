/*
 * vm.c - VM 解释器主入口（已清空，待重构）
 *
 * 当前状态：VM 通道待重构对齐新栈设计后重写。
 * 此文件仅保留接口空实现，保证编译通过。
 */

#include "vm.h"
#include <stdio.h>

Value vm_run_main(BytecodeFunc* main_fn) {
    (void)main_fn;
    fprintf(stderr, "VM 通道待重构\n");
    return val_none();
}

Value vm_func_entry(int arg_cnt, const Value* args, EvalCtx* ctx, StackFrame* frame) {
    (void)arg_cnt; (void)args; (void)ctx; (void)frame;
    return val_none();
}
