#ifndef LUMYR_VM_EXEC_GENERATOR_H
#define LUMYR_VM_EXEC_GENERATOR_H

#include "vm_exec_ctx.h"

/*
 * VM 生成器执行模块
 *
 * 本模块提供 VM 中生成器相关指令的执行逻辑，包括 yield 等。
 */

/* 执行生成器相关指令
 * 返回 1 表示指令已处理，0 表示指令不是生成器指令
 */
int vm_exec_generator(VmExecCtx* ctx);

#endif /* LUMYR_VM_EXEC_GENERATOR_H */
