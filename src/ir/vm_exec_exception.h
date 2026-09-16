#ifndef LUMYR_VM_EXEC_EXCEPTION_H
#define LUMYR_VM_EXEC_EXCEPTION_H

#include "vm_exec_ctx.h"

/*
 * VM 异常处理执行模块
 *
 * 本模块提供 VM 中异常处理相关指令的执行逻辑，包括 try-catch-finally、
 * throw、异常获取等。
 */

/* 执行异常处理相关指令
 * 返回 1 表示指令已处理，0 表示指令不是异常处理指令
 */
int vm_exec_exception(VmExecCtx* ctx);

#endif /* LUMYR_VM_EXEC_EXCEPTION_H */
