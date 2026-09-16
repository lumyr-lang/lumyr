#ifndef LUMYR_VM_INTERNAL_H
#define LUMYR_VM_INTERNAL_H

#include "bytecode.h"
#include "lumyr_value_type.h"
#include "ast/stackframe.h"

/*
 * VM 内部函数声明
 *
 * 本头文件声明 VM 内部使用的函数，这些函数不对外暴露，
 * 仅供 vm.c 和 vm_generator.c 等内部模块使用。
 */

/* 虚拟机主循环：执行字节码函数 */
Value vm_run(BytecodeFunc* bf, StackFrame* frame, EvalCtx* ctx);

/* 调用 RuntimeFunc：执行运行时函数 */
Value vm_call_rf(RuntimeFunc* rf, Value* args, int argc, StackFrame* parent, EvalCtx* ctx);

#endif /* LUMYR_VM_INTERNAL_H */
