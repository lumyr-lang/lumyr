#ifndef LUMYR_VM_EXEC_H
#define LUMYR_VM_EXEC_H

#include "bytecode.h"
#include "lumyr_value_type.h"
#include "ast/stackframe.h"

/*
 * VM 执行模块
 *
 * 本模块提供 VM 的核心执行功能，包括字节码指令的执行和内置函数的执行。
 */

/* 虚拟机主循环：执行字节码函数 */
Value vm_run(BytecodeFunc* bf, StackFrame* frame, EvalCtx* ctx);

/* 执行内置指令 */
int vm_exec_builtin(Instruction in, Value* stack, int sp, StackFrame* frame, EvalCtx* ctx);

#endif /* LUMYR_VM_EXEC_H */
